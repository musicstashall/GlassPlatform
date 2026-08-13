#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace Shared::RootSwcaRegistryQuery
{
    /// Per-state SWCA-параметры root из Root\Active | Root\Inactive (без Root\ legacy, без animation/dropShadow).
    struct Snapshot
    {
        DWORD effectType{};
        DWORD gradientColorArgb{};
        std::optional<DWORD> borderColorArgb;
    };

    constexpr DWORD kDefaultEffectTypeWin10 = 4u;
    constexpr DWORD kDefaultEffectTypeWin11 = 6u;

    [[nodiscard]] DWORD GetDefaultRootEffectType() noexcept;
    constexpr DWORD kDefaultLightGradientArgb = 0x9EDDDDDDu;
    constexpr DWORD kDefaultDarkGradientArgb = 0x412B2B2Bu;
    constexpr std::uint8_t kDefaultGlassDrawThemeConstantAlpha = 255u;

    /// Root\SeparateActiveInactive: 0 — только Root\; 1 — Active/Inactive → Root\ (fallthrough).
    [[nodiscard]] bool UsesPerWindowStateRootParams() noexcept;

    /// Active/Inactive subkey → Root\ (или только Root\ в режиме без учёта состояния).
    [[nodiscard]] std::optional<DWORD> TryGetRootSwcaSubkey(
        bool visuallyActive, std::wstring_view valueName) noexcept;

    struct RootSwcaSubkeyRead
    {
        DWORD value{};
        bool resolvedFromRoot = false;
    };

    [[nodiscard]] RootSwcaSubkeyRead ReadRootSwcaSubkey(
        bool visuallyActive, std::wstring_view valueName, DWORD defaultValue) noexcept;

    [[nodiscard]] DWORD GetRootSwcaSubkey(
        bool visuallyActive, std::wstring_view valueName, DWORD defaultValue) noexcept;

    void QuerySnapshot(bool darkMode, bool visuallyActive, Snapshot& out) noexcept;

    [[nodiscard]] DWORD ReadRootGradientArgb(bool darkMode, bool visuallyActive) noexcept;
    [[nodiscard]] DWORD ReadRootEffectType(bool visuallyActive) noexcept;

    [[nodiscard]] std::optional<DWORD> TryReadRootBorderColorArgb(
        bool darkMode, bool visuallyActive) noexcept;

    [[nodiscard]] std::uint8_t ReadRootGlassDrawThemeConstantAlpha(bool visuallyActive) noexcept;

    [[nodiscard]] RootSwcaSubkeyRead ReadRootGlassDrawThemeConstantAlphaWithSource(
        bool visuallyActive) noexcept;
}
