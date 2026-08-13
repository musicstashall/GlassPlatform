#pragma once

#include <windows.h>
#include <cstdint>

namespace Shared::BorderDisplayHelper
{
    struct BorderUiState
    {
        bool registryKeyPresent = false;
        bool usesSystemDefault = false;
        bool colorUseNone = false;
        std::uint32_t displayArgb = 0;
    };

    enum class InheritedBorderSource : std::uint8_t
    {
        None = 0,
        ThemeColorization,
        ChartViewTheme,
        DwmProbe,
        Hardcoded808080,
    };

    struct InheritedBorderState
    {
        InheritedBorderSource source = InheritedBorderSource::Hardcoded808080;
        bool                  colorUseNone = false;
        std::uint32_t         displayArgb = 0;
        /// COLORREF (0x00BBGGRR) при source=DwmProbe; иначе 0.
        std::uint32_t         dwmColorRef = 0;
    };

    [[nodiscard]] const wchar_t* InheritedBorderSourceName(
        InheritedBorderSource source) noexcept;

    /// Цвет бордюра без per-subkey override (LightMode_BorderColor на sub).
    [[nodiscard]] InheritedBorderState QueryInheritedBorderForBwmSubKey(
        const wchar_t* bwmRelSubKey, bool darkMode) noexcept;

    /// Root SWCA: inherited без ключа на Root\Active|Inactive.
    [[nodiscard]] InheritedBorderState QueryInheritedForRootSubKey(
        bool isActive, bool darkMode) noexcept;

    /// Flyout/part border (Menu, DropDown, Tooltip, Tray, "" global).
    [[nodiscard]] BorderUiState QueryForBwmSubKey(
        const wchar_t* bwmRelSubKey, bool darkMode) noexcept;

    /// Root SWCA: Root\Active or Root\Inactive.
    [[nodiscard]] BorderUiState QueryForRootSubKey(
        bool isActive, bool darkMode) noexcept;

    [[nodiscard]] std::uint32_t EnsureOpaqueAlpha(std::uint32_t argb) noexcept;

    [[nodiscard]] std::uint32_t ColorRefToDisplayArgb(unsigned long colorRef) noexcept;

    /// Default flyout border — ImmersiveStart::Menu (BWM parity).
    [[nodiscard]] COLORREF ResolveImmersiveStartFlyoutBorderColorRef(bool darkMode) noexcept;
    [[nodiscard]] std::uint32_t ResolveImmersiveStartFlyoutBorderArgb(bool darkMode) noexcept;
}
