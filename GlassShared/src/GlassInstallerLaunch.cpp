#include "SharedPch.h"
#include "GlassInstallerLaunch.h"

#include <wil/registry.h>
#include <objbase.h>

namespace Shared::GlassInstallerLaunch {

namespace {

constexpr wchar_t kKeyPath[] = L"Software\\StanislavKa\\GlassPlatform\\Installer";
constexpr wchar_t kValueTicket[] = L"MaintenanceTicket";
constexpr wchar_t kValueTicketExpiry[] = L"MaintenanceTicketExpiryUtc";
constexpr wchar_t kValueEulaAccepted[] = L"EulaAccepted";
constexpr ULONGLONG kTicketLifetime100Ns = 120ULL * 10'000'000ULL;

[[nodiscard]] ULONGLONG FileTimeUtcNow() noexcept
{
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

[[nodiscard]] bool OpenInstallerKey(HKEY root, REGSAM access, wil::unique_hkey& key) noexcept
{
    const REGSAM sam = root == HKEY_LOCAL_MACHINE ? (access | KEY_WOW64_64KEY) : access;
    return RegOpenKeyExW(root, kKeyPath, 0, sam, &key) == ERROR_SUCCESS;
}

[[nodiscard]] bool WriteEulaAcceptedTo(HKEY root) noexcept
{
    wil::unique_hkey key;
    const REGSAM sam = KEY_SET_VALUE | (root == HKEY_LOCAL_MACHINE ? KEY_WOW64_64KEY : 0);
    if (RegCreateKeyExW(root, kKeyPath, 0, nullptr, REG_OPTION_NON_VOLATILE, sam, nullptr, &key, nullptr)
        != ERROR_SUCCESS)
        return false;
    const DWORD one = 1;
    return RegSetValueExW(key.get(), kValueEulaAccepted, 0, REG_DWORD,
               reinterpret_cast<const BYTE*>(&one), sizeof(one))
        == ERROR_SUCCESS;
}

} // namespace

bool IssueMaintenanceTicket() noexcept
{
    try {
        wil::unique_hkey key;
        if (RegCreateKeyExW(
                HKEY_LOCAL_MACHINE,
                kKeyPath,
                0,
                nullptr,
                REG_OPTION_NON_VOLATILE,
                KEY_SET_VALUE | KEY_WOW64_64KEY,
                nullptr,
                &key,
                nullptr)
            != ERROR_SUCCESS)
            return false;

        GUID guid{};
        if (FAILED(CoCreateGuid(&guid)))
            return false;
        wchar_t ticket[64]{};
        if (StringFromGUID2(guid, ticket, static_cast<int>(std::size(ticket))) <= 0)
            return false;

        const ULONGLONG expiry = FileTimeUtcNow() + kTicketLifetime100Ns;
        if (RegSetValueExW(
                key.get(),
                kValueTicket,
                0,
                REG_SZ,
                reinterpret_cast<const BYTE*>(ticket),
                static_cast<DWORD>((wcslen(ticket) + 1) * sizeof(wchar_t)))
            != ERROR_SUCCESS)
            return false;
        if (RegSetValueExW(
                key.get(),
                kValueTicketExpiry,
                0,
                REG_QWORD,
                reinterpret_cast<const BYTE*>(&expiry),
                sizeof(expiry))
            != ERROR_SUCCESS)
            return false;
        return true;
    } catch (...) {
        return false;
    }
}

bool TryConsumeMaintenanceTicket() noexcept
{
    try {
        wil::unique_hkey key;
        if (!OpenInstallerKey(HKEY_LOCAL_MACHINE, KEY_READ | KEY_SET_VALUE, key))
            return false;

        wchar_t ticket[64]{};
        DWORD ticketBytes = sizeof(ticket);
        ULONGLONG expiry = 0;
        DWORD expiryBytes = sizeof(expiry);
        if (RegGetValueW(
                key.get(),
                nullptr,
                kValueTicket,
                RRF_RT_REG_SZ,
                nullptr,
                ticket,
                &ticketBytes)
            != ERROR_SUCCESS)
            return false;
        if (RegGetValueW(
                key.get(),
                nullptr,
                kValueTicketExpiry,
                RRF_RT_REG_QWORD,
                nullptr,
                &expiry,
                &expiryBytes)
            != ERROR_SUCCESS)
            return false;
        if (!ticket[0] || FileTimeUtcNow() > expiry)
            return false;

        RegDeleteValueW(key.get(), kValueTicket);
        RegDeleteValueW(key.get(), kValueTicketExpiry);
        return true;
    } catch (...) {
        return false;
    }
}

bool IsEulaAccepted() noexcept
{
    try {
        for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
            wil::unique_hkey key;
            if (!OpenInstallerKey(root, KEY_READ, key))
                continue;
            DWORD accepted = 0;
            DWORD bytes = sizeof(accepted);
            if (RegGetValueW(
                    key.get(),
                    nullptr,
                    kValueEulaAccepted,
                    RRF_RT_REG_DWORD,
                    nullptr,
                    &accepted,
                    &bytes)
                != ERROR_SUCCESS)
                continue;
            if (accepted != 0)
                return true;
        }
        return false;
    } catch (...) {
        return false;
    }
}

bool MarkEulaAccepted() noexcept
{
    try {
        if (WriteEulaAcceptedTo(HKEY_LOCAL_MACHINE))
            return true;
        return WriteEulaAcceptedTo(HKEY_CURRENT_USER);
    } catch (...) {
        return false;
    }
}

} // namespace Shared::GlassInstallerLaunch
