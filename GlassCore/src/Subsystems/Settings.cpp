// GlassCore/src/Subsystems/Settings.cpp
//
// FIX (Bug 3): исправлен цвет эффекта блюра.
//
// Проблема:
//   1. QueryBackdropEffectContext хардкодила gradientColor = 0x00000000.
//      BWM использует дефолт 0x9EDDDDDD (светлая тема) / 0x412B2B2B (тёмная).
//
//   2. ApplyBackdropEffect передавала ARGB-цвет (0xAARRGGBB) напрямую в
//      EffectHelper::SetWindowBackdrop, тогда как ACCENT_POLICY.dwGradientColor
//      ожидает формат ABGR (0xAABBGGRR). R и B перепутаны → цвет неправильный.
//
//   3. QueryBorderContext устанавливала colorUseDefault = false с color = 0 →
//      хук ставил чёрную рамку вместо системного цвета.

#include "CorePch.h"
#include <Subsystems/Settings.h>
#include <Subsystems/RootSwcaGradient.h>
#include <Subsystems/EffectHelper.h>
#include <RegHelper.h>
#include <SystemHelper.h>
#include <ColorUtils.h>
#include <Helpers/ThemeHelper.h>

#include <Helpers/WinApiUtils.h>
#include <RootBackdropPolicy.h>
#include <Helpers/DwmGlassOpacity.h>
#include <RootPolicyQuery.h>
#include <RootSwcaRegistryQuery.h>
#include <EffectTypeQuery.h>
#include <DebugLog.h>
#include <TrayBorderDrawMode.h>
#include <TrayXamlPaint.h>

#include <atomic>
#include <string>
#include <string_view>
using namespace GlassCore::EffectHelper;

namespace GlassCore::Settings
{
    namespace
    {
        [[nodiscard]] bool IsShellTrayWindowForDropEffect(HWND hWnd) noexcept
        {
            if (!hWnd)
                return false;
            wchar_t cls[256]{};
            if (GetClassNameW(hWnd, cls, _countof(cls)) <= 0)
                return false;
            return _wcsicmp(cls, L"Shell_TrayWnd") == 0 || _wcsicmp(cls, L"Shell_SecondaryTrayWnd") == 0;
        }

        std::atomic<unsigned> g_trayBorderApplySeq{ 0 };

        void LogRootGlassDwmSetAttr(
            HWND hWnd,
            DWORD attr,
            DWORD value,
            HRESULT hr,
            const wchar_t* detail) noexcept
        {
            DebugLog(
                DBG_IMPORTANT,
                L"[GlassCore][Root][GlassDWM] DwmSetWindowAttribute hwnd=0x%p attr=%u value=0x%08X hr=0x%08X %s",
                hWnd,
                attr,
                value,
                hr,
                detail ? detail : L"");
        }
    }
    // -------------------------------------------------------------------------
    // ApplyBackdropEffect
    //
    // FIX: конвертируем ARGB (0xAARRGGBB) → ABGR (0xAABBGGRR) перед передачей
    // в SetWindowBackdrop. ACCENT_POLICY.dwGradientColor = 0xAABBGGRR.
    // -------------------------------------------------------------------------
    void ApplyBackdropEffect(HWND hWnd, bool darkMode, const WindowBackdropEffectContext& backdropContext, bool skipNcRendering, bool flyoutBackdrop)
    {
        // Применяем dark mode перед установкой backdrop (как в BWM ApiEx.cpp:45)
        EffectHelper::EnableWindowDarkMode(hWnd, darkMode);

        WindowBackdropEffectContext effectiveBackdrop = backdropContext;
        if (IsShellTrayWindowForDropEffect(hWnd)
            && Shared::TrayBorderDrawMode::ReadFromRegistry() == Shared::TrayBorderDrawMode::Mode::Xaml)
        {
            // XAML border: SWCA accent rim (kAccentBorderAll) конкурирует с BackgroundStroke, особенно при ROUND.
            effectiveBackdrop.enableDropShadow = false;
        }

        const bool logTraySwca = IsShellTrayWindowForDropEffect(hWnd);
        if (logTraySwca)
        {
            DebugLog(
                DBG_IMPORTANT,
                L"[GlassCore][Settings] Tray ApplyBackdropEffect dropShadow=%u effectType=%u xamlBorder=%u",
                effectiveBackdrop.enableDropShadow ? 1u : 0u,
                effectiveBackdrop.effectType,
                Shared::TrayBorderDrawMode::ReadFromRegistry() == Shared::TrayBorderDrawMode::Mode::Xaml ? 1u : 0u);
        }

        EffectType type = EffectType::None;
        // Маппинг выровнен с BWM enum EffectType:
        // 0=None, 1=Transparent, 2=Solid, 3=Blur, 4=AcrylicBlur,
        // 5=ModernAcrylicBlur, 6=Acrylic, 7=Mica, 8=MicaAlt
        switch (effectiveBackdrop.effectType)
        {
        case 1: type = EffectType::Transparent;        break;
        case 2: type = EffectType::Solid;              break;
        case 3: type = EffectType::Blur;               break;
        case 4: type = EffectType::AcrylicBlur;        break;
        case 5: type = EffectType::ModernAcrylicBlur;  break;
        case 6: type = EffectType::Acrylic;            break;
        case 7: type = EffectType::Mica;               break;
        case 8: type = EffectType::MicaAlt;            break;
        default: type = EffectType::None;              break;
        }

        // Конвертируем ARGB → ABGR для ACCENT_POLICY.dwGradientColor
        // gradientColor хранится как ARGB: 0xAARRGGBB; альфа — из самого GradientColor (задаётся в реестре).
        // GlassOpacity НЕ применяется здесь — он влияет только на root-окна (фаза 5).
        DWORD argb = effectiveBackdrop.gradientColor;
        const BYTE  a = static_cast<BYTE>((argb >> 24) & 0xFF);
        const BYTE  r = static_cast<BYTE>((argb >> 16) & 0xFF);
        const BYTE  g = static_cast<BYTE>((argb >> 8) & 0xFF);
        const BYTE  b = static_cast<BYTE>(argb & 0xFF);
        // ACCENT_POLICY: 0xAABBGGRR
        const DWORD accentColor = (static_cast<DWORD>(a) << 24)
            | (static_cast<DWORD>(b) << 16)
            | (static_cast<DWORD>(g) << 8)
            | static_cast<DWORD>(r);

        EffectHelper::SetWindowBackdrop(
            hWnd,
            effectiveBackdrop.enableDropShadow,
            accentColor,
            type,
            skipNcRendering,
            flyoutBackdrop);

        // Применяем DWM transition после установки backdrop (как в BWM ApiEx.cpp:48)
        DwmTransitionOwnedWindow(hWnd, DWMTRANSITION_OWNEDWINDOW_REPOSITION);
    }

    void ApplyBorderEffect(HWND hWnd, bool darkMode, const BorderContext& border)
    {
        // Паритет TranslucentFlyouts ApiEx::ApplyBorderEffect — без TriggerWindowNCRendering (NC только из backdrop ET 6–8).
        EffectHelper::EnableWindowDarkMode(hWnd, darkMode);

        if (Shared::SystemHelper::g_buildNumber >= 22000)
        {
            const bool logTrayBorder = IsShellTrayWindowForDropEffect(hWnd);
            const unsigned seq = logTrayBorder
                ? g_trayBorderApplySeq.fetch_add(1u, std::memory_order_relaxed) + 1u
                : 0u;
            HRESULT cornerHr = S_OK;
            HRESULT borderHr = S_OK;
            COLORREF borderColorRef = 0;

            cornerHr = DwmSetWindowAttribute(hWnd, DWMWA_WINDOW_CORNER_PREFERENCE,
                &border.cornerType, sizeof(border.cornerType));
            if (logTrayBorder)
            {
                DebugLog(
                    DBG_IMPORTANT,
                    L"[GlassCore][Settings] Tray DWM corners seq=%u hwnd=0x%p pref=%u hr=0x%08X",
                    seq,
                    hWnd,
                    static_cast<unsigned>(border.cornerType),
                    cornerHr);
            }
            else
            {
                LogRootGlassDwmSetAttr(
                    hWnd,
                    DWMWA_WINDOW_CORNER_PREFERENCE,
                    static_cast<DWORD>(border.cornerType),
                    cornerHr,
                    L"corner");
            }

            if (border.colorUseNone)
            {
                COLORREF noColor = DWMWA_COLOR_NONE;
                borderColorRef = noColor;
                borderHr = DwmSetWindowAttribute(hWnd, DWMWA_BORDER_COLOR,
                    &noColor, sizeof(noColor));
            }
            else if (!border.colorUseDefault)
            {
                // border.color — ARGB (0xAARRGGBB); DWM ожидает COLORREF (0x00BBGGRR)
                COLORREF cr = RGB(
                    (border.color >> 16) & 0xFF,
                    (border.color >> 8) & 0xFF,
                    border.color & 0xFF);
                borderColorRef = cr;
                borderHr = DwmSetWindowAttribute(hWnd, DWMWA_BORDER_COLOR, &cr, sizeof(cr));
            }
            else
            {
                COLORREF defColor = DWMWA_COLOR_DEFAULT;
                borderColorRef = defColor;
                borderHr = DwmSetWindowAttribute(hWnd, DWMWA_BORDER_COLOR, &defColor, sizeof(defColor));
            }

            if (logTrayBorder)
            {
                DebugLog(
                    DBG_IMPORTANT,
                    L"[GlassCore][Settings] Tray DWM border seq=%u hwnd=0x%p colorRef=0x%08X hr=0x%08X "
                    L"useNone=%u cornerType=%u",
                    seq,
                    hWnd,
                    borderColorRef,
                    borderHr,
                    border.colorUseNone ? 1u : 0u,
                    static_cast<unsigned>(border.cornerType));

                DWORD effectiveCorner = static_cast<DWORD>(DWMWCP_DEFAULT);
                const HRESULT effHr = DwmGetWindowAttribute(
                    hWnd,
                    DWMWA_WINDOW_CORNER_PREFERENCE,
                    &effectiveCorner,
                    sizeof(effectiveCorner));
                DebugLog(
                    DBG_IMPORTANT,
                    L"[GlassCore][Settings] Tray effectiveCornerPref=%u hr=0x%08X seq=%u",
                    effectiveCorner,
                    effHr,
                    seq);
            }
            else
            {
                LogRootGlassDwmSetAttr(
                    hWnd,
                    DWMWA_BORDER_COLOR,
                    static_cast<DWORD>(borderColorRef),
                    borderHr,
                    border.colorUseNone ? L"border=NONE"
                        : (border.colorUseDefault ? L"border=DEFAULT" : L"border=ARGB"));
            }
        }

        DwmTransitionOwnedWindow(hWnd, DWMTRANSITION_OWNEDWINDOW_REPOSITION);
    }

    void ApplyEffect(HWND hWnd, bool darkMode,
        const WindowBackdropEffectContext& backdropContext,
        const BorderContext& border,
        bool skipNcRendering)
    {
        // Как в BWM ApiEx.cpp:13-17 — просто вызываем два метода подряд
        ApplyBackdropEffect(hWnd, darkMode, backdropContext, skipNcRendering);
        ApplyBorderEffect(hWnd, darkMode, border);
    }

    void PreparePopupSwcaBackdropHwnd(HWND hWnd) noexcept
    {
        if (!hWnd || !IsWindow(hWnd))
            return;

        EffectHelper::SetWindowBackdrop(hWnd, FALSE, 0, EffectType::None);
        (void)Shared::WinApiUtils::ExtendFrameIntoEntireClientArea(hWnd, false);
        (void)Shared::WinApiUtils::EnableBlurBehindWindow(hWnd, false);

        if (Shared::SystemHelper::g_buildNumber >= 22000)
        {
            const DWM_SYSTEMBACKDROP_TYPE noneType = DWMSBT_NONE;
            (void)DwmSetWindowAttribute(
                hWnd, DWMWA_SYSTEMBACKDROP_TYPE, &noneType, sizeof(noneType));
            constexpr DWORD kDwmwaCaptionImmersiveCompat = 1029u;
            const DWORD zero = 0;
            (void)DwmSetWindowAttribute(
                hWnd, kDwmwaCaptionImmersiveCompat, &zero, sizeof(zero));
        }

        DebugLog(DBG_IMPORTANT,
            L"[GlassCore][Settings] PreparePopupSwcaBackdropHwnd hwnd=0x%p rootSwca=%d",
            hWnd,
            RootPolicyUsesSwcaBackdrop(false) ? 1 : 0);
    }

    void ApplyPopupSwcaEffect(HWND hWnd, bool darkMode,
        const WindowBackdropEffectContext& backdropContext,
        const BorderContext& border,
        bool skipNcRendering)
    {
        // Паритет TranslucentFlyouts Api::ApplyEffect — без PreparePopupSwcaBackdropHwnd на show flyout.
        ApplyBackdropEffect(hWnd, darkMode, backdropContext, skipNcRendering, true);
        ApplyBorderEffect(hWnd, darkMode, border);
    }

    void ApplyRootWindowGlassEffect(HWND hWnd, bool darkMode)
    {
        WindowBackdropEffectContext backdrop{};
        BorderContext border{};
        const bool visuallyActive = [&]() noexcept -> bool {
            const HWND fg = GetForegroundWindow();
            if (!fg || !hWnd || !IsWindow(hWnd))
                return true;
            const HWND rWnd = GetAncestor(hWnd, GA_ROOT);
            const HWND rFg = GetAncestor(fg, GA_ROOT);
            return rWnd && rWnd == rFg;
        }();
        QueryRootBackdropEffectContext(darkMode, visuallyActive, backdrop);
        QueryRootBorderContext(darkMode, visuallyActive, border);

        // Решение DWM vs SWCA — по RootBackdropMode (0=off, 1=DWM, 2=SWCA).
        const DWORD rootBackdropMode = backdrop.rootBackdropMode;
        const bool useDwm = (rootBackdropMode == kRootBackdropModeDwm);
        const bool useSwca = (rootBackdropMode == kRootBackdropModeSwca);

        DebugLog(DBG_IMPORTANT,
            L"[GlassCore][Backdrop] hwnd=0x%p → %s "
            L"(EffectType=%u RootBackdropMode=%u)",
            hWnd,
            useDwm ? L"DWM" : (useSwca ? L"SWCA" : L"OFF"),
            backdrop.effectType,
            rootBackdropMode);

        EffectHelper::EnableWindowDarkMode(hWnd, darkMode);

        if (useDwm)
        {
            // Сбрасываем SWCA
            EffectHelper::SetWindowBackdrop(hWnd, FALSE, 0, EffectType::None);
            const HRESULT hr = Shared::WinApiUtils::EnableBlurBehindWindow(hWnd, true);
            DebugLog(DBG_IMPORTANT,
                L"[GlassCore][BackDrop] DwmBlurBehind hwnd=0x%p hr=0x%08X",
                hWnd, hr);
        }
        else if (useSwca)
        {
            (void)Shared::WinApiUtils::EnableBlurBehindWindow(hWnd, false);
            (void)Shared::WinApiUtils::ExtendFrameIntoEntireClientArea(hWnd, false);
            WindowBackdropEffectContext mappedBackdrop = backdrop;
            // effectType уже engine-нативный (0-8), маппинг не нужен.
            const bool visuallyActive = [&]() noexcept -> bool {
                const HWND fg = GetForegroundWindow();
                if (!fg)
                    return false;
                const HWND rWnd = GetAncestor(hWnd, GA_ROOT);
                const HWND rFg = GetAncestor(fg, GA_ROOT);
                return rWnd && rWnd == rFg;
            }();
            mappedBackdrop.gradientColor = GlassCore::Root::ReadRootSwcaGradientArgb(visuallyActive);
            ApplyBackdropEffect(hWnd, darkMode, mappedBackdrop);
            DebugLog(
                DBG_IMPORTANT,
                L"[GlassCore][BackDrop] SWCA hwnd=0x%p effectType=%u gradientColor=0x%08X",
                hWnd,
                mappedBackdrop.effectType,
                mappedBackdrop.gradientColor);
        }
        else
        {
            EffectHelper::SetWindowBackdrop(hWnd, FALSE, 0, EffectType::None);
            (void)Shared::WinApiUtils::EnableBlurBehindWindow(hWnd, false);
            (void)Shared::WinApiUtils::ExtendFrameIntoEntireClientArea(hWnd, false);
        }

        ApplyBorderEffect(hWnd, darkMode, border);
        DwmTransitionOwnedWindow(hWnd, DWMTRANSITION_OWNEDWINDOW_REPOSITION);
    }

    bool RootPolicyUsesSwcaBackdrop(bool darkMode)
    {
        (void)darkMode;
        return Shared::RootPolicyQuery::UsesSwcaBackdrop();
    }

    void DropEffect(std::wstring_view part, HWND hWnd)
    {
        const bool trayShellHwnd = IsShellTrayWindowForDropEffect(hWnd);
        const bool trayPart = (part == kTrayRegistryPart) && trayShellHwnd;
        // Win11 Shell_*TrayWnd: ACCENT_DISABLED → чёрная панель.
        // Stock Taskbar / XAML clear — SWCA Transparent + gradient=0 (TaskbarBorders.md).
        const bool win11TrayTransparent =
            trayPart && Shared::SystemHelper::g_buildNumber >= 22000;

        if (win11TrayTransparent)
            EffectHelper::SetWindowBackdrop(hWnd, FALSE, 0, EffectType::Transparent);
        else
            EffectHelper::SetWindowBackdrop(hWnd, FALSE, 0, EffectType::None);

        if (Shared::SystemHelper::g_buildNumber >= 22000)
        {
            // DropDown на Shell_*TrayWnd — top-level shell, не popup: убираем preference скругления
            // и отдаём системный backdrop (как после плана tray detach).
            // Shell tray — top-level, не popup: как для DropDown, так и для Root.
            const bool trayShell =
                (part == L"DropDown" || part == kRootRegistryPart || part == kTrayRegistryPart)
                && trayShellHwnd;
            // BWM: Menu → ROUNDSMALL, все остальные → DEFAULT.
            // Win11 tray Transparent: не перетирать SYSTEMBACKDROP_TYPE в AUTO после SWCA clear.
            DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_DEFAULT;
            if (trayShell && !win11TrayTransparent)
            {
                constexpr DWORD kDwmwaSystemBackdropType = 38;
                const DWM_SYSTEMBACKDROP_TYPE autoBackdrop = DWMSBT_AUTO;
                DwmSetWindowAttribute(hWnd, kDwmwaSystemBackdropType,
                    &autoBackdrop, sizeof(autoBackdrop));
            }
            else if (part == L"Menu")
            {
                corner = DWMWCP_ROUNDSMALL;
            }
            DwmSetWindowAttribute(hWnd, DWMWA_WINDOW_CORNER_PREFERENCE,
                &corner, sizeof(corner));

            COLORREF noColor = DWMWA_COLOR_DEFAULT;
            DwmSetWindowAttribute(hWnd, DWMWA_BORDER_COLOR, &noColor, sizeof(noColor));
        }

        EffectHelper::EnableWindowDarkMode(hWnd, FALSE);
        EffectHelper::TriggerWindowNCRendering(hWnd);
        // Win11 explorer shell: RDW_ERASE на корне заливает GDI-фоном WinUI-мост шапки (кнопки min/max/close).
        const bool skipClientErase =
            part == kRootRegistryPart
            && Shared::SystemHelper::g_buildNumber >= 22000
            && Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(hWnd);
        if (!skipClientErase)
            InvalidateRect(hWnd, nullptr, TRUE);
    }

    // -------------------------------------------------------------------------
    // QueryBackdropEffectContext
    //
    // gradientColor: для popups / частей кроме Root — BWM-дефолты и LightMode_/DarkMode_GradientColor.
    // Для kRootRegistryPart: gradientColor вне этого запроса; EffectType — в QueryRootBackdropEffectContext (Root\Active|Inactive).
    // Формат ARGB (0xAARRGGBB).
    // -------------------------------------------------------------------------
    void QueryBackdropEffectContext(std::wstring_view part, bool darkMode,
        WindowBackdropEffectContext& context)
    {
        context.rootBackdropMode = kRootBackdropModeSwca;

        // BWM-совместимые дефолты (ARGB 0xAARRGGBB) — popups и прочие part.
        constexpr DWORD kLightGradient = 0x9EDDDDDD;
        constexpr DWORD kDarkGradient = 0x412B2B2B;

        if (part == kRootRegistryPart)
        {
            // EffectType задаётся только в QueryRootBackdropEffectContext (Active/Inactive).
            context.effectType = 4;
        }
        else
        {
            // BWM ApiEx::QueryBackdropEffectContext: дефолт EffectHelper::ModernAcrylicBlur (=5), не AcrylicBlur.
            context.effectType = Shared::EffectTypeQuery::ReadEffectiveEffectType(part);
        }

        context.enableDropShadow = Shared::RegHelper::Get<DWORD>(
            { part, L"" }, L"EnableDropShadow", 0, 1);

        if (part == kRootRegistryPart)
        {
            context.gradientColor = 0;
        }
        else if (darkMode)
        {
            context.gradientColor = Shared::RegHelper::Get<DWORD>(
                { part, L"" }, L"DarkMode_GradientColor", kDarkGradient, 1);
        }
        else
        {
            context.gradientColor = Shared::RegHelper::Get<DWORD>(
                { part, L"" }, L"LightMode_GradientColor", kLightGradient, 1);
        }
        if (part != kRootRegistryPart)
        {
            /*DebugLog(DBG_IMPORTANT,
                L"[GlassCore][Settings] QueryBackdropEffectContext: part=%s darkMode=%d "
                L"effectType=%u enableDropShadow=%u gradientColor=0x%08X",
                part.data(), darkMode, context.effectType, context.enableDropShadow, context.gradientColor);*/
        }
    }

    // -------------------------------------------------------------------------
    // QueryBorderContext — паритет BWM Api::QueryBorderContext
    // -------------------------------------------------------------------------
    void QueryBorderContext(std::wstring_view part, bool darkMode, BorderContext& context)
    {
        context.colorUseDefault = true;
        context.colorUseNone = Shared::RegHelper::Get<DWORD>(
            { part, L"" }, L"NoBorderColor", 0, 1) != 0;

        if (!context.colorUseNone)
        {
            const bool enableColorization = Shared::RegHelper::Get<DWORD>(
                { part, L"" }, L"EnableThemeColorization", 0, 1) != 0;
            if (enableColorization)
            {
                const std::wstring colorizationType = Shared::RegHelper::Get<std::wstring>(
                    { part, L"" },
                    darkMode ? L"DarkMode_ThemeColorizationType" : L"LightMode_ThemeColorizationType",
                    L"ImmersiveStartHoverBackground",
                    1);
                context.color = Shared::ThemeHelper::GetThemeColorizationColor(colorizationType);
            }

            std::optional<DWORD> borderColor;
            if (darkMode)
                borderColor = Shared::RegHelper::TryGet<DWORD>({ part, L"" }, L"DarkMode_BorderColor", 1);
            else
                borderColor = Shared::RegHelper::TryGet<DWORD>({ part, L"" }, L"LightMode_BorderColor", 1);

            if (borderColor.has_value())
            {
                context.color = borderColor.value();
                context.colorUseDefault = false;
            }
            else
            {
                context.colorUseDefault = enableColorization ? false : true;
            }
        }
        else
        {
            context.color = DWMWA_COLOR_NONE;
            context.colorUseDefault = false;
        }

        if (Shared::SystemHelper::g_buildNumber >= 22000)
        {
            const DWORD cornerDefault = (part == kRootRegistryPart || part == kTrayRegistryPart)
                ? static_cast<DWORD>(DWMWCP_ROUND)
                : static_cast<DWORD>(DWMWCP_ROUNDSMALL);
            context.cornerType = static_cast<DWM_WINDOW_CORNER_PREFERENCE>(
                Shared::RegHelper::Get<DWORD>(
                    { part, L"" }, L"CornerType",
                    cornerDefault,
                    1));
        }
        else
        {
            context.cornerType = DWMWCP_DONOTROUND;
        }
    }

    void QueryRootBackdropEffectContext(bool darkMode, bool visuallyActive, WindowBackdropEffectContext& context)
    {
        Shared::RootSwcaRegistryQuery::Snapshot snap{};
        Shared::RootSwcaRegistryQuery::QuerySnapshot(darkMode, visuallyActive, snap);

        context.effectType = snap.effectType;
        context.enableDropShadow = 0;
        context.gradientColor = snap.gradientColorArgb;
        context.rootBackdropMode = kRootBackdropModeSwca;

        if (Shared::RootPolicyQuery::ReadNoBackdropFromRegistry() != 0)
        {
            context.rootBackdropMode = kRootBackdropModeOff;
            return;
        }

        context.rootBackdropMode = Shared::RootPolicyQuery::ReadRootBackdropMode();
    }

    void QueryRootBorderContext(bool darkMode, bool visuallyActive, BorderContext& context)
    {
        // NoBorderColor / CornerType — из Root\ (fallthrough OK).
        QueryBorderContext(kRootRegistryPart, darkMode, context);

        if (context.colorUseNone)
            return;

        // Сброс color-path QueryBorderContext (fallthrough / EnableThemeColorization).
        context.color = 0;
        context.colorUseDefault = true;

        // Цвет бордюра: только точный ключ Active|Inactive (Inactive → Root\ parent, без Active).
        if (const auto borderColor =
                Shared::RootSwcaRegistryQuery::TryReadRootBorderColorArgb(darkMode, visuallyActive))
        {
            context.color = borderColor.value();
            context.colorUseDefault = false;
        }
    }

    void QueryTrayBackdropEffectContext(bool darkMode, WindowBackdropEffectContext& context)
    {
        QueryBackdropEffectContext(kTrayRegistryPart, darkMode, context);
        context.rootBackdropMode = kRootBackdropModeSwca;

        // XAML paint path: HWND полностью clear — фон/зоны рисует TaskbarCompat на острове.
        // gradientColor=0 обязателен: Transparent + ненулевой тинт = сплошная подложка под зонами.
        if (Shared::TrayBorderDrawMode::ReadFromRegistry() == Shared::TrayBorderDrawMode::Mode::Xaml)
        {
            context.effectType = 1; // Transparent (SWCA Clear)
            context.gradientColor = 0;
            context.enableDropShadow = false;
            DebugLog(
                DBG_IMPORTANT,
                L"[GlassCore][Settings] Tray XAML paint path — force SWCA Transparent grad=0");
        }
    }

    void QueryTrayBorderContext(bool darkMode, BorderContext& context)
    {
        QueryBorderContext(kTrayRegistryPart, darkMode, context);

        if (!context.colorUseNone)
        {
            // Tray: только точный ключ Tray\LightMode_* / DarkMode_* (maxFallThrough=0).
            // QueryBorderContext использует fallthrough=1 и может подтянуть Root\ — на live-reload
            // это даёт системный/default DWM-бордюр вместо override из Tray\.
            const wchar_t* const valueName =
                darkMode ? L"DarkMode_BorderColor" : L"LightMode_BorderColor";
            if (const auto exact = Shared::RegHelper::TryGet<DWORD>(
                    { kTrayRegistryPart, L"" }, valueName, 0))
            {
                context.color = exact.value();
                context.colorUseDefault = false;
            }
            else if (!Shared::RegHelper::Get<DWORD>(
                         { kTrayRegistryPart, L"" }, L"EnableThemeColorization", 0, 1))
            {
                context.colorUseDefault = true;
            }
        }

        const auto drawMode = Shared::TrayBorderDrawMode::ReadFromRegistry();
        if (drawMode == Shared::TrayBorderDrawMode::Mode::Xaml)
        {
            // Бордюр и углы HWND — на XAML-пути; DWM отключаем.
            context.colorUseNone = true;
            context.colorUseDefault = false;
            context.color = 0;
            context.cornerType = DWMWCP_DONOTROUND;
            DebugLog(
                DBG_IMPORTANT,
                L"[GlassCore][Settings] Tray XAML paint path — DWM border NONE, corner DONOTROUND");
        }
    }

    void QueryMenuCustomRenderingContext(bool darkMode, WndCompat::Menu::MenuCustomRendering& context)
    {
        context = {};

        context.enable = Shared::RegHelper::Get<DWORD>(
            { L"Menu" }, L"EnableCustomRendering", 0) != 0;
        if (!context.enable)
            return;

        context.separator_disabled = Shared::RegHelper::Get<DWORD>(
            { L"Separator", L"Menu" }, L"Disabled", 0) != 0;
        if (!context.separator_disabled)
        {
            context.separator_width = Shared::RegHelper::Get<DWORD>(
                { L"Separator", L"Menu" }, L"Width", 1000);
            if (darkMode)
            {
                context.separator_color = Shared::RegHelper::Get<DWORD>(
                    { L"Separator", L"Menu" }, L"DarkMode_Color", 0x30D9D9D9);
            }
            else
            {
                context.separator_color = Shared::RegHelper::Get<DWORD>(
                    { L"Separator", L"Menu" }, L"LightMode_Color", 0x30262626);
            }
            if (Shared::RegHelper::Get<DWORD>(
                    { L"Separator", L"Menu" }, L"EnableThemeColorization", 0) != 0)
            {
                const auto colorizationType = Shared::RegHelper::Get<std::wstring>(
                    { L"Separator", L"Menu" },
                    darkMode ? L"DarkMode_ThemeColorizationType" : L"LightMode_ThemeColorizationType",
                    L"ImmersiveStartHoverBackground");
                context.separator_color =
                    Shared::ThemeHelper::GetThemeColorizationColor(colorizationType);
            }
        }

        context.focusing_disabled = Shared::RegHelper::Get<DWORD>(
            { L"Focusing", L"Menu" }, L"Disabled", 0) != 0;
        if (!context.focusing_disabled)
        {
            context.focusing_cornerRadius = Shared::RegHelper::Get<DWORD>(
                { L"Focusing", L"Menu" }, L"CornerRadius", 8);
            context.focusing_width = Shared::RegHelper::Get<DWORD>(
                { L"Focusing", L"Menu" }, L"Width", 1000);
            if (darkMode)
            {
                context.focusing_color = Shared::RegHelper::Get<DWORD>(
                    { L"Focusing", L"Menu" }, L"DarkMode_Color", 0xFFFFFFFF);
            }
            else
            {
                context.focusing_color = Shared::RegHelper::Get<DWORD>(
                    { L"Focusing", L"Menu" }, L"LightMode_Color", 0xFF000000);
            }
            if (Shared::RegHelper::Get<DWORD>(
                    { L"Focusing", L"Menu" }, L"EnableThemeColorization", 0) != 0)
            {
                const auto colorizationType = Shared::RegHelper::Get<std::wstring>(
                    { L"Focusing", L"Menu" },
                    darkMode ? L"DarkMode_ThemeColorizationType" : L"LightMode_ThemeColorizationType",
                    L"ImmersiveStartHoverBackground");
                context.focusing_color =
                    Shared::ThemeHelper::GetThemeColorizationColor(colorizationType);
            }
        }

        context.disabledHot_disabled = Shared::RegHelper::Get<DWORD>(
            { L"DisabledHot", L"Menu" }, L"Disabled", 0) != 0;
        if (!context.disabledHot_disabled)
        {
            context.disabledHot_cornerRadius = Shared::RegHelper::Get<DWORD>(
                { L"DisabledHot", L"Menu" }, L"CornerRadius", 8);
            context.disabledHot_color = Shared::RegHelper::Get<DWORD>(
                { L"DisabledHot", L"Menu" },
                darkMode ? L"DarkMode_Color" : L"LightMode_Color",
                0x00000000);
            if (Shared::RegHelper::Get<DWORD>(
                    { L"DisabledHot", L"Menu" }, L"EnableThemeColorization", 0) != 0)
            {
                const auto colorizationType = Shared::RegHelper::Get<std::wstring>(
                    { L"DisabledHot", L"Menu" },
                    darkMode ? L"DarkMode_ThemeColorizationType" : L"LightMode_ThemeColorizationType",
                    L"ImmersiveStartHoverBackground");
                context.disabledHot_color =
                    Shared::ThemeHelper::GetThemeColorizationColor(colorizationType);
            }
        }

        context.hot_disabled = Shared::RegHelper::Get<DWORD>(
            { L"Hot", L"Menu" }, L"Disabled", 0) != 0;
        if (!context.hot_disabled)
        {
            context.hot_cornerRadius = Shared::RegHelper::Get<DWORD>(
                { L"Hot", L"Menu" }, L"CornerRadius", 8);
            if (darkMode)
            {
                context.hot_color = Shared::RegHelper::Get<DWORD>(
                    { L"Hot", L"Menu" }, L"DarkMode_Color", 0x41808080);
            }
            else
            {
                context.hot_color = Shared::RegHelper::Get<DWORD>(
                    { L"Hot", L"Menu" }, L"LightMode_Color", 0x30000000);
            }
            if (Shared::RegHelper::Get<DWORD>(
                    { L"Hot", L"Menu" }, L"EnableThemeColorization", 0) != 0)
            {
                const auto colorizationType = Shared::RegHelper::Get<std::wstring>(
                    { L"Hot", L"Menu" },
                    darkMode ? L"DarkMode_ThemeColorizationType" : L"LightMode_ThemeColorizationType",
                    L"ImmersiveStartHoverBackground");
                context.hot_color =
                    Shared::ThemeHelper::GetThemeColorizationColor(colorizationType);
            }
        }

        DebugLog(DBG_IMPORTANT, L"[GlassCore][Settings] QueryMenuCustomRenderingContext: enable=%d dark=%d sep_disabled=%d sep_width=%u sep_color=0x%08X",
            context.enable, darkMode, context.separator_disabled, context.separator_width, context.separator_color);
    }

    void QueryIconBackgroundColorRemovalContext(std::vector<std::wstring_view> keyTree, WndCompat::Menu::IconBackgroundColorRemoval& context)
    {
        RtlSecureZeroMemory(&context, sizeof(context));

        // BWM ApiEx: presence ColorTreatAsTransparent = enable.
        const auto colorOpt = Shared::RegHelper::TryGet<DWORD>(
            keyTree, L"ColorTreatAsTransparent");
        if (!colorOpt.has_value())
            return;

        context.enable = true;
        context.colorTreatAsTransparent = colorOpt.value();
        context.colorTreatAsTransparentThreshold = Shared::RegHelper::Get<DWORD>(
            keyTree, L"ColorTreatAsTransparentThreshold", 50);
    }

    void QueryMenuIconBackgroundColorRemovalContext(WndCompat::Menu::IconBackgroundColorRemoval& context)
    {
        // Миграция легаси-ключей не выполняется (приложение не было публичным).
        QueryIconBackgroundColorRemovalContext({ L"Menu" }, context);
    }

    void QueryFlyoutAnimationContext(std::wstring_view part, FlyoutAnimationContext& context)
    {
        RtlSecureZeroMemory(&context, sizeof(context));

        // Как FlyoutControls + Registry::ReadBwmDwordFb:
        // значения в HKCU\BWM\<part>\Animation; при отсутствии — в корне BWM (общие).
        std::wstring animKey;
        animKey.assign(part);
        animKey.append(L"\\Animation");
        const std::wstring_view animPath{ animKey };

        constexpr size_t kAnimFb = 1;
        // BWM ApiEx: …\<part>\EnableFluentAnimation.
        // Один сегмент + maxFallThrough=0 — без чтения из «голого» корня BWM.
        {
            const auto enAnim = Shared::RegHelper::TryGet<DWORD>(
                { std::wstring_view(animKey) }, L"EnableFluentAnimation", 0);
            const auto enPart = Shared::RegHelper::TryGet<DWORD>({ part }, L"EnableFluentAnimation", 0);
            const DWORD enableDw = enAnim.has_value() ? enAnim.value()
                : (enPart.has_value() ? enPart.value() : 0);
            context.enable = (enableDw != 0);
        }

        context.fadeOutTime = Shared::RegHelper::Get<DWORD>(
            { animPath, L"" }, L"FadeOutTime", 350, kAnimFb);

        {
            std::optional<DWORD> imm = Shared::RegHelper::TryGet<DWORD>(
                { animPath, L"" }, L"ImmediateInterrupting", kAnimFb);
            if (!imm)
                imm = Shared::RegHelper::TryGet<DWORD>(
                    { animPath, L"" }, L"ImmediateInterupting", kAnimFb);
            if (!imm)
                imm = Shared::RegHelper::TryGet<DWORD>(
                    { animPath, L"" }, L"EnableImmediateInterupting", kAnimFb);
            context.immediateInterupting = (imm.value_or(0) != 0);
        }

        context.startRatio = Shared::RegHelper::Get<DWORD>(
            { animPath, L"" }, L"StartRatio", 50, kAnimFb);

        context.popInTime = Shared::RegHelper::Get<DWORD>(
            { animPath, L"" }, L"PopInTime", 250, kAnimFb);

        context.fadeInTime = Shared::RegHelper::Get<DWORD>(
            { animPath, L"" }, L"FadeInTime", 87, kAnimFb);

        context.popInStyle = Shared::RegHelper::Get<DWORD>(
            { animPath, L"" }, L"PopInStyle", 0, kAnimFb);
    }

    void QueryTooltipRenderingContext(TooltipRenderingContext& context, bool darkMode)
    {
        constexpr DWORD darkModeColorArgb{ 0xFFFFFFFF };
        constexpr DWORD lightModeColorArgb{ 0xFF1A1A1A };

        const DWORD argb = darkMode
            ? Shared::RegHelper::Get<DWORD>({ L"Tooltip" }, L"DarkMode_Color", darkModeColorArgb)
            : Shared::RegHelper::Get<DWORD>({ L"Tooltip" }, L"LightMode_Color", lightModeColorArgb);

        context.color = Shared::ColorUtils::ARGBToCOLORREF(argb);

        context.marginsType = Shared::RegHelper::Get<DWORD>({ L"Tooltip" }, L"MarginsType", 0);

        context.margins.cxLeftWidth = static_cast<int>(Shared::RegHelper::Get<DWORD>(
            { L"Tooltip" }, L"Margins_cxLeftWidth", 6));
        context.margins.cxRightWidth = static_cast<int>(Shared::RegHelper::Get<DWORD>(
            { L"Tooltip" }, L"Margins_cxRightWidth", 6));
        context.margins.cyTopHeight = static_cast<int>(Shared::RegHelper::Get<DWORD>(
            { L"Tooltip" }, L"Margins_cyTopHeight", 6));
        context.margins.cyBottomHeight = static_cast<int>(Shared::RegHelper::Get<DWORD>(
            { L"Tooltip" }, L"Margins_cyBottomHeight", 6));
    }

    bool IsTooltipCustomizationDisabled() noexcept
    {
        if (Shared::ThemeHelper::IsHighContrastActive())
            return true;
        if (GetSystemMetrics(SM_CLEANBOOT))
            return true;

        SYSTEM_POWER_STATUS powerStatus{};
        if (GetSystemPowerStatus(&powerStatus) && powerStatus.SystemStatusFlag)
            return true;

        static const std::wstring s_exeFileName = []
        {
            std::wstring path;
            path.resize(MAX_PATH);
            const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
            if (!n || n >= path.size())
                return std::wstring{};
            path.resize(n);
            const size_t slash = path.find_last_of(L'\\');
            if (slash != std::wstring::npos && slash + 1 < path.size())
                path.erase(0, slash + 1);
            return path;
        }();

        if (Shared::RegHelper::Get<DWORD>({ L"Tooltip", L"" }, L"Disabled", 0, 1) != 0)
            return true;

        if (!s_exeFileName.empty())
        {
            if (Shared::RegHelper::Get<DWORD>({ L"DisabledList", L"Tooltip" }, s_exeFileName, 0, 1) != 0)
                return true;
            if (Shared::RegHelper::Get<DWORD>({ L"DisabledList" }, s_exeFileName, 0, 1) != 0)
                return true;
        }

        return false;
    }

    bool IsComboBoxAnimation()
    {
        return false;
    }
}