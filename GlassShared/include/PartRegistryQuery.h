#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace Shared::PartRegistryQuery
{
    [[nodiscard]] std::optional<std::uint32_t> TryReadLocalDword(
        std::wstring_view partSubKey,
        std::wstring_view valueName) noexcept;

    /// RegHelper fallthrough part → global → default. Root\\Active|Inactive — RootSwcaRegistryQuery.
    [[nodiscard]] std::uint32_t ReadEffectiveDword(
        std::wstring_view partSubKey,
        std::wstring_view valueName,
        std::uint32_t defaultValue) noexcept;

    [[nodiscard]] std::uint32_t ReadUiGradientArgb(
        std::wstring_view partSubKey,
        bool darkMode) noexcept;

    [[nodiscard]] std::uint32_t ReadUiBorderArgb(
        std::wstring_view partSubKey,
        bool darkMode) noexcept;

    [[nodiscard]] std::uint32_t ReadUiCornerType(std::wstring_view partSubKey) noexcept;

    /// Root\\CornerType: DWMWCP_ROUND на Win11 (top-level 8px), иначе DONOTROUND.
    [[nodiscard]] std::uint32_t GetDefaultRootCornerType() noexcept;

    [[nodiscard]] bool ReadUiNoBorderColor(std::wstring_view partSubKey) noexcept;
}
