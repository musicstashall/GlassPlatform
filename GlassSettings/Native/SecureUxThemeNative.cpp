#include "NativeHost.h"
#include "StockVisualStyle.h"

#include <DebugLog.h>
#include <Helpers/ThemeHelper.h>
#include <uxtheme.h>
#include <SystemVisualStyle.h>
#include <wil/resource.h>
#include <msi.h>
#include <SecureUxThemeInventory.h>
#include <cstring>

#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "msi.lib")

#include <string>
#include <vector>

namespace
{
    constexpr wchar_t kOurDllName[] = L"SecureUxTheme4.dll";
    constexpr wchar_t kLegacyDllName[] = L"SecureUxTheme.dll";
    constexpr wchar_t kProxyDllName[] = L"ThemeUiProxy.dll";
    constexpr wchar_t kLoadedEventName[] = L"SecureUxTheme_Loaded";
    constexpr DWORD kFlgApplicationVerifier = 0x100;
    constexpr wchar_t kIfeoPath[] =
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\";

    constexpr wchar_t kThemeManager2[] = L"{9324DA94-50EC-4A14-A770-E90CA03E7C8F}";
    constexpr wchar_t kThemeManagerShared[] = L"{C04B329E-5823-4415-9C93-BA44688947B0}";
    constexpr wchar_t kInProcProxy[] = L"%SystemRoot%\\system32\\ThemeUiProxy.dll";
    constexpr wchar_t kInProcThemeUi[] = L"%SystemRoot%\\system32\\themeui.dll";

    constexpr const wchar_t* kInProcKeysNative[] = {
        L"SOFTWARE\\Classes\\CLSID\\{9324DA94-50EC-4A14-A770-E90CA03E7C8F}\\InProcServer32",
        L"SOFTWARE\\Classes\\CLSID\\{C04B329E-5823-4415-9C93-BA44688947B0}\\InProcServer32",
    };
    constexpr const wchar_t* kInProcKeysWow[] = {
        L"SOFTWARE\\Classes\\WOW6432Node\\CLSID\\{9324DA94-50EC-4A14-A770-E90CA03E7C8F}\\InProcServer32",
        L"SOFTWARE\\Classes\\WOW6432Node\\CLSID\\{C04B329E-5823-4415-9C93-BA44688947B0}\\InProcServer32",
    };

    HMODULE g_loadedPatcher = nullptr;
    [[nodiscard]] bool HasUnsupportedLegacySecureUxTheme(const GlassSettings_SecureUxThemeNativeQuery& q) noexcept;

    struct Wow64FsRedirectGuard
    {
        PVOID cookie = nullptr;
        BOOL disabled = FALSE;

        Wow64FsRedirectGuard() noexcept
        {
            disabled = Wow64DisableWow64FsRedirection(&cookie);
        }

        ~Wow64FsRedirectGuard() noexcept
        {
            if (disabled)
                Wow64RevertWow64FsRedirection(cookie);
        }
    };

    [[nodiscard]] bool IsWow64ProcessNow() noexcept
    {
        BOOL wow = FALSE;
        if (!IsWow64Process(GetCurrentProcess(), &wow))
            return true; // Unknown process architecture must never permit native registry writes.
        return wow != FALSE;
    }

    [[nodiscard]] USHORT QueryNativeMachine() noexcept
    {
        using Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
        const auto fn = reinterpret_cast<Fn>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));
        if (fn)
        {
            USHORT processMachine = 0;
            USHORT nativeMachine = 0;
            if (fn(GetCurrentProcess(), &processMachine, &nativeMachine) && nativeMachine != 0)
                return nativeMachine;
        }

        SYSTEM_INFO info{};
        GetNativeSystemInfo(&info);
        switch (info.wProcessorArchitecture)
        {
        case PROCESSOR_ARCHITECTURE_AMD64:
            return IMAGE_FILE_MACHINE_AMD64;
        case PROCESSOR_ARCHITECTURE_ARM64:
            return IMAGE_FILE_MACHINE_ARM64;
        case PROCESSOR_ARCHITECTURE_INTEL:
            return IMAGE_FILE_MACHINE_I386;
        default:
            return 0;
        }
    }

    void CopyW(wchar_t* dest, size_t cch, const wchar_t* src) noexcept
    {
        if (!dest || cch == 0)
            return;
        dest[0] = 0;
        if (!src)
            return;
        wcsncpy_s(dest, cch, src, _TRUNCATE);
    }

    [[nodiscard]] std::wstring NativeSystem32() noexcept
    {
        wchar_t windows[MAX_PATH]{};
        if (GetWindowsDirectoryW(windows, MAX_PATH) == 0)
            return {};
        std::wstring path = windows;
        if (!path.empty() && path.back() != L'\\')
            path += L'\\';
        path += L"System32";
        return path;
    }

    [[nodiscard]] std::wstring NativeSysWow64() noexcept
    {
        wchar_t windows[MAX_PATH]{};
        if (GetWindowsDirectoryW(windows, MAX_PATH) == 0)
            return {};
        std::wstring path = windows;
        if (!path.empty() && path.back() != L'\\')
            path += L'\\';
        path += L"SysWOW64";
        return path;
    }

    [[nodiscard]] std::wstring JoinPath(const std::wstring& folder, const wchar_t* name) noexcept
    {
        if (folder.empty() || !name)
            return {};
        std::wstring path = folder;
        if (path.back() != L'\\' && path.back() != L'/')
            path += L'\\';
        path += name;
        return path;
    }

    [[nodiscard]] bool FileExistsNative(const std::wstring& path) noexcept
    {
        Wow64FsRedirectGuard guard;
        const DWORD attr = GetFileAttributesW(path.c_str());
        return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    [[nodiscard]] int ReadPeMachine(const std::wstring& path) noexcept
    {
        Wow64FsRedirectGuard guard;
        const wil::unique_hfile file(CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr));
        if (!file)
            return 0;

        IMAGE_DOS_HEADER dos{};
        DWORD read = 0;
        if (!ReadFile(file.get(), &dos, sizeof(dos), &read, nullptr) || read != sizeof(dos))
            return 0;
        if (dos.e_magic != IMAGE_DOS_SIGNATURE)
            return 0;
        if (SetFilePointer(file.get(), dos.e_lfanew, nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER)
            return 0;

        IMAGE_NT_HEADERS32 nt{};
        if (!ReadFile(file.get(), &nt, sizeof(nt), &read, nullptr) || read < sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER))
            return 0;
        if (nt.Signature != IMAGE_NT_SIGNATURE)
            return 0;
        return nt.FileHeader.Machine;
    }

    [[nodiscard]] bool TokenEquals(const wchar_t* token, const wchar_t* name) noexcept
    {
        return token && name && _wcsicmp(token, name) == 0;
    }

    [[nodiscard]] bool VerifierHasToken(const wchar_t* list, const wchar_t* name) noexcept
    {
        if (!list || !*list || !name)
            return false;

        std::wstring copy = list;
        wchar_t* context = nullptr;
        for (wchar_t* tok = wcstok_s(copy.data(), L" \t", &context);
             tok;
             tok = wcstok_s(nullptr, L" \t", &context))
        {
            if (TokenEquals(tok, name))
                return true;
        }
        return false;
    }

    [[nodiscard]] std::wstring AddVerifierToken(const wchar_t* list, const wchar_t* name)
    {
        if (VerifierHasToken(list, name))
            return list ? list : std::wstring();

        std::wstring result;
        if (list && *list)
        {
            result = list;
            result += L' ';
        }
        result += name;
        return result;
    }

    [[nodiscard]] std::wstring RemoveVerifierToken(const wchar_t* list, const wchar_t* name)
    {
        if (!list || !*list)
            return {};

        std::wstring result;
        std::wstring copy = list;
        wchar_t* context = nullptr;
        for (wchar_t* tok = wcstok_s(copy.data(), L" \t", &context);
             tok;
             tok = wcstok_s(nullptr, L" \t", &context))
        {
            if (TokenEquals(tok, name))
                continue;
            if (!result.empty())
                result += L' ';
            result += tok;
        }
        return result;
    }

    [[nodiscard]] std::wstring IfeoSubkey(const wchar_t* executable)
    {
        return std::wstring(kIfeoPath) + executable;
    }

    void ReadIfeo(
        const wchar_t* executable,
        DWORD& globalFlag,
        wchar_t* verifierDlls,
        size_t verifierCch) noexcept
    {
        globalFlag = 0;
        if (verifierDlls && verifierCch)
            verifierDlls[0] = 0;

        const auto subkey = IfeoSubkey(executable);
        wil::unique_hkey key;
        if (RegOpenKeyExW(
                HKEY_LOCAL_MACHINE,
                subkey.c_str(),
                0,
                KEY_READ | KEY_WOW64_64KEY,
                &key) != ERROR_SUCCESS)
        {
            return;
        }

        DWORD type = 0;
        DWORD size = sizeof(globalFlag);
        if (RegQueryValueExW(key.get(), L"GlobalFlag", nullptr, &type, reinterpret_cast<BYTE*>(&globalFlag), &size)
            != ERROR_SUCCESS)
        {
            globalFlag = 0;
        }

        if (verifierDlls && verifierCch)
        {
            DWORD bytes = static_cast<DWORD>(verifierCch * sizeof(wchar_t));
            type = 0;
            if (RegQueryValueExW(
                    key.get(),
                    L"VerifierDlls",
                    nullptr,
                    &type,
                    reinterpret_cast<BYTE*>(verifierDlls),
                    &bytes) != ERROR_SUCCESS)
            {
                verifierDlls[0] = 0;
            }
            else
            {
                verifierDlls[verifierCch - 1] = 0;
            }
        }
    }

    [[nodiscard]] bool LoadedEventPresent() noexcept
    {
        const wil::unique_handle event(OpenEventW(SYNCHRONIZE, FALSE, kLoadedEventName));
        return static_cast<bool>(event);
    }

    [[nodiscard]] bool InProcIsProxy(HKEY root, const wchar_t* clsid, REGSAM wow) noexcept
    {
        std::wstring path = L"SOFTWARE\\Classes\\CLSID\\";
        path += clsid;
        path += L"\\InProcServer32";

        wil::unique_hkey key;
        if (RegOpenKeyExW(root, path.c_str(), 0, KEY_READ | wow, &key) != ERROR_SUCCESS)
            return false;

        wchar_t value[MAX_PATH]{};
        DWORD bytes = sizeof(value);
        DWORD type = 0;
        if (RegQueryValueExW(key.get(), nullptr, nullptr, &type, reinterpret_cast<BYTE*>(value), &bytes)
            != ERROR_SUCCESS)
        {
            return false;
        }

        const wchar_t* file = wcsrchr(value, L'\\');
        file = file ? file + 1 : value;
        return _wcsicmp(file, kProxyDllName) == 0;
    }

    [[nodiscard]] bool ComProxyHijacked() noexcept
    {
        if (InProcIsProxy(HKEY_LOCAL_MACHINE, kThemeManager2, KEY_WOW64_64KEY)
            || InProcIsProxy(HKEY_LOCAL_MACHINE, kThemeManagerShared, KEY_WOW64_64KEY))
        {
            return true;
        }

        if (IsWow64ProcessNow()
            || QueryNativeMachine() == IMAGE_FILE_MACHINE_AMD64
            || QueryNativeMachine() == IMAGE_FILE_MACHINE_ARM64)
        {
            if (InProcIsProxy(HKEY_LOCAL_MACHINE, kThemeManager2, KEY_WOW64_32KEY)
                || InProcIsProxy(HKEY_LOCAL_MACHINE, kThemeManagerShared, KEY_WOW64_32KEY))
            {
                return true;
            }
        }

        return false;
    }

    [[nodiscard]] bool OfficialMsiProductPresent() noexcept
    {
        return Shared::SecureUxThemeInventory::IsInstalled();
    }

    [[nodiscard]] bool IsNative64() noexcept
    {
        const USHORT machine = QueryNativeMachine();
        return machine == IMAGE_FILE_MACHINE_AMD64 || machine == IMAGE_FILE_MACHINE_ARM64;
    }

    // REG_OPTION_BACKUP_RESTORE grants the requested restore access using
    // privileges, without taking ownership or leaving a relaxed WRP key DACL.
    struct RegistrationPrivileges {
        wil::unique_handle token;
        BYTE previous[sizeof(TOKEN_PRIVILEGES)+2*sizeof(LUID_AND_ATTRIBUTES)]{};
        bool adjusted=false, ready=false;
        RegistrationPrivileges() noexcept {
            HANDLE raw=nullptr;
            if (!OpenProcessToken(GetCurrentProcess(),TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY,&raw)) return;
            token.reset(raw);
            struct { DWORD count; LUID_AND_ATTRIBUTES items[2]; } requested{};
            requested.count=2;
            if (!LookupPrivilegeValueW(nullptr,SE_BACKUP_NAME,&requested.items[0].Luid) ||
                !LookupPrivilegeValueW(nullptr,SE_RESTORE_NAME,&requested.items[1].Luid)) return;
            for (auto& item:requested.items) item.Attributes=SE_PRIVILEGE_ENABLED;
            DWORD bytes=sizeof(previous); SetLastError(ERROR_SUCCESS);
            adjusted=AdjustTokenPrivileges(token.get(),FALSE,reinterpret_cast<TOKEN_PRIVILEGES*>(&requested),
                sizeof(previous),reinterpret_cast<TOKEN_PRIVILEGES*>(previous),&bytes)!=FALSE;
            ready=adjusted && GetLastError()==ERROR_SUCCESS;
        }
        ~RegistrationPrivileges() {
            if (adjusted) AdjustTokenPrivileges(token.get(),FALSE,reinterpret_cast<TOKEN_PRIVILEGES*>(previous),0,nullptr,nullptr);
        }
    };
    struct RegistrationValue {
        std::wstring key, name;
        bool exists=false;
        DWORD type=REG_NONE;
        std::vector<BYTE> data;
    };
    [[nodiscard]] LONG ReadRegistrationValue(RegistrationValue& value) {
        wil::unique_hkey key;
        LONG rc=RegOpenKeyExW(HKEY_LOCAL_MACHINE,value.key.c_str(),0,KEY_QUERY_VALUE|KEY_WOW64_64KEY,&key);
        if (rc==ERROR_ACCESS_DENIED) {
            DWORD disposition=0;
            rc=RegCreateKeyExW(HKEY_LOCAL_MACHINE,value.key.c_str(),0,nullptr,REG_OPTION_BACKUP_RESTORE,
                0,nullptr,&key,&disposition);
        }
        value.exists=false; value.data.clear(); value.type=REG_NONE;
        if (rc==ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
        if (rc!=ERROR_SUCCESS) return rc;
        DWORD bytes=0;
        rc=RegQueryValueExW(key.get(),value.name.c_str(),nullptr,&value.type,nullptr,&bytes);
        if (rc==ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
        if (rc!=ERROR_SUCCESS || bytes>65536) return rc==ERROR_SUCCESS?ERROR_INVALID_DATA:rc;
        value.data.resize(bytes);
        rc=RegQueryValueExW(key.get(),value.name.c_str(),nullptr,&value.type,value.data.data(),&bytes);
        if (rc==ERROR_SUCCESS) { value.exists=true; value.data.resize(bytes); }
        return rc;
    }
    [[nodiscard]] bool SameRegistrationValue(const RegistrationValue& a,const RegistrationValue& b) {
        return a.exists==b.exists && (!a.exists || (a.type==b.type && a.data==b.data));
    }
    [[nodiscard]] LONG WriteRegistrationValue(const RegistrationValue& value) {
        wil::unique_hkey key; DWORD disposition=0;
        const LONG rc=RegCreateKeyExW(HKEY_LOCAL_MACHINE,value.key.c_str(),0,nullptr,REG_OPTION_BACKUP_RESTORE,
            0,nullptr,&key,&disposition);
        if (rc!=ERROR_SUCCESS) return rc;
        if (value.exists) return RegSetValueExW(key.get(),value.name.c_str(),0,value.type,
            value.data.data(),static_cast<DWORD>(value.data.size()));
        const LONG removed=RegDeleteValueW(key.get(),value.name.c_str());
        return removed==ERROR_FILE_NOT_FOUND?ERROR_SUCCESS:removed;
    }
    [[nodiscard]] bool RegistrationString(const RegistrationValue& value,std::wstring& text) {
        text.clear(); if (!value.exists) return true;
        if ((value.type!=REG_SZ && value.type!=REG_EXPAND_SZ) || value.data.size()<sizeof(wchar_t) ||
            value.data.size()%sizeof(wchar_t)) return false;
        const auto* data=reinterpret_cast<const wchar_t*>(value.data.data());
        const auto count=value.data.size()/sizeof(wchar_t);
        if (data[count-1]) return false;
        text.assign(data,count-1); return text.find(L'\0')==std::wstring::npos;
    }
    void SetRegistrationString(RegistrationValue& value,const std::wstring& text,DWORD type=REG_SZ) {
        value.exists=!text.empty(); value.type=type;
        const auto* begin=reinterpret_cast<const BYTE*>(text.c_str());
        value.data.assign(begin,begin+(text.size()+1)*sizeof(wchar_t));
    }
    [[nodiscard]] bool KnownComPath(const std::wstring& value,const wchar_t* file) {
        wchar_t expanded[32768]{};
        const DWORD n=ExpandEnvironmentStringsW(value.c_str(),expanded,_countof(expanded));
        return n && n<=_countof(expanded) && _wcsicmp(expanded,JoinPath(NativeSystem32(),file).c_str())==0;
    }
    [[nodiscard]] HRESULT ToggleRegistration(bool enable) {
        RegistrationPrivileges privileges;
        if (!privileges.ready) return HRESULT_FROM_WIN32(ERROR_PRIVILEGE_NOT_HELD);
        wil::unique_handle mutex(CreateMutexW(nullptr,FALSE,L"Global\\GlassSettings.SecureUxThemeRegistration"));
        if (!mutex) return HRESULT_FROM_WIN32(GetLastError());
        const DWORD wait=WaitForSingleObject(mutex.get(),10000);
        if (wait!=WAIT_OBJECT_0 && wait!=WAIT_ABANDONED) return HRESULT_FROM_WIN32(ERROR_BUSY);
        auto unlock=wil::scope_exit([&]{ReleaseMutex(mutex.get());});
        struct Change { RegistrationValue before, after; };
        std::vector<Change> changes;
        auto queue=[&](RegistrationValue before,RegistrationValue after) {
            if (!SameRegistrationValue(before,after)) changes.push_back({std::move(before),std::move(after)});
        };
        // Snapshot and validate every target before the first write. Refuse a
        // foreign COM server, an unknown value type or an unreadable target.
        for (const auto* keys:{&kInProcKeysNative,&kInProcKeysWow}) {
            if (keys==&kInProcKeysWow && !IsNative64()) continue;
            for (const wchar_t* path:*keys) {
                RegistrationValue before; before.key=path;
                LONG rc=ReadRegistrationValue(before);
                if (rc!=ERROR_SUCCESS) return HRESULT_FROM_WIN32(rc);
                if (!before.exists && !enable) continue;
                std::wstring text;
                if (!before.exists || !RegistrationString(before,text) ||
                    (!KnownComPath(text,L"themeui.dll") && !KnownComPath(text,kProxyDllName)))
                    return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                auto after=before; SetRegistrationString(after,enable?kInProcProxy:kInProcThemeUi,REG_EXPAND_SZ);
                queue(before,after);
            }
        }
        for (const wchar_t* exe:{L"winlogon.exe",L"explorer.exe",L"SystemSettings.exe",L"LogonUI.exe",L"dwm.exe"}) {
            if (enable && _wcsicmp(exe,L"winlogon.exe")) continue;
            RegistrationValue dlls; dlls.key=IfeoSubkey(exe); dlls.name=L"VerifierDlls";
            LONG rc=ReadRegistrationValue(dlls); if (rc!=ERROR_SUCCESS) return HRESULT_FROM_WIN32(rc);
            std::wstring text; if (!RegistrationString(dlls,text)) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            if (!enable && !VerifierHasToken(text.c_str(),kOurDllName)) continue;
            RegistrationValue flags; flags.key=dlls.key; flags.name=L"GlobalFlag";
            rc=ReadRegistrationValue(flags); if (rc!=ERROR_SUCCESS) return HRESULT_FROM_WIN32(rc);
            DWORD bits=0;
            if (flags.exists) {
                if (flags.type!=REG_DWORD || flags.data.size()!=sizeof(bits)) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                memcpy(&bits,flags.data.data(),sizeof(bits));
            }
            auto nextDlls=dlls, nextFlags=flags;
            text=enable?AddVerifierToken(text.c_str(),kOurDllName):RemoveVerifierToken(text.c_str(),kOurDllName);
            SetRegistrationString(nextDlls,text,dlls.exists?dlls.type:REG_SZ);
            if (enable) bits|=kFlgApplicationVerifier;
            else if (text.empty()) bits&=~kFlgApplicationVerifier;
            nextFlags.exists=bits!=0; nextFlags.type=REG_DWORD;
            nextFlags.data.resize(sizeof(bits)); memcpy(nextFlags.data.data(),&bits,sizeof(bits));
            queue(dlls,nextDlls); queue(flags,nextFlags);
        }
        size_t applied=0; LONG error=ERROR_SUCCESS;
        try { for (;applied<changes.size();++applied) {
            auto current=changes[applied].before;
            error=ReadRegistrationValue(current);
            if (error==ERROR_SUCCESS && !SameRegistrationValue(current,changes[applied].before)) error=ERROR_RETRY;
            if (error==ERROR_SUCCESS) error=WriteRegistrationValue(changes[applied].after);
            if (error!=ERROR_SUCCESS) break;
        }
        if (error==ERROR_SUCCESS) {
            for (const auto& change:changes) {
                auto current=change.after;
                error=ReadRegistrationValue(current);
                if (error==ERROR_SUCCESS && !SameRegistrationValue(current,change.after)) error=ERROR_RETRY;
                if (error!=ERROR_SUCCESS) break;
            }
        }
        } catch (...) { error=ERROR_NOT_ENOUGH_MEMORY; }
        if (error!=ERROR_SUCCESS) {
            bool restored=true;
            while (applied) {
                auto& change=changes[--applied];
                try {
                    auto current=change.after;
                    if (ReadRegistrationValue(current)!=ERROR_SUCCESS || !SameRegistrationValue(current,change.after) ||
                        WriteRegistrationValue(change.before)!=ERROR_SUCCESS) restored=false;
                } catch (...) { restored=false; }
            }
            if (!restored) DebugLog(DBG_ERROR,L"[SecureUxTheme] registration rollback incomplete; inspect IFEO/COM before reboot");
            return HRESULT_FROM_WIN32(restored?error:ERROR_TRANSACTIONAL_CONFLICT);
        }
        return S_OK;
    }

}

extern "C" HRESULT WINAPI GlassSettings_SecureUxThemeQuery(
    GlassSettings_SecureUxThemeNativeQuery* out) noexcept
{
    if (!out)
        return E_POINTER;

    ZeroMemory(out, sizeof(*out));

    const USHORT nativeMachine = QueryNativeMachine();
    out->NativeMachine = nativeMachine;
    out->ProcessMachine = sizeof(void*) == 8 ? IMAGE_FILE_MACHINE_AMD64 : IMAGE_FILE_MACHINE_I386;
    out->Wow64Process = IsWow64ProcessNow() ? TRUE : FALSE;

    const auto system32 = NativeSystem32();
    const std::wstring systemDll4 = system32 + L"\\" + kOurDllName;
    const std::wstring systemLegacy = system32 + L"\\" + kLegacyDllName;
    const std::wstring proxy = system32 + L"\\" + kProxyDllName;
    CopyW(out->SystemDll4Path, _countof(out->SystemDll4Path), systemDll4.c_str());
    CopyW(out->SystemLegacyDllPath, _countof(out->SystemLegacyDllPath), systemLegacy.c_str());
    out->SystemDll4Present = FileExistsNative(systemDll4) ? TRUE : FALSE;
    out->SystemLegacyDllPresent = FileExistsNative(systemLegacy) ? TRUE : FALSE;
    out->ThemeUiProxyPresent = FileExistsNative(proxy) ? TRUE : FALSE;
    out->LoadedEventPresent = (LoadedEventPresent() || g_loadedPatcher) ? TRUE : FALSE;

    CopyW(out->LocalDllPath, _countof(out->LocalDllPath), systemDll4.c_str());
    out->LocalDllPresent = out->SystemDll4Present;
    if (out->SystemDll4Present)
    {
        out->LocalDllMachine = ReadPeMachine(systemDll4);
        out->LocalDllArchMatchesOs =
            (out->LocalDllMachine != 0 && out->LocalDllMachine == nativeMachine) ? TRUE : FALSE;
    }

    wchar_t winlogonDlls[512]{};
    ReadIfeo(L"winlogon.exe", out->WinlogonGlobalFlag, winlogonDlls, _countof(winlogonDlls));
    CopyW(out->WinlogonVerifierDlls, _countof(out->WinlogonVerifierDlls), winlogonDlls);
    out->WinlogonHasOurToken = VerifierHasToken(winlogonDlls, kOurDllName) ? TRUE : FALSE;
    out->WinlogonHasLegacyExact = VerifierHasToken(winlogonDlls, kLegacyDllName) ? TRUE : FALSE;

    wchar_t explorerDlls[512]{};
    ReadIfeo(L"explorer.exe", out->ExplorerGlobalFlag, explorerDlls, _countof(explorerDlls));
    out->ExplorerHasLegacyExact = VerifierHasToken(explorerDlls, kLegacyDllName) ? TRUE : FALSE;

    wchar_t settingsDlls[512]{};
    ReadIfeo(L"SystemSettings.exe", out->SettingsGlobalFlag, settingsDlls, _countof(settingsDlls));
    out->SettingsHasLegacyExact = VerifierHasToken(settingsDlls, kLegacyDllName) ? TRUE : FALSE;

    wchar_t logonUiDlls[512]{};
    ReadIfeo(L"LogonUI.exe", out->LogonUiGlobalFlag, logonUiDlls, _countof(logonUiDlls));
    out->LogonUiHasLegacyExact = VerifierHasToken(logonUiDlls, kLegacyDllName) ? TRUE : FALSE;

    wchar_t dwmDlls[512]{};
    ReadIfeo(L"dwm.exe", out->DwmGlobalFlag, dwmDlls, _countof(dwmDlls));
    out->DwmHasLegacyExact = VerifierHasToken(dwmDlls, kLegacyDllName) ? TRUE : FALSE;

    out->ComProxyHijacked = ComProxyHijacked() ? TRUE : FALSE;
    out->OfficialMsiPresent = OfficialMsiProductPresent() ? TRUE : FALSE;
    return S_OK;
}

extern "C" HRESULT WINAPI GlassSettings_SecureUxThemeActivate(
    const wchar_t* payloadFolder, BOOL reactivate) noexcept
{
    if (IsWow64ProcessNow())
        return HRESULT_FROM_WIN32(ERROR_INSTALL_PLATFORM_UNSUPPORTED);

    const USHORT nativeMachine = QueryNativeMachine();
    if (nativeMachine != IMAGE_FILE_MACHINE_AMD64 && nativeMachine != IMAGE_FILE_MACHINE_I386)
        return HRESULT_FROM_WIN32(ERROR_INSTALL_PLATFORM_UNSUPPORTED);

    (void)payloadFolder; (void)reactivate;
    GlassSettings_SecureUxThemeNativeQuery before{};
    const HRESULT queried=GlassSettings_SecureUxThemeQuery(&before);
    if (FAILED(queried)) return queried;
    if (HasUnsupportedLegacySecureUxTheme(before)) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    const auto system=NativeSystem32();
    std::vector<wil::unique_hfile> modules;
    auto lockModule=[&](const std::wstring& path)->HRESULT {
        wil::unique_hfile handle(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if (!handle) return HRESULT_FROM_WIN32(GetLastError());
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle.get(),&info)) return HRESULT_FROM_WIN32(GetLastError());
        if (info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        modules.push_back(std::move(handle)); return S_OK;
    };
    try {
    for (const wchar_t* file:{kOurDllName,kProxyDllName}) {
        const auto path=JoinPath(system,file);
        const HRESULT locked=lockModule(path); if (FAILED(locked)) return locked;
        if (ReadPeMachine(path)!=nativeMachine) return HRESULT_FROM_WIN32(ERROR_BAD_EXE_FORMAT);
        if (IsNative64()) {
            const auto wow=JoinPath(NativeSysWow64(),file);
            const HRESULT lockedWow=lockModule(wow); if (FAILED(lockedWow)) return lockedWow;
            if (ReadPeMachine(wow)!=IMAGE_FILE_MACHINE_I386)
                return HRESULT_FROM_WIN32(ERROR_BAD_EXE_FORMAT);
        }
    }
    return ToggleRegistration(true);
    } catch (...) { return E_OUTOFMEMORY; }
}

// Registration only. Files, MSI registration and source cache remain untouched.
extern "C" HRESULT WINAPI GlassSettings_SecureUxThemeDeactivate() noexcept
{
    if (IsWow64ProcessNow()) return HRESULT_FROM_WIN32(ERROR_INSTALL_PLATFORM_UNSUPPORTED);
    GlassSettings_SecureUxThemeNativeQuery query{};
    const HRESULT queried=GlassSettings_SecureUxThemeQuery(&query);
    if (FAILED(queried)) return queried;
    if (HasUnsupportedLegacySecureUxTheme(query)) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    if (query.WinlogonHasOurToken || query.LoadedEventPresent ||
        (query.ComProxyHijacked && query.SystemDll4Present)) {
        if (Shared::SystemVisualStyle::CurrentForMaintenance()!=Shared::SystemVisualStyle::State::Stock)
            return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    }
    try { return ToggleRegistration(false); }
    catch (...) { return E_OUTOFMEMORY; }
}

extern "C" BOOL WINAPI GlassSettings_SecureUxThemeEnsureLoaded() noexcept
{
    if (g_loadedPatcher)
        return TRUE;

    GlassSettings_SecureUxThemeNativeQuery query{};
    if (FAILED(GlassSettings_SecureUxThemeQuery(&query)) || !query.WinlogonHasOurToken ||
        !(query.WinlogonGlobalFlag&kFlgApplicationVerifier) || !query.ComProxyHijacked) return FALSE;
    const auto path = NativeSystem32() + L"\\" + kOurDllName;
    Wow64FsRedirectGuard guard;
    g_loadedPatcher = LoadLibraryW(path.c_str());
    return g_loadedPatcher ? TRUE : FALSE;
}

extern "C" BOOL WINAPI GlassSettings_SecureUxThemeCheckCleanupPending() noexcept
{
    return FALSE;
}

extern "C" BOOL WINAPI GlassSettings_SecureUxThemeTryDeleteSystemFiles() noexcept
{
    return FALSE; // MSI owns these files; never delete them outside its transaction.
}

namespace
{
    // Биты кода выхода GlassSettings_SecureUxThemeSetupV1W (helper деинсталлятора).
    constexpr DWORD kSecureUxExitBadArgs = 1u;
    constexpr DWORD kSecureUxExitRebootRequired = 2u;
    constexpr DWORD kSecureUxExitThemeNotRestored = 4u;
    constexpr DWORD kSecureUxExitDeactivateFailed = 8u;
    constexpr DWORD kSecureUxExitExternalInstall = 16u;
    constexpr DWORD kSecureUxStateUnavailable = 0u;
    constexpr DWORD kSecureUxStateInactive = 1u;
    constexpr DWORD kSecureUxStateActive = 2u;
    constexpr DWORD kSecureUxStateActivationPending = 3u;
    constexpr DWORD kSecureUxStateDeactivationPending = 4u;
    constexpr DWORD kSecureUxStateExternal = 5u;
    constexpr DWORD kSecureUxStateConflict = 6u;

    // Only incompatible pre-v4 installations remain excluded. An official v4
    // MSI installed outside GlassSetup is managed exactly like any other v4.
    [[nodiscard]] bool HasUnsupportedLegacySecureUxTheme(
        const GlassSettings_SecureUxThemeNativeQuery& q) noexcept
    {
        const auto legacyHook = [](DWORD globalFlag, BOOL exact) noexcept {
            return exact != FALSE && (globalFlag & kFlgApplicationVerifier) != 0;
        };
        return legacyHook(q.WinlogonGlobalFlag, q.WinlogonHasLegacyExact)
            || legacyHook(q.ExplorerGlobalFlag, q.ExplorerHasLegacyExact)
            || legacyHook(q.SettingsGlobalFlag, q.SettingsHasLegacyExact)
            || legacyHook(q.LogonUiGlobalFlag, q.LogonUiHasLegacyExact)
            || legacyHook(q.DwmGlobalFlag, q.DwmHasLegacyExact);
    }

    // Возврат к стандартному стилю до снятия обхода подписи — иначе после перезагрузки
    // пользовательский .msstyles не пройдёт проверку подписи (parity с toggle-off GS).
    [[nodiscard]] bool ApplyStockVisualStyle() noexcept
    {
        NativeStockVisualStyle::Result result{};
        const HRESULT hr=NativeStockVisualStyle::Apply(result);
        DebugLog(DBG_NORMAL,L"[GlassSettings][SecureUxTheme] stock theme: hr=0x%08lX COM=0x%08lX font=0x%08lX read=0x%08lX verified=%d",
            static_cast<unsigned long>(hr),static_cast<unsigned long>(result.comHr),static_cast<unsigned long>(result.fontHr),static_cast<unsigned long>(result.readHr),result.verified?1:0);
        return SUCCEEDED(hr)&&result.verified;
    }
}

// Legacy GlassUnins entry: stock-style preparation only. The explicit Settings
// toggle uses GlassSettings_SecureUxThemeDeactivate separately. MSI owns removal.
// Биты кода выхода: 1 — неверные аргументы; 2 — нужна перезагрузка (отложена очистка
// файлов); 4 — стандартный стиль не подтверждён (hook сохранён); 8 —
// деактивация не удалась; 16 — установлен внешний SecureUxTheme, ничего не изменено.
extern "C" void CALLBACK GlassSettings_SecureUxThemeSetupV1W(
    HWND, HINSTANCE, LPWSTR cmdLine, int) noexcept
{
#ifdef _M_IX86
#pragma comment(linker, "/EXPORT:GlassSettings_SecureUxThemeSetupV1W=_GlassSettings_SecureUxThemeSetupV1W@16")
#endif
    if (cmdLine && *cmdLine)
        ::ExitProcess(kSecureUxExitBadArgs);
    if (IsWow64ProcessNow())
        ::ExitProcess(kSecureUxExitDeactivateFailed);

    GlassSettings_SecureUxThemeNativeQuery query{};
    if (FAILED(GlassSettings_SecureUxThemeQuery(&query)))
        ::ExitProcess(kSecureUxExitDeactivateFailed);
    if (HasUnsupportedLegacySecureUxTheme(query))
        ::ExitProcess(kSecureUxExitExternalInstall);

    DWORD code = 0;
    const bool oursActive = query.WinlogonHasOurToken
        || query.LoadedEventPresent
        || (query.ComProxyHijacked && query.SystemDll4Present);
    const auto style=Shared::SystemVisualStyle::CurrentForMaintenance();
    if(oursActive&&style==Shared::SystemVisualStyle::State::Unknown) {
        DebugLog(DBG_ERROR,L"[GlassSettings][SecureUxTheme] active visual style could not be verified; hook retained");
        ::ExitProcess(kSecureUxExitDeactivateFailed|kSecureUxExitThemeNotRestored);
    }
    if (oursActive && style==Shared::SystemVisualStyle::State::ResetRequired &&
        (!ApplyStockVisualStyle()||Shared::SystemVisualStyle::CurrentForMaintenance()!=Shared::SystemVisualStyle::State::Stock))
    {
        DebugLog(DBG_WARNING,
            L"[GlassSettings][SecureUxTheme] setup: stock visual style could not be applied");
        ::ExitProcess(kSecureUxExitDeactivateFailed | kSecureUxExitThemeNotRestored);
    }
    // Uninstaller helper prepares style only. MSI may intentionally retain proxy/COM.
    // Settings toggle-off is a separate explicit registration operation.
    ::ExitProcess(code);
}

// Versioned contract: prepares only the current visual style, never removes
// IFEO/COM registrations or system files. Old installed DLLs lack this export.
extern "C" void CALLBACK GlassSettings_SecureUxThemePrepareRemovalV2W(
    HWND hwnd, HINSTANCE instance, LPWSTR cmdLine, int show) noexcept
{
#ifdef _M_IX86
#pragma comment(linker, "/EXPORT:GlassSettings_SecureUxThemePrepareRemovalV2W=_GlassSettings_SecureUxThemePrepareRemovalV2W@16")
#endif
    (void)hwnd;(void)instance;(void)show;
    if(cmdLine&&*cmdLine)::ExitProcess(static_cast<UINT>(E_INVALIDARG));
    if(IsWow64ProcessNow())::ExitProcess(static_cast<UINT>(HRESULT_FROM_WIN32(ERROR_INSTALL_PLATFORM_UNSUPPORTED)));
    GlassSettings_SecureUxThemeNativeQuery query{};
    const HRESULT queried=GlassSettings_SecureUxThemeQuery(&query);
    if(FAILED(queried))::ExitProcess(static_cast<UINT>(queried));
    if(HasUnsupportedLegacySecureUxTheme(query))::ExitProcess(static_cast<UINT>(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)));
    const bool active=query.WinlogonHasOurToken||query.LoadedEventPresent||
        (query.ComProxyHijacked&&query.SystemDll4Present);
    if(!active)::ExitProcess(0);
    NativeStockVisualStyle::Result result{};
    const HRESULT hr=NativeStockVisualStyle::Apply(result);
    DebugLog(DBG_NORMAL,L"[GlassSettings][SecureUxTheme] removal theme: hr=0x%08lX COM=0x%08lX font=0x%08lX read=0x%08lX verified=%d",
        static_cast<unsigned long>(hr),static_cast<unsigned long>(result.comHr),static_cast<unsigned long>(result.fontHr),static_cast<unsigned long>(result.readHr),result.verified?1:0);
    // V2's caller accepts only zero; keep V1's legacy bitmask unchanged, but
    // return the actual HRESULT here instead of collapsing every failure to 0xC.
    ::ExitProcess(static_cast<UINT>(FAILED(hr)?hr:result.verified?S_OK:HRESULT_FROM_WIN32(ERROR_INVALID_STATE)));
}

// Небольшие протоколы для карточки SecureUxTheme в GlassSetup/GlassUnins.
// Значение состояния передаётся кодом завершения процесса rundll32, чтобы
// Setup любой разрядности мог вызвать нативную DLL, соответствующую ОС.
extern "C" void CALLBACK GlassSettings_SecureUxThemeQueryStateV1W(
    HWND, HINSTANCE, LPWSTR cmdLine, int) noexcept
{
#ifdef _M_IX86
#pragma comment(linker, "/EXPORT:GlassSettings_SecureUxThemeQueryStateV1W=_GlassSettings_SecureUxThemeQueryStateV1W@16")
#endif
    if (cmdLine && *cmdLine) ::ExitProcess(kSecureUxStateConflict);
    if (IsWow64ProcessNow()) ::ExitProcess(kSecureUxStateConflict);
    GlassSettings_SecureUxThemeNativeQuery q{};
    if (FAILED(GlassSettings_SecureUxThemeQuery(&q))) ::ExitProcess(kSecureUxStateConflict);
    if (HasUnsupportedLegacySecureUxTheme(q)) ::ExitProcess(kSecureUxStateExternal);
    const bool ourIfeo=q.WinlogonHasOurToken && (q.WinlogonGlobalFlag&kFlgApplicationVerifier)!=0;
    if (ourIfeo && q.LoadedEventPresent) ::ExitProcess(kSecureUxStateActive);
    if (ourIfeo) ::ExitProcess(kSecureUxStateActivationPending);
    if (q.LoadedEventPresent) ::ExitProcess(kSecureUxStateDeactivationPending);
    // Inert DLLs and the author's permanent proxy after MSI removal are not corruption.
    if (q.ComProxyHijacked || q.SystemDll4Present || q.ThemeUiProxyPresent || q.OfficialMsiPresent)
        ::ExitProcess(kSecureUxStateInactive);
    ::ExitProcess(kSecureUxStateUnavailable);
}

extern "C" void CALLBACK GlassSettings_SecureUxThemeActivateSetupV1W(
    HWND, HINSTANCE, LPWSTR cmdLine, int) noexcept
{
#ifdef _M_IX86
#pragma comment(linker, "/EXPORT:GlassSettings_SecureUxThemeActivateSetupV1W=_GlassSettings_SecureUxThemeActivateSetupV1W@16")
#endif
    if (cmdLine && *cmdLine) ::ExitProcess(kSecureUxExitBadArgs);
    if (IsWow64ProcessNow()) ::ExitProcess(kSecureUxExitDeactivateFailed);
    GlassSettings_SecureUxThemeNativeQuery before{};
    if (FAILED(GlassSettings_SecureUxThemeQuery(&before))) ::ExitProcess(kSecureUxExitDeactivateFailed);
    if (HasUnsupportedLegacySecureUxTheme(before)) ::ExitProcess(kSecureUxExitExternalInstall);
    if (FAILED(GlassSettings_SecureUxThemeActivate(nullptr,FALSE))) ::ExitProcess(kSecureUxExitDeactivateFailed);
    GlassSettings_SecureUxThemeNativeQuery after{};
    if (FAILED(GlassSettings_SecureUxThemeQuery(&after))) ::ExitProcess(kSecureUxExitDeactivateFailed);
    const bool active=after.WinlogonHasOurToken && (after.WinlogonGlobalFlag&kFlgApplicationVerifier)!=0;
    if(!active)::ExitProcess(kSecureUxExitDeactivateFailed);
    ::ExitProcess(!after.LoadedEventPresent ? kSecureUxExitRebootRequired : 0u);
}
