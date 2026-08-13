#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace Shared::EffectTypeQuery
{
    /// Паритет GlassCore::QueryBackdropEffectContext / FlyoutControls kDefEffect.
    constexpr std::uint32_t kDefaultFlyoutEffectType = 5u;
    constexpr std::uint32_t kMaxEffectTypeIndex = 8u;

    [[nodiscard]] std::uint32_t GetDefaultFlyoutEffectType() noexcept;
    [[nodiscard]] std::uint32_t GetDefaultRootEffectType() noexcept;

    /// Только локальный ключ подраздела (без fallback на global / Root Active).
    [[nodiscard]] std::optional<std::uint32_t> TryReadLocalEffectType(
        std::wstring_view partSubKey) noexcept;

    /// Эффективный ET для движка: RegHelper fallthrough part → global → default flyout.
    /// Root\\Active|Inactive — через RootSwcaRegistryQuery.
    [[nodiscard]] std::uint32_t ReadEffectiveEffectType(std::wstring_view partSubKey) noexcept;

    /// Индекс ComboBox 0–8 для GlassSettings UI.
    [[nodiscard]] std::uint32_t ReadUiEffectType(std::wstring_view partSubKey) noexcept;
}
