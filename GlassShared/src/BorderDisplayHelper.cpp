#include "SharedPch.h"
#include <Helpers/BorderDisplayHelper.h>
#include <Helpers/ThemeHelper.h>
#include <Helpers/SystemHelper.h>
#include <RegHelper.h>
#include <TrayBorderDrawMode.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <dwmapi.h>
#include <wil/resource.h>

#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "dwmapi.lib")

#ifndef DWMWA_COLOR_DEFAULT
#define DWMWA_COLOR_DEFAULT 0xFFFFFFFEu
#endif

namespace Shared::BorderDisplayHelper
{
    namespace
    {
        [[nodiscard]] const wchar_t* BorderValueName(bool darkMode) noexcept
        {
            return darkMode ? L"DarkMode_BorderColor" : L"LightMode_BorderColor";
        }

        [[nodiscard]] std::optional<DWORD> TryGetBorderOnExactSubKey(
            const wchar_t* bwmRelSubKey, bool darkMode) noexcept
        {
            if (!bwmRelSubKey)
                return std::nullopt;
            return Shared::RegHelper::TryGet<DWORD>(
                { std::wstring_view{ bwmRelSubKey }, L"" },
                BorderValueName(darkMode),
                0);
        }

        [[nodiscard]] std::uint32_t ResolveMenuPopupBorderArgb(bool darkMode) noexcept
        {
            const wchar_t* themeClass = darkMode
                ? L"DarkMode_ImmersiveStart::Menu"
                : L"LightMode_ImmersiveStart::Menu";
            wil::unique_htheme themeHandle{ OpenThemeData(nullptr, themeClass) };
            if (!themeHandle)
                return 0xFF808080u;
            COLORREF color{};
            if (FAILED(GetThemeColor(themeHandle.get(), MENU_POPUPBORDERS, 0, TMT_FILLCOLORHINT, &color)))
                return 0xFF808080u;
            return ColorRefToDisplayArgb(static_cast<unsigned long>(color));
        }

        struct DwmProbeBorderResult
        {
            std::uint32_t         displayArgb = 0xFF808080u;
            std::uint32_t         colorRef    = 0;
            InheritedBorderSource source      = InheritedBorderSource::Hardcoded808080;
        };

        [[nodiscard]] DwmProbeBorderResult ResolveDwmDefaultBorderDetailed() noexcept
        {
            DwmProbeBorderResult result{};
            if (Shared::SystemHelper::g_buildNumber < 22000)
                return result;

            HWND probe = CreateWindowExW(
                WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                L"STATIC",
                nullptr,
                WS_POPUP,
                -32000, -32000, 1, 1,
                nullptr,
                nullptr,
                GetModuleHandleW(nullptr),
                nullptr);
            if (!probe)
                return result;
            const auto destroyProbe = wil::scope_exit([&] {
                if (IsWindow(probe))
                    DestroyWindow(probe);
            });

            COLORREF defColor = DWMWA_COLOR_DEFAULT;
            if (FAILED(DwmSetWindowAttribute(probe, DWMWA_BORDER_COLOR, &defColor, sizeof(defColor))))
                return result;

            COLORREF resolved = 0;
            if (FAILED(DwmGetWindowAttribute(probe, DWMWA_BORDER_COLOR, &resolved, sizeof(resolved))))
                return result;

            result.colorRef    = static_cast<std::uint32_t>(resolved);
            result.displayArgb = ColorRefToDisplayArgb(static_cast<unsigned long>(resolved));
            result.source      = InheritedBorderSource::DwmProbe;
            return result;
        }

        [[nodiscard]] std::uint32_t ResolveDwmDefaultBorderArgb() noexcept
        {
            return ResolveDwmDefaultBorderDetailed().displayArgb;
        }

        constexpr int kChartViewTrayBorderPart  = 2;
        constexpr int kChartViewTrayBorderState = 0;

        [[nodiscard]] InheritedBorderState ResolveChartViewTrayBorder(bool darkMode) noexcept
        {
            InheritedBorderState state{};
            const wchar_t* const themeClassList = darkMode
                ? L"DarkMode::ChartView;ChartView"
                : L"ChartView";
            wil::unique_htheme themeHandle{ OpenThemeData(nullptr, themeClassList) };
            if (!themeHandle)
                return state;

            COLORREF fill{};
            if (FAILED(GetThemeColor(
                    themeHandle.get(),
                    kChartViewTrayBorderPart,
                    kChartViewTrayBorderState,
                    TMT_FILLCOLOR,
                    &fill)))
            {
                return state;
            }

            int alphaLevel = 255;
            if (FAILED(GetThemeInt(
                    themeHandle.get(),
                    kChartViewTrayBorderPart,
                    kChartViewTrayBorderState,
                    TMT_ALPHALEVEL,
                    &alphaLevel)))
            {
                alphaLevel = 255;
            }
            if (alphaLevel < 0)
                alphaLevel = 0;
            else if (alphaLevel > 255)
                alphaLevel = 255;

            state.displayArgb = (static_cast<std::uint32_t>(alphaLevel) << 24)
                | (ColorRefToDisplayArgb(static_cast<unsigned long>(fill)) & 0x00FFFFFFu);
            state.source = InheritedBorderSource::ChartViewTheme;
            return state;
        }

        [[nodiscard]] bool IsTrayXamlBorderDrawMode() noexcept
        {
            return Shared::TrayBorderDrawMode::ReadFromRegistry()
                == Shared::TrayBorderDrawMode::Mode::Xaml;
        }

        // Tray (при BorderDrawMode=XAML), Tray\Xaml и Tray\Xaml\{Popup|…} — ChartView.
        [[nodiscard]] bool IsTrayXamlBorderPart(std::wstring_view part) noexcept
        {
            if (part == L"Tray" || part == L"Tray\\Xaml")
                return true;
            constexpr std::wstring_view kPrefix = L"Tray\\Xaml\\";
            return part.size() > kPrefix.size()
                && part.substr(0, kPrefix.size()) == kPrefix;
        }

        [[nodiscard]] bool ShouldUseChartViewTrayBorder(std::wstring_view part) noexcept
        {
            if (!IsTrayXamlBorderPart(part))
                return false;
            // Tray\Xaml и смежные — всегда ChartView; Tray — только при BorderDrawMode=XAML.
            if (part == L"Tray\\Xaml" || part.starts_with(L"Tray\\Xaml\\"))
                return true;
            return IsTrayXamlBorderDrawMode();
        }

        [[nodiscard]] bool IsFlyoutImmersiveStartPart(std::wstring_view part) noexcept
        {
            return part == L"Menu" || part == L"DropDown" || part == L"Tooltip" || part.empty();
        }

        [[nodiscard]] std::uint32_t ResolveSystemDefaultBorderArgb(
            std::wstring_view part, bool darkMode) noexcept
        {
            if (ShouldUseChartViewTrayBorder(part))
                return ResolveChartViewTrayBorder(darkMode).displayArgb;
            if (part == L"Root")
                return ResolveDwmDefaultBorderArgb();
            if (IsFlyoutImmersiveStartPart(part))
                return ResolveMenuPopupBorderArgb(darkMode);
            return ResolveMenuPopupBorderArgb(darkMode);
        }

        [[nodiscard]] InheritedBorderState BuildInheritedBorder(
            std::wstring_view part, bool darkMode) noexcept
        {
            InheritedBorderState state{};

            state.colorUseNone = Shared::RegHelper::Get<DWORD>(
                { part, L"" }, L"NoBorderColor", 0, 1) != 0;
            if (state.colorUseNone)
            {
                state.source = InheritedBorderSource::None;
                return state;
            }

            const bool enableColorization = Shared::RegHelper::Get<DWORD>(
                { part, L"" }, L"EnableThemeColorization", 0, 1) != 0;
            if (enableColorization)
            {
                const std::wstring colorizationType = Shared::RegHelper::Get<std::wstring>(
                    { part, L"" },
                    darkMode ? L"DarkMode_ThemeColorizationType" : L"LightMode_ThemeColorizationType",
                    L"ImmersiveStartHoverBackground",
                    1);
                state.displayArgb = EnsureOpaqueAlpha(
                    Shared::ThemeHelper::GetThemeColorizationColor(colorizationType));
                state.source = InheritedBorderSource::ThemeColorization;
                return state;
            }

            if (ShouldUseChartViewTrayBorder(part))
            {
                const InheritedBorderState chartView = ResolveChartViewTrayBorder(darkMode);
                if (chartView.source == InheritedBorderSource::ChartViewTheme)
                {
                    state.displayArgb = chartView.displayArgb;
                    state.source      = InheritedBorderSource::ChartViewTheme;
                    return state;
                }
                state.displayArgb = 0xFF808080u;
                state.source      = InheritedBorderSource::Hardcoded808080;
                return state;
            }

            if (part == L"Root")
            {
                const DwmProbeBorderResult dwm = ResolveDwmDefaultBorderDetailed();
                state.displayArgb = dwm.displayArgb;
                state.dwmColorRef = dwm.colorRef;
                state.source      = dwm.source;
                return state;
            }

            state.displayArgb = ResolveMenuPopupBorderArgb(darkMode);
            state.source      = InheritedBorderSource::ThemeColorization;
            return state;
        }

        [[nodiscard]] BorderUiState BuildFromBorderQuery(
            std::wstring_view part,
            bool darkMode,
            const wchar_t* exactSubKeyForPresence) noexcept
        {
            BorderUiState state{};

            state.colorUseNone = Shared::RegHelper::Get<DWORD>(
                { part, L"" }, L"NoBorderColor", 0, 1) != 0;
            if (state.colorUseNone)
            {
                state.displayArgb = 0;
                return state;
            }

            const auto keyOnExact = TryGetBorderOnExactSubKey(exactSubKeyForPresence, darkMode);
            state.registryKeyPresent = keyOnExact.has_value();

            bool colorUseDefault = true;
            std::uint32_t color = 0;

            const bool enableColorization = Shared::RegHelper::Get<DWORD>(
                { part, L"" }, L"EnableThemeColorization", 0, 1) != 0;
            if (enableColorization)
            {
                const std::wstring colorizationType = Shared::RegHelper::Get<std::wstring>(
                    { part, L"" },
                    darkMode ? L"DarkMode_ThemeColorizationType" : L"LightMode_ThemeColorizationType",
                    L"ImmersiveStartHoverBackground",
                    1);
                color = Shared::ThemeHelper::GetThemeColorizationColor(colorizationType);
            }

            std::optional<DWORD> borderColor;
            if (darkMode)
                borderColor = Shared::RegHelper::TryGet<DWORD>({ part, L"" }, L"DarkMode_BorderColor", 1);
            else
                borderColor = Shared::RegHelper::TryGet<DWORD>({ part, L"" }, L"LightMode_BorderColor", 1);

            if (borderColor.has_value())
            {
                color = borderColor.value();
                colorUseDefault = false;
            }
            else
            {
                colorUseDefault = !enableColorization;
            }

            state.usesSystemDefault = colorUseDefault;
            if (colorUseDefault)
                state.displayArgb = ResolveSystemDefaultBorderArgb(part, darkMode);
            else
                state.displayArgb = EnsureOpaqueAlpha(color);
            return state;
        }
    }

    const wchar_t* InheritedBorderSourceName(InheritedBorderSource source) noexcept
    {
        switch (source)
        {
        case InheritedBorderSource::None:
            return L"None";
        case InheritedBorderSource::ThemeColorization:
            return L"ThemeColorization";
        case InheritedBorderSource::ChartViewTheme:
            return L"ChartViewTheme";
        case InheritedBorderSource::DwmProbe:
            return L"DwmProbe";
        case InheritedBorderSource::Hardcoded808080:
            return L"Hardcoded808080";
        }
        return L"?";
    }

    InheritedBorderState QueryInheritedBorderForBwmSubKey(
        const wchar_t* bwmRelSubKey, bool darkMode) noexcept
    {
        std::wstring_view part = (bwmRelSubKey && bwmRelSubKey[0] != L'\0')
            ? std::wstring_view{ bwmRelSubKey }
            : std::wstring_view{};
        return BuildInheritedBorder(part, darkMode);
    }

    InheritedBorderState QueryInheritedForRootSubKey(bool isActive, bool darkMode) noexcept
    {
        (void)isActive;
        return BuildInheritedBorder(L"Root", darkMode);
    }

    std::uint32_t EnsureOpaqueAlpha(std::uint32_t argb) noexcept
    {
        if ((argb >> 24) == 0)
            return (argb & 0x00FFFFFFu) | 0xFF000000u;
        return argb;
    }

    std::uint32_t ColorRefToDisplayArgb(unsigned long colorRef) noexcept
    {
        const BYTE r = GetRValue(colorRef);
        const BYTE g = GetGValue(colorRef);
        const BYTE b = GetBValue(colorRef);
        return 0xFF000000u
            | (static_cast<std::uint32_t>(r) << 16)
            | (static_cast<std::uint32_t>(g) << 8)
            | static_cast<std::uint32_t>(b);
    }

    COLORREF ResolveImmersiveStartFlyoutBorderColorRef(bool darkMode) noexcept
    {
        const std::uint32_t argb = ResolveMenuPopupBorderArgb(darkMode);
        return RGB(
            (argb >> 16) & 0xFF,
            (argb >> 8) & 0xFF,
            argb & 0xFF);
    }

    std::uint32_t ResolveImmersiveStartFlyoutBorderArgb(bool darkMode) noexcept
    {
        return ResolveMenuPopupBorderArgb(darkMode);
    }

    BorderUiState QueryForBwmSubKey(const wchar_t* bwmRelSubKey, bool darkMode) noexcept
    {
        std::wstring_view part = (bwmRelSubKey && bwmRelSubKey[0] != L'\0')
            ? std::wstring_view{ bwmRelSubKey }
            : std::wstring_view{};
        if (part.empty())
        {
            BorderUiState state = BuildFromBorderQuery(L"", darkMode, L"");
            const auto keyRoot = TryGetBorderOnExactSubKey(L"", darkMode);
            state.registryKeyPresent = keyRoot.has_value();
            return state;
        }
        return BuildFromBorderQuery(part, darkMode, bwmRelSubKey);
    }

    BorderUiState QueryForRootSubKey(bool isActive, bool darkMode) noexcept
    {
        const wchar_t* const activeSub = Shared::RegHelper::kBwmRootActivePath.data();
        const wchar_t* const inactiveSub = Shared::RegHelper::kBwmRootInactivePath.data();
        const wchar_t* const subKey = isActive ? activeSub : inactiveSub;

        BorderUiState state = BuildFromBorderQuery(L"Root", darkMode, subKey);

        const auto rootSubColor = Shared::RegHelper::TryGet<DWORD>(
            { subKey }, BorderValueName(darkMode), 0);
        state.registryKeyPresent = rootSubColor.has_value();

        if (rootSubColor.has_value())
        {
            state.usesSystemDefault = false;
            state.displayArgb = EnsureOpaqueAlpha(rootSubColor.value());
        }
        else if (!state.colorUseNone)
        {
            const bool enableColorization = Shared::RegHelper::Get<DWORD>(
                { L"Root", L"" }, L"EnableThemeColorization", 0, 1) != 0;
            if (!enableColorization)
            {
                state.usesSystemDefault = true;
                state.displayArgb = ResolveDwmDefaultBorderArgb();
            }
        }
        return state;
    }
}
