#include "GlassUserDefaults.h"
#include <DefaultProcessLists.h>
#include <InjectionMode.h>
#include <cstdio>
#include <cwchar>

namespace GlassUserDefaults {
namespace {

namespace Defaults = GlassShared::DefaultProcessLists;

// The header is data only; make sure its numbers are the platform's modes.
static_assert(Defaults::kModeForbidden == static_cast<unsigned>(Shared::InjectionMode::Forbidden));
static_assert(Defaults::kModePopupOnly == static_cast<unsigned>(Shared::InjectionMode::PopupOnly));
static_assert(Defaults::kModeFull == static_cast<unsigned>(Shared::InjectionMode::Full));

struct ListProbe {
    bool exists = false;
    bool hasEntries = false;   // at least one named value; the default value does not count
    LSTATUS error = ERROR_SUCCESS;
};

ListProbe ProbeList(const wchar_t* subkey) noexcept {
    ListProbe probe;
    HKEY key = nullptr;
    const LSTATUS open = RegOpenKeyExW(HKEY_CURRENT_USER, subkey, 0, KEY_QUERY_VALUE, &key);
    if (open == ERROR_FILE_NOT_FOUND) return probe;
    if (open != ERROR_SUCCESS) { probe.error = open; return probe; }
    probe.exists = true;
    wchar_t name[260];
    for (DWORD index = 0;; ++index) {
        DWORD cch = static_cast<DWORD>(sizeof(name) / sizeof(name[0]));
        const LSTATUS status = RegEnumValueW(key, index, name, &cch, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status == ERROR_MORE_DATA || (status == ERROR_SUCCESS && cch > 0)) { probe.hasEntries = true; break; }
        if (status != ERROR_SUCCESS) { probe.error = status; break; }
    }
    RegCloseKey(key);
    return probe;
}

void Describe(SeedReport& report, const wchar_t* list, const wchar_t* api, const wchar_t* name, LSTATUS status) noexcept {
    report.error = status;
    wchar_t text[512];
    _snwprintf_s(text, _TRUNCATE, L"%s: %s(%s) failed with error %ld", list, api, name ? name : L"", static_cast<long>(status));
    try { report.detail = text; } catch (...) {}
}

LSTATUS WriteDword(HKEY key, const wchar_t* name, DWORD value) noexcept {
    return RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

} // namespace

bool SeedProcessLists(SeedReport& report) noexcept {
    report = SeedReport{};

    // AttachList: one REG_DWORD (Shared::InjectionMode) per executable.
    {
        const ListProbe probe = ProbeList(Defaults::kAttachListSubkey);
        if (probe.error != ERROR_SUCCESS) { Describe(report, L"AttachList", L"RegOpenKeyEx", nullptr, probe.error); return false; }
        if (!probe.hasEntries) {
            HKEY key = nullptr;
            const LSTATUS created = RegCreateKeyExW(HKEY_CURRENT_USER, Defaults::kAttachListSubkey, 0, nullptr,
                REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr);
            if (created != ERROR_SUCCESS) { Describe(report, L"AttachList", L"RegCreateKeyEx", nullptr, created); return false; }
            report.attachListCreated = true;
            for (const auto& entry : Defaults::kAttachList) {
                const LSTATUS status = WriteDword(key, entry.exe, static_cast<DWORD>(entry.mode));
                if (status != ERROR_SUCCESS) {
                    RegCloseKey(key);
                    Describe(report, L"AttachList", L"RegSetValueEx", entry.exe, status);
                    return false;
                }
                ++report.attachWritten;
            }
            RegCloseKey(key);
        }
    }

    // BlockList: REG_DWORD 1 per executable plus a descriptive default value.
    {
        const ListProbe probe = ProbeList(Defaults::kBlockListSubkey);
        if (probe.error != ERROR_SUCCESS) { Describe(report, L"BlockList", L"RegOpenKeyEx", nullptr, probe.error); return false; }
        if (!probe.hasEntries) {
            HKEY key = nullptr;
            const LSTATUS created = RegCreateKeyExW(HKEY_CURRENT_USER, Defaults::kBlockListSubkey, 0, nullptr,
                REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr);
            if (created != ERROR_SUCCESS) { Describe(report, L"BlockList", L"RegCreateKeyEx", nullptr, created); return false; }
            report.blockListCreated = true;
            const DWORD descriptionBytes =
                static_cast<DWORD>((wcslen(Defaults::kBlockListDescription) + 1) * sizeof(wchar_t));
            // The note is cosmetic; a failure to write it is not a failure of the list.
            (void)RegSetValueExW(key, nullptr, 0, REG_SZ,
                reinterpret_cast<const BYTE*>(Defaults::kBlockListDescription), descriptionBytes);
            for (const wchar_t* exe : Defaults::kBlockList) {
                const LSTATUS status = WriteDword(key, exe, 1);
                if (status != ERROR_SUCCESS) {
                    RegCloseKey(key);
                    Describe(report, L"BlockList", L"RegSetValueEx", exe, status);
                    return false;
                }
                ++report.blockWritten;
            }
            RegCloseKey(key);
        }
    }
    return true;
}

void UndoSeededProcessLists(const SeedReport& report) noexcept {
    // Only lists this run filled are removed; a list the user already had is
    // never touched, exactly like during seeding.
    if (report.attachListCreated) (void)RegDeleteTreeW(HKEY_CURRENT_USER, Defaults::kAttachListSubkey);
    if (report.blockListCreated) (void)RegDeleteTreeW(HKEY_CURRENT_USER, Defaults::kBlockListSubkey);
}

} // namespace GlassUserDefaults
