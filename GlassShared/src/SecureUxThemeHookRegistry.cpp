#include "SharedPch.h"

#include "SecureUxThemeHookRegistry.h"
#include <string>
#include <vector>
#include <cstring>

namespace {
constexpr wchar_t kOurDllName[] = L"SecureUxTheme4.dll";
constexpr wchar_t kIfeoPath[] =
    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\";
constexpr DWORD kFlgApplicationVerifier = 0x100;
constexpr wchar_t kInProcProxy[] = L"%SystemRoot%\\system32\\ThemeUiProxy.dll";
constexpr wchar_t kInProcThemeUi[] = L"%SystemRoot%\\system32\\themeui.dll";
constexpr wchar_t kThemeManager2[] = L"{9324DA94-50EC-4A14-A770-E90CA03E7C8F}";
constexpr wchar_t kThemeManagerShared[] = L"{C04B329E-5823-4415-9C93-BA44688947B0}";
constexpr const wchar_t* kInProcKeysNative[] = {
    L"SOFTWARE\\Classes\\CLSID\\{9324DA94-50EC-4A14-A770-E90CA03E7C8F}\\InProcServer32",
    L"SOFTWARE\\Classes\\CLSID\\{C04B329E-5823-4415-9C93-BA44688947B0}\\InProcServer32",
};
constexpr const wchar_t* kInProcKeysWow[] = {
    L"SOFTWARE\\Classes\\WOW6432Node\\CLSID\\{9324DA94-50EC-4A14-A770-E90CA03E7C8F}\\InProcServer32",
    L"SOFTWARE\\Classes\\WOW6432Node\\CLSID\\{C04B329E-5823-4415-9C93-BA44688947B0}\\InProcServer32",
};

[[nodiscard]] bool TokenEquals(const wchar_t* token, const wchar_t* name) noexcept {
    return token && name && _wcsicmp(token, name) == 0;
}

[[nodiscard]] bool VerifierHasToken(const wchar_t* list, const wchar_t* name) noexcept {
    if (!list || !*list || !name) return false;
    std::wstring copy = list;
    wchar_t* context = nullptr;
    for (wchar_t* tok = wcstok_s(copy.data(), L" \t", &context); tok; tok = wcstok_s(nullptr, L" \t", &context))
        if (TokenEquals(tok, name)) return true;
    return false;
}

[[nodiscard]] std::wstring AddVerifierToken(const wchar_t* list, const wchar_t* name) {
    if (VerifierHasToken(list, name)) return list ? list : std::wstring();
    std::wstring result;
    if (list && *list) { result = list; result += L' '; }
    result += name;
    return result;
}

[[nodiscard]] std::wstring RemoveVerifierToken(const wchar_t* list, const wchar_t* name) {
    if (!list || !*list) return {};
    std::wstring result;
    std::vector<wchar_t> copy(list, list + wcslen(list));
    copy.push_back(L'\0');
    wchar_t* context = nullptr;
    for (wchar_t* tok = wcstok_s(copy.data(), L" \t", &context); tok; tok = wcstok_s(nullptr, L" \t", &context)) {
        if (TokenEquals(tok, name)) continue;
        if (!result.empty()) result += L' ';
        result += tok;
    }
    return result;
}

[[nodiscard]] std::wstring IfeoSubkey(const wchar_t* executable) {
    return std::wstring(kIfeoPath) + executable;
}

void ReadIfeo(const wchar_t* executable, DWORD& globalFlag, wchar_t* verifierDlls, size_t verifierCch) noexcept {
    globalFlag = 0;
    if (verifierDlls && verifierCch) verifierDlls[0] = 0;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, IfeoSubkey(executable).c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return;
    DWORD type = 0, size = sizeof(globalFlag);
    if (RegQueryValueExW(key, L"GlobalFlag", nullptr, &type, reinterpret_cast<BYTE*>(&globalFlag), &size) != ERROR_SUCCESS)
        globalFlag = 0;
    if (verifierDlls && verifierCch) {
        DWORD bytes = static_cast<DWORD>(verifierCch * sizeof(wchar_t));
        type = 0;
        if (RegQueryValueExW(key, L"VerifierDlls", nullptr, &type, reinterpret_cast<BYTE*>(verifierDlls), &bytes) != ERROR_SUCCESS)
            verifierDlls[0] = 0;
        else
            verifierDlls[verifierCch - 1] = 0;
    }
    RegCloseKey(key);
}

[[nodiscard]] LONG WriteIfeo(const wchar_t* executable, DWORD globalFlag, const wchar_t* verifierDlls) noexcept {
    HKEY key = nullptr;
    DWORD disp = 0;
    const LONG created = RegCreateKeyExW(
        HKEY_LOCAL_MACHINE, IfeoSubkey(executable).c_str(), 0, nullptr, 0,
        KEY_SET_VALUE | KEY_QUERY_VALUE | KEY_WOW64_64KEY, nullptr, &key, &disp);
    if (created != ERROR_SUCCESS) return created;
    LONG status = ERROR_SUCCESS;
    if (globalFlag == 0) {
        status = RegDeleteValueW(key, L"GlobalFlag");
        if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
    } else {
        status = RegSetValueExW(key, L"GlobalFlag", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&globalFlag), sizeof(globalFlag));
    }
    if (status != ERROR_SUCCESS) { RegCloseKey(key); return status; }
    if (!verifierDlls || !*verifierDlls) {
        status = RegDeleteValueW(key, L"VerifierDlls");
        if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
    } else {
        const DWORD bytes = static_cast<DWORD>((wcslen(verifierDlls) + 1) * sizeof(wchar_t));
        status = RegSetValueExW(key, L"VerifierDlls", 0, REG_SZ, reinterpret_cast<const BYTE*>(verifierDlls), bytes);
    }
    RegCloseKey(key);
    return status;
}

[[nodiscard]] LONG SetInProcDefault(const wchar_t* subkey, const wchar_t* expandValue) noexcept {
    HKEY key = nullptr;
    LONG status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &key);
    if (status != ERROR_SUCCESS) {
        status = RegCreateKeyExW(
            HKEY_LOCAL_MACHINE, subkey, 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr);
        if (status != ERROR_SUCCESS) return status;
    }
    const DWORD bytes = static_cast<DWORD>((wcslen(expandValue) + 1) * sizeof(wchar_t));
    status = RegSetValueExW(key, nullptr, 0, REG_EXPAND_SZ, reinterpret_cast<const BYTE*>(expandValue), bytes);
    RegCloseKey(key);
    return status;
}

[[nodiscard]] bool SetAllInProcDefaults(const wchar_t* expandValue) noexcept {
    bool ok = true;
    for (const wchar_t* key : kInProcKeysNative)
        if (SetInProcDefault(key, expandValue) != ERROR_SUCCESS) ok = false;
#if defined(_WIN64)
    for (const wchar_t* key : kInProcKeysWow)
        if (SetInProcDefault(key, expandValue) != ERROR_SUCCESS) ok = false;
#endif
    return ok;
}
} // namespace

namespace Shared::SecureUxThemeHook {
bool Install() noexcept {
    DWORD globalFlag = 0;
    wchar_t verifier[512]{};
    ReadIfeo(L"winlogon.exe", globalFlag, verifier, _countof(verifier));
    const auto nextList = AddVerifierToken(verifier, kOurDllName);
    globalFlag |= kFlgApplicationVerifier;
    if (WriteIfeo(L"winlogon.exe", globalFlag, nextList.c_str()) != ERROR_SUCCESS) return false;
    return SetAllInProcDefaults(kInProcProxy);
}

bool Uninstall() noexcept {
    DWORD globalFlag = 0;
    wchar_t verifier[512]{};
    ReadIfeo(L"winlogon.exe", globalFlag, verifier, _countof(verifier));
    const auto nextList = RemoveVerifierToken(verifier, kOurDllName);
    if (nextList.empty()) globalFlag &= ~kFlgApplicationVerifier;
    if (WriteIfeo(L"winlogon.exe", globalFlag, nextList.empty() ? nullptr : nextList.c_str()) != ERROR_SUCCESS)
        return false;
    return SetAllInProcDefaults(kInProcThemeUi);
}
}
