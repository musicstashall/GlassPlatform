#include <Windows.h>

#include <DebugLog.h>
#include <GlassModulePath.h>
#include <GlassShellMenuConstants.h>
#include <GlassShellRegistration.h>
#include <Helpers/SystemHelper.h>

#include "ShellSparsePackage.h"

#include <ShlObj.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "shell32.lib")

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

    [[nodiscard]] bool ResolveBundledGlassShellExtDll(wchar_t* out, size_t cch) noexcept
    {
        if (!out || cch == 0)
            return false;
        const std::wstring path =
            GlassShared::GlassModulePath::ResolveGlassModulePathNextToModule(nullptr, L"GlassShellExt.dll");
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

    [[nodiscard]] bool RunRegsvr32(bool unregister) noexcept
    {
        wchar_t dllPath[MAX_PATH]{};
        if (!ResolveBundledGlassShellExtDll(dllPath, _countof(dllPath)))
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

    [[nodiscard]] bool IsRegistryShellExtensionRegistered() noexcept
    {
        return Shared::GlassShellRegistration::IsLegacyContextMenuHandlerPresent();
    }

    void RemoveLegacyContextMenuHandlerOnly() noexcept
    {
        Shared::GlassShellRegistration::RemoveLegacyContextMenuHandler();
    }

    void RemoveLegacyContextMenuHandlerForUnregister() noexcept
    {
        RemoveLegacyContextMenuHandlerOnly();
        (void)RunRegsvr32(true);
        RemoveLegacyContextMenuHandlerOnly();
    }

    [[nodiscard]] bool FindSparsePackageByPrefixInRepoKey(const wchar_t* subkey) noexcept
    {
        if (!subkey || !*subkey)
            return false;

        HKEY hKey = nullptr;
        if (::RegOpenKeyExW(HKEY_CURRENT_USER, subkey, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
            return false;

        const std::wstring prefix = std::wstring(GlassShared::GlassShellMenu::kSparsePackageName) + L"_";
        bool found = false;
        for (DWORD index = 0;; ++index)
        {
            wchar_t name[256]{};
            DWORD cch = _countof(name);
            const LONG e = ::RegEnumKeyExW(hKey, index, name, &cch, nullptr, nullptr, nullptr, nullptr);
            if (e == ERROR_NO_MORE_ITEMS)
                break;
            if (e != ERROR_SUCCESS)
                continue;
            if (_wcsnicmp(name, prefix.c_str(), prefix.size()) == 0)
            {
                found = true;
                break;
            }
        }

        ::RegCloseKey(hKey);
        return found;
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

        static constexpr wchar_t kPackagesKey[] =
            L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\Repository\\Packages";
        static constexpr wchar_t kFamiliesKey[] =
            L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\Repository\\Families";

        if (FindSparsePackageByPrefixInRepoKey(kPackagesKey))
            return true;
        if (FindSparsePackageByPrefixInRepoKey(kFamiliesKey))
            return true;

        return GlassSettings::ShellSparsePackage::IsSparsePackageRegisteredForCurrentUser();
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

    [[nodiscard]] bool RegisterBundledDllQuietImpl() noexcept
    {
        SetLastOpMessage(OpMessageId::None);
        if (IsWindows11OrGreater())
        {
            if (!GlassSettings::ShellSparsePackage::RegisterSparsePackageQuiet())
            {
                DebugLog(
                    DBG_ERROR,
                    L"[GlassSettings][ShellExt] register: sparse package failed (Win11, без regsvr32 fallback)");
                SetLastOpMessageFromSparseError();
                return false;
            }

            RemoveLegacyContextMenuHandlerOnly();
            VerifyWin11RegistrationHasNoLegacyHandlers();
            SetLastOpMessage(OpMessageId::Success);
            return true;
        }

        if (!RunRegsvr32(false))
        {
            SetLastOpMessage(OpMessageId::FailedToRegister);
            return false;
        }

        SetLastOpMessage(OpMessageId::Success);
        return true;
    }

    [[nodiscard]] bool UnregisterBundledDllQuietImpl() noexcept
    {
        SetLastOpMessage(OpMessageId::None);

        if (IsWindows11OrGreater())
        {
            if (IsSparseShellPackageRegistered() ||
                GlassSettings::ShellSparsePackage::IsSparsePackageInstalledForCurrentUser())
            {
                if (!GlassSettings::ShellSparsePackage::UnregisterSparsePackageQuiet())
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
            if (!GlassSettings::ShellSparsePackage::UnregisterSparsePackageQuiet())
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
            RemoveLegacyContextMenuHandlerForUnregister();

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
