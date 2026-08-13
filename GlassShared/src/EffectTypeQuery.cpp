#include "SharedPch.h"
#include <EffectTypeQuery.h>
#include <PartRegistryQuery.h>
#include <Helpers/RegHelper.h>
#include <RootSwcaRegistryQuery.h>

#include <algorithm>

namespace Shared::EffectTypeQuery
{
    namespace
    {
        [[nodiscard]] std::uint32_t ClampUiIndex(std::uint32_t value) noexcept
        {
            return std::min(value, kMaxEffectTypeIndex);
        }

        [[nodiscard]] bool IsRootSwcaSubKey(std::wstring_view partSubKey) noexcept
        {
            return partSubKey == RegHelper::kBwmRootActivePath
                || partSubKey == RegHelper::kBwmRootInactivePath;
        }
    } // namespace

    std::uint32_t GetDefaultFlyoutEffectType() noexcept
    {
        return kDefaultFlyoutEffectType;
    }

    std::uint32_t GetDefaultRootEffectType() noexcept
    {
        return RootSwcaRegistryQuery::GetDefaultRootEffectType();
    }

    std::optional<std::uint32_t> TryReadLocalEffectType(std::wstring_view partSubKey) noexcept
    {
        return PartRegistryQuery::TryReadLocalDword(partSubKey, L"EffectType");
    }

    std::uint32_t ReadEffectiveEffectType(std::wstring_view partSubKey) noexcept
    {
        if (partSubKey == RegHelper::kBwmRootActivePath)
            return RootSwcaRegistryQuery::ReadRootEffectType(true);

        if (partSubKey == RegHelper::kBwmRootInactivePath)
            return RootSwcaRegistryQuery::ReadRootEffectType(false);

        return PartRegistryQuery::ReadEffectiveDword(
            partSubKey,
            L"EffectType",
            kDefaultFlyoutEffectType);
    }

    std::uint32_t ReadUiEffectType(std::wstring_view partSubKey) noexcept
    {
        if (IsRootSwcaSubKey(partSubKey))
        {
            const bool active = (partSubKey == RegHelper::kBwmRootActivePath);
            return ClampUiIndex(RootSwcaRegistryQuery::ReadRootEffectType(active));
        }

        if (const auto local = TryReadLocalEffectType(partSubKey))
            return ClampUiIndex(local.value());

        return ClampUiIndex(ReadEffectiveEffectType(partSubKey));
    }
}
