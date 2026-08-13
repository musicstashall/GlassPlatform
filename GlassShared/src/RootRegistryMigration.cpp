#include "SharedPch.h"
#include <RootRegistryMigration.h>
#include <Helpers/RegHelper.h>

#include <atomic>
#include <string>

namespace Shared::RootRegistryMigration
{
    namespace
    {
        std::atomic<bool> g_migrated{ false };

        [[nodiscard]] bool DeleteRootBackDropTypeValue() noexcept
        {
            std::wstring keyName{ RegHelper::g_regPath };
            keyName += L"\\Root";

            HKEY hKey = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, keyName.c_str(), 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS)
                return false;

            const LSTATUS st = RegDeleteValueW(hKey, L"BackDropType");
            RegCloseKey(hKey);
            return st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND;
        }
    } // namespace

    void MigrateLegacyRootBackDropType() noexcept
    {
        if (g_migrated.exchange(true, std::memory_order_acq_rel))
            return;

        constexpr std::wstring_view kRoot{L"Root"};
        constexpr std::wstring_view kEmpty{L""};

        const auto legacy = RegHelper::TryGet<DWORD>({ kRoot, kEmpty }, L"BackDropType", 1);
        if (!legacy.has_value())
            return;

        if (legacy.value() == 0u)
        {
            std::wstring keyName{ RegHelper::g_regPath };
            keyName += L"\\Root";
            (void)wil::reg::set_value_nothrow<DWORD>(
                HKEY_CURRENT_USER, keyName.c_str(), L"NoBackdrop", 1u);
        }

        (void)DeleteRootBackDropTypeValue();
    }
}
