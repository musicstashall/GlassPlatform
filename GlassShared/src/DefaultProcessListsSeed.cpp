#include "SharedPch.h"

#include "DefaultProcessListsSeed.h"

#include <DefaultProcessLists.h>
#include <InjectionMode.h>

#include <cstdio>

namespace GlassShared::DefaultProcessLists {
namespace {

static_assert(kModeForbidden == static_cast<unsigned>(Shared::InjectionMode::Forbidden));
static_assert(kModePopupOnly == static_cast<unsigned>(Shared::InjectionMode::PopupOnly));
static_assert(kModeFull == static_cast<unsigned>(Shared::InjectionMode::Full));

struct ListProbe
{
    bool hasEntries = false; // хотя бы одно именованное значение; значение по умолчанию не считается
    LSTATUS error = ERROR_SUCCESS;
};

ListProbe ProbeList(const wchar_t* subkey) noexcept
{
    ListProbe probe;
    HKEY key = nullptr;
    const LSTATUS open = RegOpenKeyExW(HKEY_CURRENT_USER, subkey, 0, KEY_QUERY_VALUE, &key);
    if (open == ERROR_FILE_NOT_FOUND)
        return probe;
    if (open != ERROR_SUCCESS)
    {
        probe.error = open;
        return probe;
    }
    wchar_t name[260];
    for (DWORD index = 0;; ++index)
    {
        DWORD cch = static_cast<DWORD>(sizeof(name) / sizeof(name[0]));
        const LSTATUS status = RegEnumValueW(key, index, name, &cch, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS)
            break;
        if (status == ERROR_MORE_DATA || (status == ERROR_SUCCESS && cch > 0))
        {
            probe.hasEntries = true;
            break;
        }
        if (status != ERROR_SUCCESS)
        {
            probe.error = status;
            break;
        }
    }
    RegCloseKey(key);
    return probe;
}

void Describe(SeedResult& result, const wchar_t* list, const wchar_t* api, const wchar_t* name, LSTATUS status) noexcept
{
    result.error = status;
    wchar_t text[512];
    _snwprintf_s(text, _TRUNCATE, L"%s: %s(%s) failed with error %ld", list, api, name ? name : L"", static_cast<long>(status));
    try { result.detail = text; } catch (...) {}
}

LSTATUS WriteDword(HKEY key, const wchar_t* name, DWORD value) noexcept
{
    return RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

} // namespace

bool SeedMissing(SeedResult& result) noexcept
{
    result = SeedResult{};

    {
        const ListProbe probe = ProbeList(kAttachListSubkey);
        if (probe.error != ERROR_SUCCESS)
        {
            Describe(result, L"AttachList", L"RegOpenKeyEx", nullptr, probe.error);
            return false;
        }
        if (!probe.hasEntries)
        {
            HKEY key = nullptr;
            const LSTATUS created = RegCreateKeyExW(HKEY_CURRENT_USER, kAttachListSubkey, 0, nullptr,
                REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr);
            if (created != ERROR_SUCCESS)
            {
                Describe(result, L"AttachList", L"RegCreateKeyEx", nullptr, created);
                return false;
            }
            result.attachCreated = true;
            for (const auto& entry : kAttachList)
            {
                const LSTATUS status = WriteDword(key, entry.exe, static_cast<DWORD>(entry.mode));
                if (status != ERROR_SUCCESS)
                {
                    RegCloseKey(key);
                    Describe(result, L"AttachList", L"RegSetValueEx", entry.exe, status);
                    return false;
                }
                ++result.attachWritten;
            }
            RegCloseKey(key);
        }
    }

    {
        const ListProbe probe = ProbeList(kBlockListSubkey);
        if (probe.error != ERROR_SUCCESS)
        {
            Describe(result, L"BlockList", L"RegOpenKeyEx", nullptr, probe.error);
            return false;
        }
        if (!probe.hasEntries)
        {
            HKEY key = nullptr;
            const LSTATUS created = RegCreateKeyExW(HKEY_CURRENT_USER, kBlockListSubkey, 0, nullptr,
                REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr);
            if (created != ERROR_SUCCESS)
            {
                Describe(result, L"BlockList", L"RegCreateKeyEx", nullptr, created);
                return false;
            }
            result.blockCreated = true;
            const DWORD descriptionBytes =
                static_cast<DWORD>((wcslen(kBlockListDescription) + 1) * sizeof(wchar_t));
            // Пояснение косметическое: сбой его записи не отменяет список.
            (void)RegSetValueExW(key, nullptr, 0, REG_SZ,
                reinterpret_cast<const BYTE*>(kBlockListDescription), descriptionBytes);
            for (const wchar_t* exe : kBlockList)
            {
                const LSTATUS status = WriteDword(key, exe, 1);
                if (status != ERROR_SUCCESS)
                {
                    RegCloseKey(key);
                    Describe(result, L"BlockList", L"RegSetValueEx", exe, status);
                    return false;
                }
                ++result.blockWritten;
            }
            RegCloseKey(key);
        }
    }
    return true;
}

void UndoSeed(const SeedResult& result) noexcept
{
    if (result.attachCreated)
        (void)RegDeleteTreeW(HKEY_CURRENT_USER, kAttachListSubkey);
    if (result.blockCreated)
        (void)RegDeleteTreeW(HKEY_CURRENT_USER, kBlockListSubkey);
}

} // namespace GlassShared::DefaultProcessLists
