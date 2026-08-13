#include "SharedPch.h"
#include <RootSwcaRegistryQuery.h>
#include <Helpers/RegHelper.h>
#include <Helpers/SystemHelper.h>

#include <algorithm>

namespace Shared::RootSwcaRegistryQuery
{
    namespace
    {
        [[nodiscard]] std::wstring_view StateSubkeyPath(bool visuallyActive) noexcept
        {
            return visuallyActive ? RegHelper::kBwmRootActivePath : RegHelper::kBwmRootInactivePath;
        }

        [[nodiscard]] RootSwcaSubkeyRead ReadSubkeyWithRootFallthrough(
            bool visuallyActive,
            std::wstring_view valueName,
            DWORD defaultValue) noexcept
        {
            RootSwcaSubkeyRead out{};
            out.value = defaultValue;
            out.resolvedFromRoot = true;

            if (!UsesPerWindowStateRootParams())
            {
                if (const auto root = RegHelper::TryGet<DWORD>(
                        { RegHelper::kBwmRootPath }, valueName, 0))
                {
                    out.value = root.value();
                    return out;
                }
                return out;
            }

            const auto statePath = StateSubkeyPath(visuallyActive);
            if (const auto state = RegHelper::TryGet<DWORD>({ statePath }, valueName, 0))
            {
                out.value = state.value();
                out.resolvedFromRoot = false;
                return out;
            }

            if (const auto root = RegHelper::TryGet<DWORD>(
                    { RegHelper::kBwmRootPath }, valueName, 0))
            {
                out.value = root.value();
                out.resolvedFromRoot = true;
                return out;
            }

            return out;
        }
    } // namespace

    bool UsesPerWindowStateRootParams() noexcept
    {
        if (const auto flag = RegHelper::TryGet<DWORD>(
                { RegHelper::kBwmRootPath }, L"SeparateActiveInactive", 0))
        {
            return flag.value() != 0;
        }

        if (RegHelper::TryGet<DWORD>({ RegHelper::kBwmRootInactivePath }, L"EffectType", 0))
            return true;

        static constexpr std::wstring_view kProbeNames[] = {
            L"LightMode_GradientColor",
            L"DarkMode_GradientColor",
            L"LightMode_BorderColor",
            L"DarkMode_BorderColor",
            L"GlassDrawThemeConstantAlpha",
        };

        for (const auto name : kProbeNames)
        {
            if (RegHelper::TryGet<DWORD>({ RegHelper::kBwmRootInactivePath }, name, 0))
                return true;
        }

        return false;
    }

    std::optional<DWORD> TryGetRootSwcaSubkey(
        bool visuallyActive, std::wstring_view valueName) noexcept
    {
        if (!UsesPerWindowStateRootParams())
        {
            return RegHelper::TryGet<DWORD>(
                { RegHelper::kBwmRootPath }, valueName, 0);
        }

        const auto statePath = StateSubkeyPath(visuallyActive);
        if (const auto state = RegHelper::TryGet<DWORD>({ statePath }, valueName, 0))
            return state;

        return RegHelper::TryGet<DWORD>(
            { RegHelper::kBwmRootPath }, valueName, 0);
    }

    RootSwcaSubkeyRead ReadRootSwcaSubkey(
        bool visuallyActive, std::wstring_view valueName, DWORD defaultValue) noexcept
    {
        return ReadSubkeyWithRootFallthrough(visuallyActive, valueName, defaultValue);
    }

    DWORD GetRootSwcaSubkey(
        bool visuallyActive, std::wstring_view valueName, DWORD defaultValue) noexcept
    {
        return ReadSubkeyWithRootFallthrough(visuallyActive, valueName, defaultValue).value;
    }

    DWORD GetDefaultRootEffectType() noexcept
    {
        return Shared::SystemHelper::g_buildNumber >= 22000
            ? kDefaultEffectTypeWin11
            : kDefaultEffectTypeWin10;
    }

    DWORD ReadRootEffectType(bool visuallyActive) noexcept
    {
        if (const auto effectType = TryGetRootSwcaSubkey(visuallyActive, L"EffectType"))
        {
            if (effectType.value() <= 8u)
                return effectType.value();
        }
        return GetDefaultRootEffectType();
    }

    DWORD ReadRootGradientArgb(bool darkMode, bool visuallyActive) noexcept
    {
        const wchar_t* const valueName =
            darkMode ? L"DarkMode_GradientColor" : L"LightMode_GradientColor";
        const DWORD defaultGradient =
            darkMode ? kDefaultDarkGradientArgb : kDefaultLightGradientArgb;
        return GetRootSwcaSubkey(visuallyActive, valueName, defaultGradient);
    }

    std::optional<DWORD> TryReadRootBorderColorArgb(
        bool darkMode, bool visuallyActive) noexcept
    {
        const wchar_t* const valueName =
            darkMode ? L"DarkMode_BorderColor" : L"LightMode_BorderColor";
        return TryGetRootSwcaSubkey(visuallyActive, valueName);
    }

    void QuerySnapshot(bool darkMode, bool visuallyActive, Snapshot& out) noexcept
    {
        out.effectType = ReadRootEffectType(visuallyActive);
        out.gradientColorArgb = ReadRootGradientArgb(darkMode, visuallyActive);
        out.borderColorArgb = TryReadRootBorderColorArgb(darkMode, visuallyActive);
    }

    RootSwcaSubkeyRead ReadRootGlassDrawThemeConstantAlphaWithSource(
        bool visuallyActive) noexcept
    {
        return ReadSubkeyWithRootFallthrough(
            visuallyActive,
            L"GlassDrawThemeConstantAlpha",
            static_cast<DWORD>(kDefaultGlassDrawThemeConstantAlpha));
    }

    std::uint8_t ReadRootGlassDrawThemeConstantAlpha(bool visuallyActive) noexcept
    {
        const auto read = ReadRootGlassDrawThemeConstantAlphaWithSource(visuallyActive);
        return static_cast<std::uint8_t>(std::min<DWORD>(read.value, 255u));
    }
}
