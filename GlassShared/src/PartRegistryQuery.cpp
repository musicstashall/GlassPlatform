#include "SharedPch.h"
#include <PartRegistryQuery.h>
#include <Helpers/BorderDisplayHelper.h>
#include <Helpers/RegHelper.h>
#include <Helpers/SystemHelper.h>
#include <RootSwcaRegistryQuery.h>

#include <dwmapi.h>
#include <vector>

namespace Shared::PartRegistryQuery
{
    std::uint32_t GetDefaultRootCornerType() noexcept
    {
        return Shared::SystemHelper::g_buildNumber >= 22000
            ? static_cast<std::uint32_t>(DWMWCP_ROUND)
            : static_cast<std::uint32_t>(DWMWCP_DONOTROUND);
    }

    namespace
    {
        [[nodiscard]] bool IsRootSwcaSubKey(std::wstring_view partSubKey) noexcept
        {
            return partSubKey == RegHelper::kBwmRootActivePath
                || partSubKey == RegHelper::kBwmRootInactivePath;
        }

        [[nodiscard]] std::vector<std::wstring_view> MakeKeyTree(std::wstring_view partSubKey)
        {
            if (partSubKey.empty())
                return {};
            return { partSubKey, L"" };
        }

        [[nodiscard]] size_t FallThroughDepth(std::wstring_view partSubKey) noexcept
        {
            return partSubKey.empty() ? 0u : 1u;
        }

        [[nodiscard]] const wchar_t* GradientValueName(bool darkMode) noexcept
        {
            return darkMode ? L"DarkMode_GradientColor" : L"LightMode_GradientColor";
        }

        [[nodiscard]] std::uint32_t DefaultGradientArgb(bool darkMode) noexcept
        {
            return darkMode
                ? RootSwcaRegistryQuery::kDefaultDarkGradientArgb
                : RootSwcaRegistryQuery::kDefaultLightGradientArgb;
        }

        [[nodiscard]] std::uint32_t DefaultCornerType() noexcept
        {
            return Shared::SystemHelper::g_buildNumber >= 22000
                ? static_cast<std::uint32_t>(DWMWCP_ROUNDSMALL)
                : static_cast<std::uint32_t>(DWMWCP_DONOTROUND);
        }

        [[nodiscard]] std::uint32_t DefaultCornerTypeForPart(std::wstring_view partSubKey) noexcept
        {
            if (partSubKey == RegHelper::kBwmRootPath)
                return GetDefaultRootCornerType();

            return DefaultCornerType();
        }
    } // namespace

    std::optional<std::uint32_t> TryReadLocalDword(
        std::wstring_view partSubKey,
        std::wstring_view valueName) noexcept
    {
        if (IsRootSwcaSubKey(partSubKey))
        {
            const auto& path = (partSubKey == RegHelper::kBwmRootActivePath)
                ? RegHelper::kBwmRootActivePath
                : RegHelper::kBwmRootInactivePath;
            return RegHelper::TryGet<std::uint32_t>({ path }, valueName, 0);
        }

        return RegHelper::TryGet<std::uint32_t>(MakeKeyTree(partSubKey), valueName, 0);
    }

    std::uint32_t ReadEffectiveDword(
        std::wstring_view partSubKey,
        std::wstring_view valueName,
        std::uint32_t defaultValue) noexcept
    {
        if (partSubKey == RegHelper::kBwmRootActivePath)
        {
            return RootSwcaRegistryQuery::GetRootSwcaSubkey(
                true, valueName, defaultValue);
        }

        if (partSubKey == RegHelper::kBwmRootInactivePath)
        {
            return RootSwcaRegistryQuery::GetRootSwcaSubkey(
                false, valueName, defaultValue);
        }

        return RegHelper::Get<std::uint32_t>(
            MakeKeyTree(partSubKey),
            valueName,
            defaultValue,
            FallThroughDepth(partSubKey));
    }

    std::uint32_t ReadUiGradientArgb(std::wstring_view partSubKey, bool darkMode) noexcept
    {
        if (IsRootSwcaSubKey(partSubKey))
        {
            const bool active = (partSubKey == RegHelper::kBwmRootActivePath);
            return RootSwcaRegistryQuery::ReadRootGradientArgb(darkMode, active);
        }

        const wchar_t* const valueName = GradientValueName(darkMode);
        if (const auto local = TryReadLocalDword(partSubKey, valueName))
            return local.value();

        return ReadEffectiveDword(partSubKey, valueName, DefaultGradientArgb(darkMode));
    }

    std::uint32_t ReadUiBorderArgb(std::wstring_view partSubKey, bool darkMode) noexcept
    {
        if (partSubKey == RegHelper::kBwmRootActivePath)
            return BorderDisplayHelper::QueryForRootSubKey(true, darkMode).displayArgb;

        if (partSubKey == RegHelper::kBwmRootInactivePath)
            return BorderDisplayHelper::QueryForRootSubKey(false, darkMode).displayArgb;

        std::wstring key(partSubKey);
        return BorderDisplayHelper::QueryForBwmSubKey(key.c_str(), darkMode).displayArgb;
    }

    std::uint32_t ReadUiCornerType(std::wstring_view partSubKey) noexcept
    {
        if (const auto local = TryReadLocalDword(partSubKey, L"CornerType"))
            return local.value();

        return ReadEffectiveDword(partSubKey, L"CornerType", DefaultCornerTypeForPart(partSubKey));
    }

    bool ReadUiNoBorderColor(std::wstring_view partSubKey) noexcept
    {
        return ReadEffectiveDword(partSubKey, L"NoBorderColor", 0) != 0;
    }
}
