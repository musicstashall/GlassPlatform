#include <Windows.h>

#include <DebugLog.h>
#include <GlassModulePath.h>
#include <GlassShellMenuConstants.h>
#include <GlassShellRegistration.h>
#include <Helpers/SystemHelper.h>

#include "ShellSparsePackage.h"

#include <ShlObj.h>
#include <ShellApi.h>
#include <Shlwapi.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "shell32.lib")

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

namespace
{
    std::atomic<bool> g_shellExtOpInProgress{ false };

    enum class OpMessageId : int
    {
        None = 0,
        Success = 1,
        TimedOut = 2,
        InconsistentState = 3,
        FailedToStart = 4,
        FailedToApply = 5,
        NeedsExplorerRestart = 6,
        SparseArtifactsMissing = 7,
        SparseDevCertFailed = 8,
        SparseDeploymentFailed = 9,
        FailedToRegister = 10,
        FailedToUnregister = 11,
        SparseExternalLocationConflict = 12,
    };

    std::atomic<int> g_lastOpMessageId{ static_cast<int>(OpMessageId::None) };

    struct ShellExtOpParams
    {
        bool registerNotUnregister = false;
    };

    void SetLastOpMessage(OpMessageId id) noexcept
    {
        g_lastOpMessageId.store(static_cast<int>(id), std::memory_order_release);
    }

    [[nodiscard]] bool ResolveExeOutputDirectory(wchar_t* out, size_t cch) noexcept
    {
        if (!out || cch == 0)
            return false;
        out[0] = L'\0';
        wchar_t exe[MAX_PATH]{};
        if (::GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0)
            return false;
        wchar_t* sl = wcsrchr(exe, L'\\');
        if (!sl)
            return false;
        *(sl + 1) = L'\0';
        return wcscpy_s(out, cch, exe) == 0;
    }


    [[nodiscard]] bool ResolveBundledGlassShellExtDll(
        wchar_t* out, size_t cch, const wchar_t* exactPlatformRoot) noexcept
    {
        if (!out || cch == 0)
            return false;
        if (exactPlatformRoot)
        {
            // Setup/Unins supply the already resolved root. Do not select a
            // different file near rundll32, the temporary helper, or another
            // registry value when this exact path is unavailable.
            const std::wstring exact = GlassShared::GlassModulePath::ResolveGlassModulePath(
                exactPlatformRoot, L"GlassShellExt.dll", GlassShared::GlassModulePath::NativeModuleArch());
            return !exact.empty() && wcscpy_s(out, cch, exact.c_str()) == 0;
        }
        std::wstring path =
            GlassShared::GlassModulePath::ResolveGlassModulePathNextToModule(nullptr, L"GlassShellExt.dll");
        if (path.empty())
        {
            path = GlassShared::GlassModulePath::ResolveGlassModulePathNextToModule(
                reinterpret_cast<HMODULE>(&__ImageBase), L"GlassShellExt.dll");
        }
        if (path.empty())
        {
            const wchar_t* valName =
#if defined(_WIN64)
                L"InstallPathX64";
            constexpr REGSAM machineView = KEY_WOW64_64KEY;
#else
                L"InstallPathX86";
            constexpr REGSAM machineView = KEY_WOW64_32KEY;
#endif
            const auto tryValue = [&](HKEY hive, REGSAM view, const wchar_t* name) noexcept {
                HKEY key = nullptr;
                if (::RegOpenKeyExW(hive, L"Software\\GlassPlatform", 0, KEY_READ | view, &key) != ERROR_SUCCESS)
                    return;
                wchar_t value[MAX_PATH]{};
                DWORD bytes = sizeof(value), type = 0;
                const LSTATUS status = ::RegQueryValueExW(key, name, nullptr, &type,
                    reinterpret_cast<LPBYTE>(value), &bytes);
                ::RegCloseKey(key);
                if (status == ERROR_SUCCESS && type == REG_SZ && value[0])
                    path = GlassShared::GlassModulePath::ResolveGlassModulePath(
                        value, L"GlassShellExt.dll", GlassShared::GlassModulePath::NativeModuleArch());
            };
            tryValue(HKEY_LOCAL_MACHINE, machineView, valName);
            if (path.empty()) tryValue(HKEY_LOCAL_MACHINE, machineView, L"InstallPath");
            if (path.empty()) tryValue(HKEY_CURRENT_USER, 0, valName);
            if (path.empty()) tryValue(HKEY_CURRENT_USER, 0, L"InstallPath");
        }
        if (path.empty())
            return false;
        return wcscpy_s(out, cch, path.c_str()) == 0;
    }

    [[nodiscard]] bool RunHiddenCommandLine(_In_z_ wchar_t* commandLine, DWORD timeoutMs) noexcept
    {
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, commandLine, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                &pi))
            return false;

        CloseHandle(pi.hThread);
        const DWORD wait = WaitForSingleObject(pi.hProcess, timeoutMs);
        DWORD exitCode = 1;
        if (wait == WAIT_TIMEOUT)
        {
            (void)::TerminateProcess(pi.hProcess, 1);
            (void)::WaitForSingleObject(pi.hProcess, 2000);
        }
        (void)GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        if (wait != WAIT_OBJECT_0)
            return false;
        return exitCode == 0;
    }

    [[nodiscard]] bool RunRegsvr32(bool unregister, const wchar_t* exactPlatformRoot = nullptr) noexcept
    {
        wchar_t dllPath[MAX_PATH]{};
        if (!ResolveBundledGlassShellExtDll(dllPath, _countof(dllPath), exactPlatformRoot))
            return false;

        wchar_t sysdir[MAX_PATH]{};
        if (GetSystemDirectoryW(sysdir, MAX_PATH) == 0)
            return false;

        std::wstring cmd = L"\"";
        cmd += sysdir;
        cmd += L"\\regsvr32.exe\" ";
        cmd += unregister ? L"/u /s \"" : L"/s \"";
        cmd += dllPath;
        cmd += L"\"";

        std::vector<wchar_t> buf(cmd.begin(), cmd.end());
        buf.push_back(L'\0');
        return RunHiddenCommandLine(buf.data(), 20000);
    }

    [[nodiscard]] bool HasExistingShellRegistrationKeys() noexcept
    {
        using namespace GlassShared::GlassShellMenu;
        std::vector<std::wstring> keys;
        for (const wchar_t* root : {L"DesktopBackground", L"Directory\\Background"})
        {
            const std::wstring prefix = std::wstring(L"Software\\Classes\\") + root;
            keys.push_back(prefix + L"\\ShellEx\\ContextMenuHandlers\\" + kHandlerSubkeyName);
            keys.push_back(prefix + L"\\Shell\\" + kDesktopToggleVerbSubkey);
            keys.push_back(prefix + L"\\Shell\\" + kDesktopOpenConfigVerbSubkey);
        }
        for (const wchar_t* clsid : {kClsidStringBraced, kToggleExplorerClsidBraced, kOpenConfigExplorerClsidBraced})
            keys.push_back(std::wstring(L"Software\\Classes\\CLSID\\") + clsid);
        for (const auto& path : keys)
        {
            HKEY key = nullptr;
            const LSTATUS status = ::RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &key);
            if (key) ::RegCloseKey(key);
            // Unknown/access-denied means preserve, not replace.
            if (status == ERROR_SUCCESS || (status != ERROR_FILE_NOT_FOUND && status != ERROR_PATH_NOT_FOUND))
                return true;
        }
        return false;
    }

    [[nodiscard]] bool IsRegistryShellExtensionRegistered() noexcept
    {
        return Shared::GlassShellRegistration::IsLegacyContextMenuHandlerPresent();
    }

    [[nodiscard]] bool HasMatchingLegacyShellRegistration(const wchar_t* root) noexcept
    {
        if (!root || !root[0] || !IsRegistryShellExtensionRegistered())
            return false;
        const std::wstring expected=GlassShared::GlassModulePath::ResolveGlassModulePath(
            root,L"GlassShellExt.dll",GlassShared::GlassModulePath::NativeModuleArch());
        if (expected.empty())
            return false;
        const std::wstring key=std::wstring(L"Software\\Classes\\CLSID\\")+
            GlassShared::GlassShellMenu::kClsidStringBraced+L"\\InprocServer32";
        wchar_t value[MAX_PATH]{};
        DWORD bytes=sizeof(value);
        if (::RegGetValueW(HKEY_CURRENT_USER,key.c_str(),nullptr,RRF_RT_REG_SZ,
                nullptr,value,&bytes)!=ERROR_SUCCESS || !value[0])
            return false;
        wchar_t a[MAX_PATH]{},b[MAX_PATH]{};
        const DWORD na=GetFullPathNameW(value,_countof(a),a,nullptr);
        const DWORD nb=GetFullPathNameW(expected.c_str(),_countof(b),b,nullptr);
        return na && na<_countof(a) && nb && nb<_countof(b) && _wcsicmp(a,b)==0;
    }

    [[nodiscard]] bool ExplorerCommandsBlockedOrUnknown() noexcept
    {
        constexpr wchar_t key[]=L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Blocked";
        for (const wchar_t* clsid : {GlassShared::GlassShellMenu::kToggleExplorerClsidBraced,
                                     GlassShared::GlassShellMenu::kOpenConfigExplorerClsidBraced})
        {
            DWORD bytes=0;
            const LSTATUS result=::RegGetValueW(HKEY_CURRENT_USER,key,clsid,RRF_RT_ANY,nullptr,nullptr,&bytes);
            if (result!=ERROR_FILE_NOT_FOUND && result!=ERROR_PATH_NOT_FOUND)
                return true; // present, or not safely inspectable
        }
        return false;
    }

    void RemoveLegacyContextMenuHandlerOnly() noexcept
    {
        Shared::GlassShellRegistration::RemoveLegacyContextMenuHandler();
    }

    void RemoveLegacyContextMenuHandlerForUnregister(const wchar_t* exactPlatformRoot = nullptr) noexcept
    {
        RemoveLegacyContextMenuHandlerOnly();
        (void)RunRegsvr32(true, exactPlatformRoot);
        RemoveLegacyContextMenuHandlerOnly();
    }

    [[nodiscard]] bool IsWindows11OrGreater() noexcept
    {
        return Shared::SystemHelper::g_buildNumber >= 22000;
    }

    void CleanupLegacySparseKnownFlags() noexcept
    {
        static std::once_flag once;
        std::call_once(once, []() noexcept
        {
            // Устаревший hive placement (Win32-конфигуратор); удаляем флаг sparse после миграции.
            static constexpr wchar_t kLegacyPlacementHive[] = L"Software\\GlassProject\\GlassConfig";
            HKEY hKey = nullptr;
            if (::RegOpenKeyExW(HKEY_CURRENT_USER, kLegacyPlacementHive, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS)
            {
                (void)::RegDeleteValueW(hKey, L"ShellExtSparseKnownRegistered");
                ::RegCloseKey(hKey);
            }
        });
    }

    [[nodiscard]] bool IsSparseShellPackageRegistered() noexcept
    {
        CleanupLegacySparseKnownFlags();
        return GlassSettings::ShellSparsePackage::IsSparsePackageInstalledForCurrentUser();
    }

    [[nodiscard]] bool IsWin11SparseRegistrationHealthy(bool sparsePm) noexcept
    {
        return sparsePm && !IsRegistryShellExtensionRegistered();
    }

    void VerifyWin11RegistrationHasNoLegacyHandlers() noexcept
    {
        if (!IsRegistryShellExtensionRegistered())
            return;

        DebugLog(
            DBG_WARNING,
            L"[GlassSettings][ShellExt] Win11: после register остались legacy ContextMenuHandlers");
        RemoveLegacyContextMenuHandlerOnly();
    }

    [[nodiscard]] bool IsWin11SparseUnregisterComplete() noexcept
    {
        return !IsRegistryShellExtensionRegistered() &&
               !GlassSettings::ShellSparsePackage::IsSparsePackageInstalledForCurrentUser();
    }

    [[nodiscard]] bool IsUnregisterArtifactsAbsent() noexcept
    {
        return !IsRegistryShellExtensionRegistered() &&
               !GlassSettings::ShellSparsePackage::IsSparsePackageInstalledForCurrentUser();
    }

    [[nodiscard]] bool WaitForBundledRegistrationState(bool expectedRegistered, DWORD timeoutMs, DWORD pollMs) noexcept
    {
        const DWORD step = pollMs == 0 ? 250 : pollMs;
        const DWORD started = GetTickCount();
        while (true)
        {
            const bool win11RegisterWait = expectedRegistered && IsWindows11OrGreater();
            const bool win11UnregisterWait = !expectedRegistered && IsWindows11OrGreater();

            if (win11RegisterWait)
            {
                RemoveLegacyContextMenuHandlerOnly();
                if (IsWin11SparseRegistrationHealthy(
                        GlassSettings::ShellSparsePackage::IsSparsePackageInstalledForCurrentUser()))
                    return true;
            }
            else if (win11UnregisterWait)
            {
                if (IsWin11SparseUnregisterComplete())
                    return true;
            }
            else
            {
                const bool actual = IsRegistryShellExtensionRegistered();
                if (actual == expectedRegistered)
                    return true;
            }

            if (!expectedRegistered && IsUnregisterArtifactsAbsent())
                return true;

            const DWORD elapsed = GetTickCount() - started;
            if (elapsed >= timeoutMs)
            {
                if (!expectedRegistered && IsUnregisterArtifactsAbsent())
                    return true;
                SetLastOpMessage(OpMessageId::TimedOut);
                return false;
            }
            ::Sleep(step);
        }
    }

    void SetLastOpMessageFromSparseError() noexcept
    {
        switch (GlassSettings::ShellSparsePackage::GetLastSparsePackageError())
        {
        case GlassSettings::ShellSparsePackage::SparsePackageError::ArtifactsMissing:
            SetLastOpMessage(OpMessageId::SparseArtifactsMissing);
            break;
        case GlassSettings::ShellSparsePackage::SparsePackageError::DevCertFailed:
            SetLastOpMessage(OpMessageId::SparseDevCertFailed);
            break;
        case GlassSettings::ShellSparsePackage::SparsePackageError::DeploymentFailed:
            SetLastOpMessage(OpMessageId::SparseDeploymentFailed);
            break;
        case GlassSettings::ShellSparsePackage::SparsePackageError::ExternalLocationConflict:
            SetLastOpMessage(OpMessageId::SparseExternalLocationConflict);
            break;
        default:
            SetLastOpMessage(OpMessageId::FailedToRegister);
            break;
        }
    }

    [[nodiscard]] bool RegisterBundledDllQuietImpl(bool provisionDevCertificate = true,
        const wchar_t* exactPlatformRoot = nullptr) noexcept
    {
        SetLastOpMessage(OpMessageId::None);
        if (IsWindows11OrGreater())
        {
            if (!GlassSettings::ShellSparsePackage::RegisterSparsePackageQuiet(provisionDevCertificate, exactPlatformRoot))
            {
                DebugLog(
                    DBG_ERROR,
                    L"[GlassSettings][ShellExt] register: sparse package failed (Win11, без regsvr32 fallback)");
                SetLastOpMessageFromSparseError();
                return false;
            }

            if (provisionDevCertificate) {
                RemoveLegacyContextMenuHandlerOnly();
                VerifyWin11RegistrationHasNoLegacyHandlers();
            }
            SetLastOpMessage(OpMessageId::Success);
            return true;
        }

        if (!RunRegsvr32(false, exactPlatformRoot))
        {
            SetLastOpMessage(OpMessageId::FailedToRegister);
            return false;
        }

        SetLastOpMessage(OpMessageId::Success);
        return true;
    }

    [[nodiscard]] bool UnregisterBundledDllQuietImpl(const wchar_t* exactPlatformRoot = nullptr) noexcept
    {
        SetLastOpMessage(OpMessageId::None);

        if (exactPlatformRoot)
        {
            // The versioned Setup/Unins route must never use the general
            // cross-user/package-stub cleanup used by the Settings UI.
            if (IsWindows11OrGreater())
                return GlassSettings::ShellSparsePackage::UnregisterInstallerSparsePackageQuiet(exactPlatformRoot);
            if (!HasExistingShellRegistrationKeys())
                return true;
            return HasMatchingLegacyShellRegistration(exactPlatformRoot) &&
                RunRegsvr32(true,exactPlatformRoot) && !IsRegistryShellExtensionRegistered();
        }

        if (IsWindows11OrGreater())
        {
            if (IsSparseShellPackageRegistered() ||
                GlassSettings::ShellSparsePackage::IsSparsePackageInstalledForCurrentUser())
            {
                if (!GlassSettings::ShellSparsePackage::UnregisterSparsePackageQuiet(exactPlatformRoot))
                {
                    SetLastOpMessageFromSparseError();
                    if (static_cast<OpMessageId>(g_lastOpMessageId.load(std::memory_order_acquire)) ==
                        OpMessageId::None)
                        SetLastOpMessage(OpMessageId::FailedToUnregister);
                    if (GlassSettings::ShellSparsePackage::IsSparsePackageInstalledForCurrentUser())
                        return false;
                }
            }

            RemoveLegacyContextMenuHandlerOnly();

            if (IsRegistryShellExtensionRegistered())
            {
                SetLastOpMessage(OpMessageId::InconsistentState);
                return false;
            }

            if (GlassSettings::ShellSparsePackage::IsSparsePackageInstalledForCurrentUser())
            {
                SetLastOpMessage(OpMessageId::InconsistentState);
                return false;
            }

            SetLastOpMessage(OpMessageId::Success);
            return true;
        }

        if (IsSparseShellPackageRegistered() ||
            GlassSettings::ShellSparsePackage::IsSparsePackageInstalledForCurrentUser())
        {
            if (!GlassSettings::ShellSparsePackage::UnregisterSparsePackageQuiet(exactPlatformRoot))
            {
                SetLastOpMessageFromSparseError();
                if (static_cast<OpMessageId>(g_lastOpMessageId.load(std::memory_order_acquire)) ==
                    OpMessageId::None)
                    SetLastOpMessage(OpMessageId::FailedToUnregister);
                if (GlassSettings::ShellSparsePackage::IsSparsePackageInstalledForCurrentUser())
                    return false;
            }
        }

        if (IsRegistryShellExtensionRegistered())
            RemoveLegacyContextMenuHandlerForUnregister(exactPlatformRoot);

        if (IsRegistryShellExtensionRegistered())
        {
            SetLastOpMessage(OpMessageId::InconsistentState);
            return false;
        }

        if (!GlassSettings::ShellSparsePackage::IsSparsePackageInstalledForCurrentUser())
        {
            SetLastOpMessage(OpMessageId::Success);
            return true;
        }

        SetLastOpMessage(OpMessageId::InconsistentState);
        return false;
    }

    DWORD WINAPI ShellExtWorkerThread(LPVOID param) noexcept
    {
        const auto params = std::unique_ptr<ShellExtOpParams>{ static_cast<ShellExtOpParams*>(param) };
        if (!params)
        {
            SetLastOpMessage(OpMessageId::FailedToStart);
            g_shellExtOpInProgress.store(false, std::memory_order_release);
            return 1;
        }

        bool ok = params->registerNotUnregister ? RegisterBundledDllQuietImpl() : UnregisterBundledDllQuietImpl();
        if (ok && params->registerNotUnregister)
            ok = WaitForBundledRegistrationState(true, 15000, 250);
        else if (ok && !params->registerNotUnregister)
            ok = WaitForBundledRegistrationState(false, 3000, 250);

        if (!ok && static_cast<OpMessageId>(g_lastOpMessageId.load(std::memory_order_acquire)) == OpMessageId::None)
            SetLastOpMessage(params->registerNotUnregister ? OpMessageId::FailedToRegister : OpMessageId::FailedToUnregister);

        g_shellExtOpInProgress.store(false, std::memory_order_release);
        return ok ? 0 : 1;
    }

    std::wstring ReadRundllCommand(LPWSTR cmdLine)
    {
        if (!cmdLine || !cmdLine[0]) return {};
        const auto first = static_cast<unsigned>(cmdLine[0]);
        const bool wideAscii = first < 128
            && (cmdLine[0] == L'/' || cmdLine[0] == L'-' || cmdLine[0] == L'"' || cmdLine[0] == L' ');
        if (wideAscii) return cmdLine;
        const char* ansi = reinterpret_cast<const char*>(cmdLine);
        const int count = ::MultiByteToWideChar(CP_ACP, 0, ansi, -1, nullptr, 0);
        if (count <= 1) return {};
        std::wstring parsed(static_cast<size_t>(count), L'\0');
        if (::MultiByteToWideChar(CP_ACP, 0, ansi, -1, parsed.data(), count) != count) return {};
        parsed.pop_back(); // MultiByteToWideChar included the terminator.
        return parsed;
    }

    bool ParseInstallerShellArguments(LPWSTR cmdLine, bool& unregister, std::wstring& root) noexcept
    {
        unregister = false;
        root.clear();
        try
        {
            const std::wstring args = L"GlassShellHelper " + ReadRundllCommand(cmdLine);
            int count = 0;
            LPWSTR* values = ::CommandLineToArgvW(args.c_str(), &count);
            if (!values) return false;
            struct Guard { LPWSTR* p; ~Guard() { ::LocalFree(p); } } guard{values};
            if (count != 4 || _wcsicmp(values[2], L"/installroot") != 0 || !values[3][0] ||
                ::PathIsRelativeW(values[3])) return false;
            if (_wcsicmp(values[1], L"/unregister") == 0) unregister = true;
            else if (_wcsicmp(values[1], L"/register") != 0) return false;
            root = values[3];
            return true;
        }
        catch (...) { root.clear(); return false; }
    }

}

extern "C" __declspec(dllexport) BOOL WINAPI GlassSettings_BeginShellExtensionChange(BOOL registerNotUnregister) noexcept
{
    bool expected = false;
    if (!g_shellExtOpInProgress.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire))
    {
        SetLastOpMessage(OpMessageId::FailedToStart);
        return FALSE;
    }

    auto params = std::make_unique<ShellExtOpParams>();
    params->registerNotUnregister = registerNotUnregister != FALSE;

    HANDLE hThread = ::CreateThread(nullptr, 0, ShellExtWorkerThread, params.release(), 0, nullptr);
    if (!hThread)
    {
        SetLastOpMessage(OpMessageId::FailedToStart);
        g_shellExtOpInProgress.store(false, std::memory_order_release);
        return FALSE;
    }

    ::CloseHandle(hThread);
    return TRUE;
}

extern "C" __declspec(dllexport) BOOL WINAPI GlassSettings_IsShellExtensionOperationInProgress() noexcept
{
    return g_shellExtOpInProgress.load(std::memory_order_acquire) ? TRUE : FALSE;
}

extern "C" __declspec(dllexport) DWORD WINAPI GlassSettings_GetShellExtensionLastOpMessageId() noexcept
{
    return static_cast<DWORD>(g_lastOpMessageId.load(std::memory_order_acquire));
}

extern "C" __declspec(dllexport) DWORD WINAPI GlassSettings_CopyShellExtensionLastErrorMessage(
    wchar_t* buffer,
    DWORD cch) noexcept
{
    if (!buffer || cch == 0)
        return 0;

    buffer[0] = L'\0';
    return 0;
}

extern "C" __declspec(dllexport) void WINAPI GlassSettings_NotifyShellCachesAfterRegistrationChange() noexcept
{
    ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);

    wchar_t desktopDir[MAX_PATH]{};
    if (SUCCEEDED(::SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, SHGFP_TYPE_CURRENT, desktopDir)))
        ::SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATH | SHCNF_IDLIST, desktopDir, nullptr);
}

extern "C" __declspec(dllexport) void CALLBACK GlassSettings_ShellExtRegisterW(
    HWND,
    HINSTANCE,
    LPWSTR cmdLine,
    int) noexcept
{
#ifdef _M_IX86
#pragma comment(linker, "/EXPORT:GlassSettings_ShellExtRegisterW=_GlassSettings_ShellExtRegisterW@16")
#endif
    const std::wstring parsed = ReadRundllCommand(cmdLine);
    const bool unregister = parsed.find(L"/unregister") != std::wstring::npos
        || parsed.find(L"-u") != std::wstring::npos;
    const bool setup = parsed.find(L"/setup") != std::wstring::npos;
    if (setup && !unregister && (HasExistingShellRegistrationKeys() ||
        GlassSettings::ShellSparsePackage::IsSparsePackageInstalledForCurrentUser())) {
        // Explicit sentinel: distinguish an intentional no-op from rundll32 loader errors.
        ::ExitProcess(0x47505245u);
    }
    const bool ok = unregister ? UnregisterBundledDllQuietImpl() : RegisterBundledDllQuietImpl(!setup);
    GlassSettings_NotifyShellCachesAfterRegistrationChange();
    ::ExitProcess(ok ? 0u : 1u);
}

// Versioned entry point: unlike the legacy GlassSettings UI API, the installer
// must use its resolved InstallPath without searching other candidate folders.
extern "C" __declspec(dllexport) void CALLBACK GlassSettings_ShellExtRegisterSetupV1W(
    HWND, HINSTANCE, LPWSTR cmdLine, int) noexcept
{
#ifdef _M_IX86
#pragma comment(linker, "/EXPORT:GlassSettings_ShellExtRegisterSetupV1W=_GlassSettings_ShellExtRegisterSetupV1W@16")
#endif
    bool unregister = false;
    std::wstring root;
    if (!ParseInstallerShellArguments(cmdLine, unregister, root)) ::ExitProcess(1u);
    if (!unregister)
    {
        // Preserve existing registrations only when they actually match the
        // selected root. Never silently report an unrelated/partial one as OK.
        constexpr DWORD kExisting=0x47505245u;
        constexpr DWORD kConflict=0x47504346u;
        const bool keys=HasExistingShellRegistrationKeys();
        if (IsWindows11OrGreater())
        {
            using GlassSettings::ShellSparsePackage::InstallerSparseState;
            const auto sparse=GlassSettings::ShellSparsePackage::QueryInstallerSparseState(root.c_str());
            if (sparse==InstallerSparseState::Matching && !keys && !ExplorerCommandsBlockedOrUnknown() &&
                GlassSettings::ShellSparsePackage::InstallerSparseHasVerifiedMenu(root.c_str()))
                ::ExitProcess(kExisting);
            if (sparse!=InstallerSparseState::Absent || keys)
                ::ExitProcess(kConflict);
        }
        else if (keys)
        {
            ::ExitProcess(HasMatchingLegacyShellRegistration(root.c_str()) ? kExisting : kConflict);
        }
    }
    else
    {
        // Both Unins and rollback must refuse to remove a registration that
        // belongs to a different root/user or cannot be inspected safely.
        constexpr DWORD kConflict=0x47504346u;
        const bool keys=HasExistingShellRegistrationKeys();
        if (IsWindows11OrGreater())
        {
            using GlassSettings::ShellSparsePackage::InstallerSparseState;
            const auto sparse=GlassSettings::ShellSparsePackage::QueryInstallerSparseState(root.c_str());
            if ((sparse!=InstallerSparseState::Absent && sparse!=InstallerSparseState::Matching) || keys)
                ::ExitProcess(kConflict);
        }
        else if (keys && !HasMatchingLegacyShellRegistration(root.c_str()))
        {
            ::ExitProcess(kConflict);
        }
    }
    const bool ok = unregister ? UnregisterBundledDllQuietImpl(root.c_str())
                               : RegisterBundledDllQuietImpl(false, root.c_str());
    GlassSettings_NotifyShellCachesAfterRegistrationChange();
    ::ExitProcess(ok ? 0u : 1u);
}
