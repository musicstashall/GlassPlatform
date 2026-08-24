#include "NativeHost.h"

#include <TaskSchedulerCom.h>
#include <GlassPlatformTask.h>
#include <aclapi.h>
#include <wil/resource.h>

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

    // Packed ProductCode x64 MSI v4: {D686CD0E-139E-4599-A4E2-1B9BC0B72F73}
    constexpr wchar_t kOfficialV4ProductPacked[] = L"E0DC686DE93199544A2EB1B90C7BF237";
    constexpr wchar_t kInstallerProductsPath[] =
        L"SOFTWARE\\Classes\\Installer\\Products\\";

    constexpr wchar_t kThemeManager2[] = L"{9324DA94-50EC-4A14-A770-E90CA03E7C8F}";
    constexpr wchar_t kThemeManagerShared[] = L"{C04B329E-5823-4415-9C93-BA44688947B0}";
    constexpr wchar_t kPayloadFolderName[] = L"SecureUxTheme";
    constexpr wchar_t kPatcherDll[] = L"SecureUxTheme4.dll";
    constexpr wchar_t kProxyDll[] = L"ThemeUiProxy.dll";
    constexpr wchar_t kInProcProxy[] = L"%SystemRoot%\\system32\\ThemeUiProxy.dll";
    constexpr wchar_t kInProcThemeUi[] = L"%SystemRoot%\\system32\\themeui.dll";
    constexpr wchar_t kCleanupScriptName[] = L"SecureUxThemeCleanup.cmd";

    constexpr const wchar_t* kInProcKeysNative[] = {
        L"SOFTWARE\\Classes\\CLSID\\{9324DA94-50EC-4A14-A770-E90CA03E7C8F}\\InProcServer32",
        L"SOFTWARE\\Classes\\CLSID\\{C04B329E-5823-4415-9C93-BA44688947B0}\\InProcServer32",
    };
    constexpr const wchar_t* kInProcKeysWow[] = {
        L"SOFTWARE\\Classes\\WOW6432Node\\CLSID\\{9324DA94-50EC-4A14-A770-E90CA03E7C8F}\\InProcServer32",
        L"SOFTWARE\\Classes\\WOW6432Node\\CLSID\\{C04B329E-5823-4415-9C93-BA44688947B0}\\InProcServer32",
    };

    HMODULE g_loadedPatcher = nullptr;

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
            return false;
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

    [[nodiscard]] std::wstring DirectoryName(const std::wstring& path) noexcept
    {
        if (path.empty())
            return {};
        auto trimmed = path;
        if (trimmed.back() == L'\\' || trimmed.back() == L'/')
            trimmed.pop_back();
        const auto slash = trimmed.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
            return {};
        trimmed.resize(slash);
        return trimmed;
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

    [[nodiscard]] bool VerifierExact(const wchar_t* list, const wchar_t* name) noexcept
    {
        if (!list || !name)
            return false;
        std::wstring trimmed = list;
        while (!trimmed.empty() && (trimmed.back() == L' ' || trimmed.back() == L'\t'))
            trimmed.pop_back();
        return _wcsicmp(trimmed.c_str(), name) == 0;
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

    [[nodiscard]] HRESULT WriteIfeo(const wchar_t* executable, DWORD globalFlag, const wchar_t* verifierDlls) noexcept
    {
        const auto subkey = IfeoSubkey(executable);
        wil::unique_hkey key;
        DWORD disp = 0;
        const LONG created = RegCreateKeyExW(
            HKEY_LOCAL_MACHINE,
            subkey.c_str(),
            0,
            nullptr,
            0,
            KEY_SET_VALUE | KEY_QUERY_VALUE | KEY_WOW64_64KEY,
            nullptr,
            &key,
            &disp);
        if (created != ERROR_SUCCESS)
            return HRESULT_FROM_WIN32(created);

        LONG status = ERROR_SUCCESS;
        if (globalFlag == 0)
        {
            status = RegDeleteValueW(key.get(), L"GlobalFlag");
            if (status == ERROR_FILE_NOT_FOUND)
                status = ERROR_SUCCESS;
        }
        else
        {
            status = RegSetValueExW(
                key.get(),
                L"GlobalFlag",
                0,
                REG_DWORD,
                reinterpret_cast<const BYTE*>(&globalFlag),
                sizeof(globalFlag));
        }
        if (status != ERROR_SUCCESS)
            return HRESULT_FROM_WIN32(status);

        if (!verifierDlls || !*verifierDlls)
        {
            status = RegDeleteValueW(key.get(), L"VerifierDlls");
            if (status == ERROR_FILE_NOT_FOUND)
                status = ERROR_SUCCESS;
        }
        else
        {
            const DWORD bytes = static_cast<DWORD>((wcslen(verifierDlls) + 1) * sizeof(wchar_t));
            status = RegSetValueExW(
                key.get(),
                L"VerifierDlls",
                0,
                REG_SZ,
                reinterpret_cast<const BYTE*>(verifierDlls),
                bytes);
        }
        return HRESULT_FROM_WIN32(status);
    }

    [[nodiscard]] bool LoadedEventPresent() noexcept
    {
        const wil::unique_handle event(OpenEventW(SYNCHRONIZE, FALSE, kLoadedEventName));
        if (event)
            return true;
        return GetLastError() == ERROR_ACCESS_DENIED;
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

    [[nodiscard]] bool OfficialV4ProductKeyPresent() noexcept
    {
        std::wstring path = kInstallerProductsPath;
        path += kOfficialV4ProductPacked;
        wil::unique_hkey key;
        return RegOpenKeyExW(
                   HKEY_LOCAL_MACHINE,
                   path.c_str(),
                   0,
                   KEY_READ | KEY_WOW64_64KEY,
                   &key)
            == ERROR_SUCCESS;
    }

    [[nodiscard]] bool IsNative64() noexcept
    {
        const USHORT machine = QueryNativeMachine();
        return machine == IMAGE_FILE_MACHINE_AMD64 || machine == IMAGE_FILE_MACHINE_ARM64;
    }

    [[nodiscard]] std::wstring ExeDirectory() noexcept
    {
        wchar_t modulePath[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) == 0)
            return {};
        std::wstring local = modulePath;
        const auto slash = local.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
            return {};
        local.resize(slash);
        return local;
    }

    [[nodiscard]] std::wstring TrimTrailingSlashes(const std::wstring& s) noexcept
    {
        auto end = s.find_last_not_of(L"\\/");
        return end == std::wstring::npos ? s : s.substr(0, end + 1);
    }

    [[nodiscard]] std::wstring TryReadInstallPathFromRegistry() noexcept
    {
#if defined(_WIN64)
        constexpr wchar_t kRegKey[] = L"Software\\Wow6432Node\\GlassPlatform";
#else
        constexpr wchar_t kRegKey[] = L"Software\\GlassPlatform";
#endif
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
            return {};

        wchar_t buf[MAX_PATH * 4]{};
        DWORD cb = sizeof(buf);
        DWORD type = 0;
        const LSTATUS st = RegQueryValueExW(
            hKey, L"InstallPath", nullptr, &type,
            reinterpret_cast<LPBYTE>(buf), &cb);
        RegCloseKey(hKey);

        if (st != ERROR_SUCCESS || type != REG_SZ || !buf[0])
            return {};

        return TrimTrailingSlashes(buf);
    }

    [[nodiscard]] std::wstring TryGetAutorunInstallRoot() noexcept
    {
        return GlassShared::GlassPlatformTask::TryGetAutorunGlassCompatInstallRoot(
            GlassShared::GlassPlatformTask::PlatformArch::X86);
    }

    [[nodiscard]] std::wstring ResolveX86InstallRoot() noexcept
    {
        auto root = TryReadInstallPathFromRegistry();
        if (!root.empty())
        {
            const auto compat = JoinPath(root, L"GlassCompat.dll");
            if (FileExistsNative(compat))
                return root;
        }

        root = TryGetAutorunInstallRoot();
        if (!root.empty())
        {
            const auto compat = JoinPath(root, L"GlassCompat.dll");
            if (FileExistsNative(compat))
                return root;
        }

        return {};
    }

    [[nodiscard]] std::wstring ResolvePayloadFolder(const wchar_t* payloadFolder) noexcept
    {
        if (payloadFolder && *payloadFolder)
            return payloadFolder;

        const auto exeDir = ExeDirectory();
        if (exeDir.empty())
            return {};

        if (IsNative64())
        {
            return JoinPath(DirectoryName(exeDir), kPayloadFolderName);
        }

        return JoinPath(exeDir, kPayloadFolderName);
    }

    [[nodiscard]] bool PayloadComplete(
        const std::wstring& folder,
        std::wstring& patcherSrc,
        std::wstring& proxyNativeSrc,
        std::wstring& proxyWowSrc) noexcept
    {
        patcherSrc.clear();
        proxyNativeSrc.clear();
        proxyWowSrc.clear();
        if (folder.empty())
            return false;

        patcherSrc = JoinPath(folder, kPatcherDll);
        proxyNativeSrc = JoinPath(folder, kProxyDll);
        if (!FileExistsNative(patcherSrc) || !FileExistsNative(proxyNativeSrc))
            return false;

        if (IsNative64())
        {
            const auto x86Root = ResolveX86InstallRoot();
            if (x86Root.empty())
                return false;

            const auto x86Payload = JoinPath(x86Root, kPayloadFolderName);
            proxyWowSrc = JoinPath(x86Payload, kProxyDll);
            return FileExistsNative(proxyWowSrc);
        }

        return true;
    }

    void EnablePrivilege(const wchar_t* name) noexcept
    {
        HANDLE raw = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &raw))
            return;
        wil::unique_handle token(raw);

        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (!LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid))
            return;
        AdjustTokenPrivileges(token.get(), FALSE, &tp, 0, nullptr, nullptr);
    }

    [[nodiscard]] bool MakeAdministratorsSid(PSID* sid) noexcept
    {
        SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
        return AllocateAndInitializeSid(
                   &ntAuth,
                   2,
                   SECURITY_BUILTIN_DOMAIN_RID,
                   DOMAIN_ALIAS_RID_ADMINS,
                   0,
                   0,
                   0,
                   0,
                   0,
                   0,
                   sid)
            != FALSE;
    }

    [[nodiscard]] bool SetKeyOwnerAdministrators(HKEY key) noexcept
    {
        PSID adminSid = nullptr;
        if (!MakeAdministratorsSid(&adminSid))
            return false;
        const auto freeSid = wil::scope_exit([&] { FreeSid(adminSid); });

        SECURITY_DESCRIPTOR sd{};
        if (!InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION))
            return false;
        if (!SetSecurityDescriptorOwner(&sd, adminSid, FALSE))
            return false;
        return RegSetKeySecurity(key, OWNER_SECURITY_INFORMATION, &sd) == ERROR_SUCCESS;
    }

    [[nodiscard]] bool AllowAdministratorsSetValue(HKEY key) noexcept
    {
        DWORD needed = 0;
        RegGetKeySecurity(key, DACL_SECURITY_INFORMATION, nullptr, &needed);
        if (needed == 0)
            return false;

        std::vector<BYTE> buffer(needed);
        auto* sd = reinterpret_cast<PSECURITY_DESCRIPTOR>(buffer.data());
        if (RegGetKeySecurity(key, DACL_SECURITY_INFORMATION, sd, &needed) != ERROR_SUCCESS)
            return false;

        BOOL present = FALSE;
        BOOL defaulted = FALSE;
        PACL oldDacl = nullptr;
        if (!GetSecurityDescriptorDacl(sd, &present, &oldDacl, &defaulted))
            return false;

        SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
        PSID adminSid = nullptr;
        if (!AllocateAndInitializeSid(
                &ntAuth,
                2,
                SECURITY_BUILTIN_DOMAIN_RID,
                DOMAIN_ALIAS_RID_ADMINS,
                0,
                0,
                0,
                0,
                0,
                0,
                &adminSid))
        {
            return false;
        }
        const auto freeSid = wil::scope_exit([&] { FreeSid(adminSid); });

        EXPLICIT_ACCESSW access{};
        access.grfAccessPermissions = KEY_SET_VALUE;
        access.grfAccessMode = GRANT_ACCESS;
        access.grfInheritance = NO_INHERITANCE;
        access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access.Trustee.TrusteeType = TRUSTEE_IS_GROUP;
        access.Trustee.ptstrName = static_cast<LPWSTR>(adminSid);

        PACL newDacl = nullptr;
        if (SetEntriesInAclW(1, &access, present ? oldDacl : nullptr, &newDacl) != ERROR_SUCCESS)
            return false;
        const auto freeAcl = wil::scope_exit([&] { if (newDacl) LocalFree(newDacl); });

        SECURITY_DESCRIPTOR newSd{};
        if (!InitializeSecurityDescriptor(&newSd, SECURITY_DESCRIPTOR_REVISION))
            return false;
        if (!SetSecurityDescriptorDacl(&newSd, TRUE, newDacl, FALSE))
            return false;
        return RegSetKeySecurity(key, DACL_SECURITY_INFORMATION, &newSd) == ERROR_SUCCESS;
    }

    [[nodiscard]] HRESULT SetInProcDefault(const wchar_t* relative, const wchar_t* expandValue) noexcept
    {
        if (!relative || !expandValue)
            return E_INVALIDARG;

        auto tryOpen = [&](REGSAM sam, wil::unique_hkey& key) -> LONG {
            return RegOpenKeyExW(HKEY_LOCAL_MACHINE, relative, 0, sam | KEY_WOW64_64KEY, &key);
        };

        wil::unique_hkey key;
        LONG status = tryOpen(KEY_SET_VALUE, key);
        if (status != ERROR_SUCCESS)
        {
            EnablePrivilege(SE_TAKE_OWNERSHIP_NAME);
            EnablePrivilege(SE_RESTORE_NAME);

            // WRP: KEY_SET_VALUE|WRITE_DAC вместе часто дают 5. Сначала ownership, потом DACL.
            status = tryOpen(WRITE_OWNER, key);
            if (status != ERROR_SUCCESS)
                return HRESULT_FROM_WIN32(status);

            if (!SetKeyOwnerAdministrators(key.get()))
                return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
            key.reset();

            status = tryOpen(KEY_READ | WRITE_DAC, key);
            if (status != ERROR_SUCCESS)
                return HRESULT_FROM_WIN32(status);

            if (!AllowAdministratorsSetValue(key.get()))
                return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
            key.reset();

            status = tryOpen(KEY_SET_VALUE, key);
            if (status != ERROR_SUCCESS)
                return HRESULT_FROM_WIN32(status);
        }

        const DWORD bytes = static_cast<DWORD>((wcslen(expandValue) + 1) * sizeof(wchar_t));
        status = RegSetValueExW(
            key.get(),
            nullptr,
            0,
            REG_EXPAND_SZ,
            reinterpret_cast<const BYTE*>(expandValue),
            bytes);
        return HRESULT_FROM_WIN32(status);
    }

    [[nodiscard]] HRESULT SetAllInProcDefaults(const wchar_t* expandValue) noexcept
    {
        HRESULT firstFail = S_OK;
        for (const wchar_t* key : kInProcKeysNative)
        {
            const HRESULT hr = SetInProcDefault(key, expandValue);
            if (FAILED(hr) && firstFail == S_OK)
                firstFail = hr;
        }
        if (IsNative64())
        {
            for (const wchar_t* key : kInProcKeysWow)
            {
                const HRESULT hr = SetInProcDefault(key, expandValue);
                if (FAILED(hr) && firstFail == S_OK)
                    firstFail = hr;
            }
        }
        return firstFail;
    }

    [[nodiscard]] HRESULT RunHiddenProcess(const std::wstring& application, std::wstring commandLine) noexcept
    {
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(
                application.c_str(),
                commandLine.data(),
                nullptr,
                nullptr,
                FALSE,
                CREATE_NO_WINDOW,
                nullptr,
                nullptr,
                &si,
                &pi))
        {
            return HRESULT_FROM_WIN32(GetLastError());
        }

        wil::unique_handle process(pi.hProcess);
        wil::unique_handle thread(pi.hThread);
        WaitForSingleObject(process.get(), 30000);
        DWORD code = 0;
        if (!GetExitCodeProcess(process.get(), &code))
            return HRESULT_FROM_WIN32(GetLastError());
        return code == 0 ? S_OK : HRESULT_FROM_WIN32(code);
    }

    [[nodiscard]] std::wstring SchtasksPath() noexcept
    {
        return JoinPath(NativeSystem32(), L"schtasks.exe");
    }

    [[nodiscard]] HRESULT RunSchtasks(const wchar_t* arguments) noexcept
    {
        if (!arguments)
            return E_INVALIDARG;
        const auto exe = SchtasksPath();
        std::wstring cmd = L"\"";
        cmd += exe;
        cmd += L"\" ";
        cmd += arguments;
        Wow64FsRedirectGuard guard;
        return RunHiddenProcess(exe, std::move(cmd));
    }

    [[nodiscard]] std::wstring CleanupScriptPath() noexcept
    {
        wchar_t temp[MAX_PATH]{};
        const DWORD n = GetTempPathW(MAX_PATH, temp);
        if (n == 0 || n >= MAX_PATH)
            return {};
        std::wstring path = temp;
        if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
            path += L'\\';
        path += kCleanupScriptName;
        return path;
    }

    void CancelFileCleanup() noexcept
    {
        const auto script = CleanupScriptPath();
        if (!script.empty())
            DeleteFileW(script.c_str());
        (void)RunSchtasks(L"/Delete /TN \"GlassSettings\\SecureUxThemeCleanup\" /F");

        GlassShared::TaskSchedulerCom ts;
        if (ts.Connect())
            (void)ts.DeleteRootSubfolder(L"GlassSettings");
    }

    [[nodiscard]] bool IsCleanupTaskPending() noexcept
    {
        GlassShared::TaskSchedulerCom ts;
        if (!ts.Connect())
            return false;
        return ts.TaskExists(L"GlassSettings", L"SecureUxThemeCleanup");
    }

    [[nodiscard]] HRESULT ScheduleFileCleanup() noexcept
    {
        const auto script = CleanupScriptPath();
        if (script.empty())
            return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);

        static constexpr char kScript[] =
            "@echo off\r\n"
            "del /f /q \"%SystemRoot%\\System32\\SecureUxTheme4.dll\" >nul 2>&1\r\n"
            "del /f /q \"%SystemRoot%\\System32\\ThemeUiProxy.dll\" >nul 2>&1\r\n"
            "if exist \"%SystemRoot%\\SysWOW64\\ThemeUiProxy.dll\" "
            "del /f /q \"%SystemRoot%\\SysWOW64\\ThemeUiProxy.dll\" >nul 2>&1\r\n"
            "schtasks /Delete /TN \"GlassSettings\\SecureUxThemeCleanup\" /F >nul 2>&1\r\n"
            "powershell -NoProfile -Command \"try { $s=New-Object -ComObject Schedule.Service; $s.Connect(); $s.GetFolder('\\').DeleteFolder('GlassSettings', $null) } catch {}\" >nul 2>&1\r\n"
            "del \"%~f0\"\r\n";

        {
            Wow64FsRedirectGuard guard;
            const wil::unique_hfile file(CreateFileW(
                script.c_str(),
                GENERIC_WRITE,
                0,
                nullptr,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                nullptr));
            if (!file)
                return HRESULT_FROM_WIN32(GetLastError());
            DWORD written = 0;
            const DWORD bytes = static_cast<DWORD>(sizeof(kScript) - 1);
            if (!WriteFile(file.get(), kScript, bytes, &written, nullptr) || written != bytes)
                return HRESULT_FROM_WIN32(GetLastError());
        }

        std::wstring args = L"/Create /TN \"GlassSettings\\SecureUxThemeCleanup\" /SC ONSTART /RU SYSTEM /RL HIGHEST /F /TR \"";
        args += script;
        args += L"\"";
        const HRESULT hr = RunSchtasks(args.c_str());
        if (FAILED(hr))
            DeleteFileW(script.c_str());
        return hr;
    }

    [[nodiscard]] HRESULT CopyRenamed(
        const std::wstring& src,
        const std::wstring& dest,
        bool& existed) noexcept
    {
        existed = FileExistsNative(dest);
        Wow64FsRedirectGuard guard;
        if (!CopyFileW(src.c_str(), dest.c_str(), FALSE))
        {
            const DWORD err = GetLastError();
            // Уже в System32: winlogon держит файл (официальный leftover / наш патчер) или WRP ACL.
            // Перезапись не нужна — те же имена SecureUxTheme4.dll / ThemeUiProxy.dll.
            if (existed
                && (err == ERROR_SHARING_VIOLATION
                    || err == ERROR_LOCK_VIOLATION
                    || err == ERROR_ACCESS_DENIED))
            {
                return S_OK;
            }
            return HRESULT_FROM_WIN32(err);
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
    out->LoadedEventPresent = LoadedEventPresent() ? TRUE : FALSE;

    const auto folder = ResolvePayloadFolder(nullptr);
    CopyW(out->LocalDllPath, _countof(out->LocalDllPath), folder.c_str());

    std::wstring patcherSrc;
    std::wstring proxyNativeSrc;
    std::wstring proxyWowSrc;
    const bool complete = PayloadComplete(folder, patcherSrc, proxyNativeSrc, proxyWowSrc);
    out->LocalDllPresent = complete ? TRUE : FALSE;
    if (complete)
    {
        out->LocalDllMachine = ReadPeMachine(patcherSrc);
        out->LocalDllArchMatchesOs =
            (out->LocalDllMachine != 0 && out->LocalDllMachine == nativeMachine) ? TRUE : FALSE;
    }

    wchar_t winlogonDlls[512]{};
    ReadIfeo(L"winlogon.exe", out->WinlogonGlobalFlag, winlogonDlls, _countof(winlogonDlls));
    CopyW(out->WinlogonVerifierDlls, _countof(out->WinlogonVerifierDlls), winlogonDlls);
    out->WinlogonHasOurToken = VerifierHasToken(winlogonDlls, kOurDllName) ? TRUE : FALSE;
    out->WinlogonHasLegacyExact = VerifierExact(winlogonDlls, kLegacyDllName) ? TRUE : FALSE;

    wchar_t explorerDlls[512]{};
    ReadIfeo(L"explorer.exe", out->ExplorerGlobalFlag, explorerDlls, _countof(explorerDlls));
    out->ExplorerHasLegacyExact = VerifierExact(explorerDlls, kLegacyDllName) ? TRUE : FALSE;

    wchar_t settingsDlls[512]{};
    ReadIfeo(L"SystemSettings.exe", out->SettingsGlobalFlag, settingsDlls, _countof(settingsDlls));
    out->SettingsHasLegacyExact = VerifierExact(settingsDlls, kLegacyDllName) ? TRUE : FALSE;

    wchar_t logonUiDlls[512]{};
    ReadIfeo(L"LogonUI.exe", out->LogonUiGlobalFlag, logonUiDlls, _countof(logonUiDlls));
    out->LogonUiHasLegacyExact = VerifierExact(logonUiDlls, kLegacyDllName) ? TRUE : FALSE;

    wchar_t dwmDlls[512]{};
    ReadIfeo(L"dwm.exe", out->DwmGlobalFlag, dwmDlls, _countof(dwmDlls));
    out->DwmHasLegacyExact = VerifierExact(dwmDlls, kLegacyDllName) ? TRUE : FALSE;

    out->ComProxyHijacked = ComProxyHijacked() ? TRUE : FALSE;
    out->OfficialMsiPresent = OfficialV4ProductKeyPresent() ? TRUE : FALSE;
    return S_OK;
}

extern "C" HRESULT WINAPI GlassSettings_SecureUxThemeActivate(
    const wchar_t* payloadFolder, BOOL reactivate) noexcept
{
    if (IsWow64ProcessNow())
        return HRESULT_FROM_WIN32(ERROR_INSTALL_WRONG_PROCESSOR_ARCHITECTURE);

    const USHORT nativeMachine = QueryNativeMachine();
    if (nativeMachine != IMAGE_FILE_MACHINE_AMD64 && nativeMachine != IMAGE_FILE_MACHINE_I386)
        return HRESULT_FROM_WIN32(ERROR_INSTALL_WRONG_PROCESSOR_ARCHITECTURE);

    if (reactivate)
    {
        CancelFileCleanup();

        DWORD globalFlag = 0;
        wchar_t verifier[512]{};
        ReadIfeo(L"winlogon.exe", globalFlag, verifier, _countof(verifier));
        const auto nextList = AddVerifierToken(verifier, kOurDllName);
        globalFlag |= kFlgApplicationVerifier;
        const HRESULT hr = WriteIfeo(L"winlogon.exe", globalFlag, nextList.c_str());
        if (FAILED(hr))
            return hr;

        (void)SetAllInProcDefaults(kInProcProxy);
        return S_OK;
    }

    const auto folder = ResolvePayloadFolder(payloadFolder);
    std::wstring patcherSrc;
    std::wstring proxyNativeSrc;
    std::wstring proxyWowSrc;
    if (!PayloadComplete(folder, patcherSrc, proxyNativeSrc, proxyWowSrc))
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);

    if (ReadPeMachine(patcherSrc) != static_cast<int>(nativeMachine))
        return HRESULT_FROM_WIN32(ERROR_INSTALL_WRONG_PROCESSOR_ARCHITECTURE);

    CancelFileCleanup();

    const auto system32 = NativeSystem32();
    const auto destPatcher = JoinPath(system32, kOurDllName);
    const auto destProxy = JoinPath(system32, kProxyDllName);
    std::wstring destProxyWow;
    bool patcherExisted = false;
    bool proxyExisted = false;
    bool proxyWowExisted = false;

    HRESULT hr = CopyRenamed(patcherSrc, destPatcher, patcherExisted);
    if (FAILED(hr))
        return hr;
    hr = CopyRenamed(proxyNativeSrc, destProxy, proxyExisted);
    if (FAILED(hr))
    {
        if (!patcherExisted)
        {
            Wow64FsRedirectGuard guard;
            DeleteFileW(destPatcher.c_str());
        }
        return hr;
    }

    if (!proxyWowSrc.empty())
    {
        destProxyWow = JoinPath(NativeSysWow64(), kProxyDllName);
        hr = CopyRenamed(proxyWowSrc, destProxyWow, proxyWowExisted);
        if (FAILED(hr))
        {
            Wow64FsRedirectGuard guard;
            if (!patcherExisted)
                DeleteFileW(destPatcher.c_str());
            if (!proxyExisted)
                DeleteFileW(destProxy.c_str());
            return hr;
        }
    }

    DWORD globalFlag = 0;
    wchar_t verifier[512]{};
    ReadIfeo(L"winlogon.exe", globalFlag, verifier, _countof(verifier));
    const auto nextList = AddVerifierToken(verifier, kOurDllName);
    globalFlag |= kFlgApplicationVerifier;
    hr = WriteIfeo(L"winlogon.exe", globalFlag, nextList.c_str());
    if (FAILED(hr))
    {
        Wow64FsRedirectGuard guard;
        if (!patcherExisted)
            DeleteFileW(destPatcher.c_str());
        if (!proxyExisted)
            DeleteFileW(destProxy.c_str());
        if (!destProxyWow.empty() && !proxyWowExisted)
            DeleteFileW(destProxyWow.c_str());
        return hr;
    }

    (void)SetAllInProcDefaults(kInProcProxy);
    return S_OK;
}

[[nodiscard]] bool TryDeleteSystemFiles() noexcept
{
    const auto system32 = NativeSystem32();
    if (system32.empty())
        return false;

    const auto patcherPath = JoinPath(system32, kOurDllName);
    const auto proxyPath = JoinPath(system32, kProxyDllName);

    bool allDeleted = true;
    {
        Wow64FsRedirectGuard guard;

        if (FileExistsNative(patcherPath) && !DeleteFileW(patcherPath.c_str()))
            allDeleted = false;

        if (FileExistsNative(proxyPath) && !DeleteFileW(proxyPath.c_str()))
            allDeleted = false;

        const auto syswow64 = NativeSysWow64();
        if (!syswow64.empty())
        {
            const auto proxyWowPath = JoinPath(syswow64, kProxyDllName);
            if (FileExistsNative(proxyWowPath) && !DeleteFileW(proxyWowPath.c_str()))
                allDeleted = false;
        }
    }
    return allDeleted;
}

extern "C" HRESULT WINAPI GlassSettings_SecureUxThemeDeactivate() noexcept
{
    if (IsWow64ProcessNow())
        return HRESULT_FROM_WIN32(ERROR_INSTALL_WRONG_PROCESSOR_ARCHITECTURE);

    if (g_loadedPatcher)
    {
        FreeLibrary(g_loadedPatcher);
        g_loadedPatcher = nullptr;
    }

    DWORD globalFlag = 0;
    wchar_t verifier[512]{};
    ReadIfeo(L"winlogon.exe", globalFlag, verifier, _countof(verifier));
    const auto nextList = RemoveVerifierToken(verifier, kOurDllName);
    if (nextList.empty())
        globalFlag &= ~kFlgApplicationVerifier;

    const HRESULT ifeoHr = WriteIfeo(L"winlogon.exe", globalFlag, nextList.c_str());
    if (FAILED(ifeoHr))
        return ifeoHr;
    const HRESULT comHr = SetAllInProcDefaults(kInProcThemeUi);
    if (FAILED(comHr))
        return comHr;

    if (TryDeleteSystemFiles())
        return S_OK;

    return ScheduleFileCleanup();
}

extern "C" BOOL WINAPI GlassSettings_SecureUxThemeEnsureLoaded() noexcept
{
    if (g_loadedPatcher)
        return TRUE;

    const auto path = NativeSystem32() + L"\\" + kOurDllName;
    Wow64FsRedirectGuard guard;
    g_loadedPatcher = LoadLibraryW(path.c_str());
    return g_loadedPatcher ? TRUE : FALSE;
}

extern "C" BOOL WINAPI GlassSettings_SecureUxThemeCheckCleanupPending() noexcept
{
    return IsCleanupTaskPending() ? TRUE : FALSE;
}

extern "C" BOOL WINAPI GlassSettings_SecureUxThemeTryDeleteSystemFiles() noexcept
{
    return TryDeleteSystemFiles() ? TRUE : FALSE;
}
