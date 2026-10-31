#include "CompatPch.h"
#include "CoreWindowHostInject.h"
#include "GlassCompat.h"
#include "HostPidHelper.h"

#include <GlassMessages.h>
#include <ForceUnloadGlassModules.h>
#include <InjectProcessMap.h>
#include <RootBackdropPolicy.h>
#include <GlassUlTrace.h>
#include <aclapi.h>
#include <appmodel.h>
#include <sddl.h>
#include <shobjidl.h>
#include <tlhelp32.h>

#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")
#include <dwmapi.h>

namespace
{
    struct ThreadHook
    {
        DWORD         pid = 0;
        DWORD         tid = 0;
        HWINEVENTHOOK hook = nullptr; // CREATE..SHOW
        HWINEVENTHOOK uncloakHook = nullptr; // UNCLOAKED (вне диапазона CREATE..SHOW)
        // QS ControlCenter: SHOW при cx=cy=0; размер → LOCATIONCHANGE → Attach/IXDE.
        HWINEVENTHOOK locationHook = nullptr;
    };

    std::mutex g_mx;
    std::unordered_map<unsigned long long, ThreadHook> g_hooks; // key = (pid<<32)|tid
    std::unordered_set<DWORD> g_bootstrappedPids;
    std::unordered_set<DWORD> g_remoteLoadPids;
    std::unordered_map<DWORD, DWORD> g_remoteGuiTids; // pid → GlassGuiThread tid (bootstrap exit)
    // HR12: EnumWindows не видит SEH AppContainer (hwndNull:0 @84414) — копим HWND из OOC/SHOW.
    std::unordered_map<DWORD, std::vector<HWND>> g_remoteCwHwnds;
    std::unordered_set<DWORD> g_attachExistingPosted; // HR16/17: один AttachExisting на pid
    std::unordered_map<DWORD, HANDLE> g_cwForceEvents; // host→SEH: Stop в AC не будит Wait
    // Stop hang: Refresh блокировался на Wait(8s)×failed SEH/SearchHost и не видел StopEvent.
    std::unordered_map<DWORD, ULONGLONG> g_remoteLoadFailUntilMs; // pid → GetTickCount64 cooldown
    // HR18: пакеты, для которых мы вызвали EnableDebugging (нужен парный DisableDebugging).
    std::unordered_set<std::wstring> g_sehPlmDebugPkgs;
    bool g_aclDone = false;

    [[nodiscard]] bool IsPlatformStopSignaled() noexcept
    {
        HANDLE hStop = OpenEventW(SYNCHRONIZE, FALSE, kStopEventName);
        if (!hStop)
            return false;
        const bool signaled = WaitForSingleObject(hStop, 0) == WAIT_OBJECT_0;
        CloseHandle(hStop);
        return signaled;
    }

    [[nodiscard]] bool IsRemoteLoadCoolingDown(DWORD pid) noexcept
    {
        std::lock_guard lock(g_mx);
        const auto it = g_remoteLoadFailUntilMs.find(pid);
        if (it == g_remoteLoadFailUntilMs.end())
            return false;
        return GetTickCount64() < it->second;
    }

    void MarkRemoteLoadFailed(DWORD pid) noexcept
    {
        std::lock_guard lock(g_mx);
        g_remoteLoadFailUntilMs[pid] = GetTickCount64() + 30000ull;
    }

    void ClearRemoteLoadFail(DWORD pid) noexcept
    {
        std::lock_guard lock(g_mx);
        g_remoteLoadFailUntilMs.erase(pid);
    }

    void CwForceEventName(DWORD pid, wchar_t (&out)[80]) noexcept
    {
        swprintf_s(out, L"Local\\GlassCompat.CwForce.%u", pid);
    }

    [[nodiscard]] PSECURITY_DESCRIPTOR CreateWdAcSd() noexcept
    {
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:(A;;GA;;;WD)(A;;GA;;;AC)",
                SDDL_REVISION_1,
                &sd,
                nullptr))
        {
            return nullptr;
        }
        return sd;
    }

    // H54: SEH OpenEvent(Stop) ok, но Wait не просыпается (нет cw_watcher_stop_seen).
    // Хост создаёт отдельный event и SetEvent в ForceUnload.
    void EnsureCwForceEvent(DWORD pid) noexcept
    {
        if (!pid)
            return;
        std::lock_guard lock(g_mx);
        if (g_cwForceEvents.contains(pid))
            return;

        PSECURITY_DESCRIPTOR sd = CreateWdAcSd();
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = sd;

        wchar_t name[80]{};
        CwForceEventName(pid, name);
        HANDLE hEvt = CreateEventW(sd ? &sa : nullptr, TRUE, FALSE, name);
        if (sd)
            LocalFree(sd);
        if (hEvt)
            g_cwForceEvents.emplace(pid, hEvt);
    }

    void SignalCwForceEvent(DWORD pid) noexcept
    {
        HANDLE hEvt = nullptr;
        {
            std::lock_guard lock(g_mx);
            if (const auto it = g_cwForceEvents.find(pid); it != g_cwForceEvents.end())
                hEvt = it->second;
        }
        if (hEvt)
            SetEvent(hEvt);
        else
        {
            wchar_t name[80]{};
            CwForceEventName(pid, name);
            if (HANDLE h = OpenEventW(EVENT_MODIFY_STATE, FALSE, name))
            {
                SetEvent(h);
                CloseHandle(h);
            }
        }
    }

    bool SignalCwForceUnloadFlag(DWORD pid) noexcept; // после ModuleBaseInProcess
    void WakeRemoteCwProcess(DWORD pid) noexcept;     // HR8: power (не будит job-freeze)
    void PostWmNullToTrackedHwnds(DWORD pid) noexcept;

    void CloseCwForceEvents() noexcept
    {
        std::lock_guard lock(g_mx);
        for (auto& [pid, h] : g_cwForceEvents)
        {
            (void)pid;
            if (h)
                CloseHandle(h);
        }
        g_cwForceEvents.clear();
    }

    [[nodiscard]] unsigned long long MakeKey(DWORD pid, DWORD tid) noexcept
    {
        return (static_cast<unsigned long long>(pid) << 32) | tid;
    }

    [[nodiscard]] bool IsCoreWindowHostExe(const wchar_t* base) noexcept
    {
        if (!base)
            return false;
        // Start/Search: TAP на UI-потоке CoreWindow (см. UICompat-CoreWindow.md).
        // H49 был от Attach/IXDE не с UI-потока + без фильтра tid в VTC.
        return _wcsicmp(base, L"ShellExperienceHost.exe") == 0
            || _wcsicmp(base, L"ShellHost.exe") == 0
            || _wcsicmp(base, L"StartMenuExperienceHost.exe") == 0
            || _wcsicmp(base, L"SearchHost.exe") == 0
            || _wcsicmp(base, L"SearchApp.exe") == 0;
    }

    [[nodiscard]] bool IsShellExperienceHostExe(const wchar_t* base) noexcept
    {
        return base && _wcsicmp(base, L"ShellExperienceHost.exe") == 0;
    }

    [[nodiscard]] bool IsShellHostExe(const wchar_t* base) noexcept
    {
        return base && _wcsicmp(base, L"ShellHost.exe") == 0;
    }

    [[nodiscard]] bool QueryProcessExeBase(DWORD pid, wchar_t* baseOut, size_t baseCch) noexcept
    {
        if (!baseOut || baseCch == 0)
            return false;
        baseOut[0] = L'\0';
        wchar_t exe[MAX_PATH]{};
        DWORD n = MAX_PATH;
        HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!hp)
            return false;
        const BOOL ok = QueryFullProcessImageNameW(hp, 0, exe, &n);
        CloseHandle(hp);
        if (!ok)
            return false;
        const wchar_t* base = wcsrchr(exe, L'\\');
        base = base ? base + 1 : exe;
        wcsncpy_s(baseOut, baseCch, base, _TRUNCATE);
        return true;
    }

    // Packaged/AppContainer CW-hosts с PLM job freeze (Paused в Task Manager).
    [[nodiscard]] bool IsPlmFreezableCwHostExe(const wchar_t* base) noexcept
    {
        if (!base)
            return false;
        return IsShellExperienceHostExe(base)
            || _wcsicmp(base, L"StartMenuExperienceHost.exe") == 0
            || _wcsicmp(base, L"SearchHost.exe") == 0
            || _wcsicmp(base, L"SearchApp.exe") == 0;
    }

    [[nodiscard]] bool GrantFileAppPackagesRx(const wchar_t* path) noexcept
    {
        if (!path || !path[0])
            return false;

        PSECURITY_DESCRIPTOR pSd = nullptr;
        PACL pOldDacl = nullptr;
        if (GetNamedSecurityInfoW(
                path,
                SE_FILE_OBJECT,
                DACL_SECURITY_INFORMATION,
                nullptr,
                nullptr,
                &pOldDacl,
                nullptr,
                &pSd) != ERROR_SUCCESS)
        {
            return false;
        }

        PSID pSid = nullptr;
        // ALL APPLICATION PACKAGES
        if (!ConvertStringSidToSidW(L"S-1-15-2-1", &pSid) || !pSid)
        {
            if (pSd)
                LocalFree(pSd);
            return false;
        }

        EXPLICIT_ACCESSW ea{};
        ea.grfAccessPermissions = GENERIC_READ | GENERIC_EXECUTE;
        ea.grfAccessMode = GRANT_ACCESS;
        ea.grfInheritance = NO_INHERITANCE;
        ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        ea.Trustee.ptstrName = reinterpret_cast<LPWSTR>(pSid);

        PACL pNewDacl = nullptr;
        const DWORD setEa = SetEntriesInAclW(1, &ea, pOldDacl, &pNewDacl);
        LocalFree(pSid);
        if (setEa != ERROR_SUCCESS || !pNewDacl)
        {
            if (pSd)
                LocalFree(pSd);
            return false;
        }

        const DWORD setInfo = SetNamedSecurityInfoW(
            const_cast<LPWSTR>(path),
            SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION,
            nullptr,
            nullptr,
            pNewDacl,
            nullptr);

        LocalFree(pNewDacl);
        if (pSd)
            LocalFree(pSd);
        return setInfo == ERROR_SUCCESS;
    }

    [[nodiscard]] std::wstring PeerDllPath(HMODULE hSelf, const wchar_t* dllName) noexcept
    {
        wchar_t path[MAX_PATH]{};
        if (!hSelf || !dllName)
            return {};
        if (GetModuleFileNameW(hSelf, path, MAX_PATH) == 0)
            return {};
        wchar_t* slash = wcsrchr(path, L'\\');
        if (!slash)
            return {};
        slash[1] = L'\0';
        std::wstring full = path;
        full += dllName;
        return full;
    }

    [[nodiscard]] HMODULE ModuleBaseInProcess(DWORD pid, const wchar_t* moduleName) noexcept
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap == INVALID_HANDLE_VALUE)
            return nullptr;
        MODULEENTRY32W me{};
        me.dwSize = sizeof(me);
        HMODULE base = nullptr;
        if (Module32FirstW(snap, &me))
        {
            do
            {
                if (_wcsicmp(me.szModule, moduleName) == 0)
                {
                    base = me.hModule;
                    break;
                }
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
        return base;
    }

    [[nodiscard]] bool ModuleLoadedInProcess(DWORD pid, const wchar_t* moduleName) noexcept
    {
        return ModuleBaseInProcess(pid, moduleName) != nullptr;
    }

    // H54: named Wait в SEH мёртв — пишем export GlassCompat_CwForceUnloadFlag через WPM.
    bool SignalCwForceUnloadFlag(DWORD pid) noexcept
    {
        const HMODULE remoteBase = ModuleBaseInProcess(pid, L"GlassCompat.dll");
        const HMODULE localBase = GetModuleHandleW(L"GlassCompat.dll");
        if (!remoteBase || !localBase)
            return false;

        const FARPROC localFlag = GetProcAddress(localBase, "GlassCompat_CwForceUnloadFlag");
        if (!localFlag)
            return false;

        const auto offset =
            reinterpret_cast<const BYTE*>(localFlag) - reinterpret_cast<const BYTE*>(localBase);
        void* remoteFlag = reinterpret_cast<BYTE*>(remoteBase) + offset;

        HANDLE hProcess = OpenProcess(
            PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
            FALSE,
            pid);
        if (!hProcess)
            return false;

        LONG one = 1;
        SIZE_T written = 0;
        const BOOL ok = WriteProcessMemory(
            hProcess, remoteFlag, &one, sizeof(one), &written);
        const DWORD gle = ok ? 0u : GetLastError();
        LONG readBack = -1;
        SIZE_T readN = 0;
        const BOOL readOk = ok
            && ReadProcessMemory(
                hProcess, remoteFlag, &readBack, sizeof(readBack), &readN);
        CloseHandle(hProcess);

        return ok && written == sizeof(one);
    }

    // HR14b: UWP job freeze → CRT LoadLibrary висит/WerFault (лог @85242).
    // Probe: kernel32!Sleep(1) ≤150ms; если STILL_ACTIVE — процесс не планируется.
    [[nodiscard]] bool IsRemoteProcessSchedulable(DWORD pid) noexcept
    {
        const HMODULE remoteK32 = ModuleBaseInProcess(pid, L"kernel32.dll");
        const HMODULE localK32 = GetModuleHandleW(L"kernel32.dll");
        if (!remoteK32 || !localK32)
            return false;
        const FARPROC localSleep = GetProcAddress(localK32, "Sleep");
        if (!localSleep)
            return false;
        const auto offset =
            reinterpret_cast<const BYTE*>(localSleep) - reinterpret_cast<const BYTE*>(localK32);
        auto* remoteSleep = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            reinterpret_cast<BYTE*>(remoteK32) + offset);

        HANDLE hProcess = OpenProcess(
            PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION
                | PROCESS_VM_WRITE | PROCESS_VM_READ,
            FALSE,
            pid);
        if (!hProcess)
            return false;

        HANDLE hThread = CreateRemoteThread(
            hProcess,
            nullptr,
            0,
            remoteSleep,
            reinterpret_cast<LPVOID>(static_cast<ULONG_PTR>(1)),
            0,
            nullptr);
        bool ok = false;
        DWORD wait = WAIT_FAILED;
        DWORD exitCode = 0;
        if (hThread)
        {
            wait = WaitForSingleObject(hThread, 150);
            (void)GetExitCodeThread(hThread, &exitCode);
            if (wait == WAIT_TIMEOUT)
                (void)TerminateThread(hThread, 0); // не оставлять зомби в frozen
            CloseHandle(hThread);
            ok = (wait == WAIT_OBJECT_0);
        }
        CloseHandle(hProcess);

        return ok;
    }

    // HR18: «Paused» в Task Manager у SEH = PLM suspend (не NtSuspend; HR13 resumedThr:0).
    // Microsoft: IPackageDebugSettings (PLMDebug) — EnableDebugging отключает auto-suspend,
    // Resume явно будит; Suspend+DisableDebugging возвращает управление PLM.
    // Windhawk этого цикла не делает (CRT/APC + ACL AppContainer).
    [[nodiscard]] bool QueryPackageFullNameForPid(DWORD pid, std::wstring& out) noexcept
    {
        out.clear();
        HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!hProcess)
            return false;
        UINT32 len = 0;
        LONG rc = GetPackageFullName(hProcess, &len, nullptr);
        if (rc != ERROR_INSUFFICIENT_BUFFER || len == 0)
        {
            CloseHandle(hProcess);
            return false;
        }
        out.assign(static_cast<size_t>(len), L'\0');
        rc = GetPackageFullName(hProcess, &len, out.data());
        CloseHandle(hProcess);
        if (rc != ERROR_SUCCESS)
        {
            out.clear();
            return false;
        }
        while (!out.empty() && out.back() == L'\0')
            out.pop_back();
        return !out.empty();
    }

    [[nodiscard]] IPackageDebugSettings* CreatePackageDebugSettings() noexcept
    {
        IPackageDebugSettings* settings = nullptr;
        const HRESULT hr = CoCreateInstance(
            CLSID_PackageDebugSettings,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&settings));
        if (FAILED(hr))
            return nullptr;
        return settings;
    }

    // Будит PLM-suspended пакет: EnableDebugging + Resume. Возвращает true если после этого
    // remote Sleep-probe проходит (процесс снова планируется).
    [[nodiscard]] bool TrySehPlmWake(DWORD pid) noexcept
    {
        std::wstring pkg;
        const bool gotPkg = QueryPackageFullNameForPid(pid, pkg);
        if (!gotPkg)
        {
            return false;
        }

        const HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool needUninit = SUCCEEDED(hrInit);

        IPackageDebugSettings* settings = CreatePackageDebugSettings();
        HRESULT hrEn = E_FAIL;
        HRESULT hrRes = E_FAIL;
        bool alreadyEnabled = false;
        {
            std::lock_guard lock(g_mx);
            alreadyEnabled = g_sehPlmDebugPkgs.find(pkg) != g_sehPlmDebugPkgs.end();
        }
        if (settings)
        {
            // Повторный EnableDebugging увеличивает refcount PLMDebug — Disable один раз
            // не снимает режим (утечка «не suspend»).
            if (alreadyEnabled)
                hrEn = S_OK;
            else
                hrEn = settings->EnableDebugging(pkg.c_str(), nullptr, nullptr);
            hrRes = settings->Resume(pkg.c_str());
            settings->Release();
        }
        if (SUCCEEDED(hrEn))
        {
            std::lock_guard lock(g_mx);
            g_sehPlmDebugPkgs.insert(pkg);
        }

        const bool sched = IsRemoteProcessSchedulable(pid);

        if (needUninit)
            CoUninitialize();
        return sched;
    }

    // HR18b (лог @3677–3678): Suspend при stillLoaded:true заморозил FreeSelf —
    // DLL ушла только после NC thaw. Компромисс: НЕ Suspend; DisableDebugging
    // только когда модуль уже снят; иначе оставить EnableDebugging (система/PLM
    // сами поставят на паузу после Disable, когда auto-suspend снова разрешён).
    void TrySehPlmRest(DWORD pid, bool /*suspendToo*/) noexcept
    {
        std::wstring pkg;
        if (!QueryPackageFullNameForPid(pid, pkg))
            return;

        bool weEnabled = false;
        {
            std::lock_guard lock(g_mx);
            weEnabled = g_sehPlmDebugPkgs.find(pkg) != g_sehPlmDebugPkgs.end();
        }
        if (!weEnabled)
            return;

        const HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool needUninit = SUCCEEDED(hrInit);

        IPackageDebugSettings* settings = CreatePackageDebugSettings();
        HRESULT hrDis = E_FAIL;
        if (settings)
        {
            // Suspend намеренно НЕ вызываем (HR18b).
            hrDis = settings->DisableDebugging(pkg.c_str());
            settings->Release();
        }
        if (SUCCEEDED(hrDis))
        {
            std::lock_guard lock(g_mx);
            g_sehPlmDebugPkgs.erase(pkg);
        }

        if (needUninit)
            CoUninitialize();
    }

    // Remote LoadLibrary произвольного peer DLL (GlassCompat / UICompat / …).
    bool TryRemoteLoadLibrary(
        DWORD pid,
        const std::wstring& dllPath,
        const wchar_t* loadedLeafName) noexcept
    {
        if (dllPath.empty() || !loadedLeafName || !loadedLeafName[0])
            return false;
        const bool already = ModuleLoadedInProcess(pid, loadedLeafName);
        if (already)
        {
            // #region agent log
            // GLASS-ULTRACE
            GlassUlTrace::Logf(
                "H80",
                "CoreWindowHostInject.cpp:TryRemoteLoadLibrary",
                "cw_remote_load",
                "\"target_pid\":%lu,\"leaf\":\"skip_already\",\"ok\":1,\"already\":1",
                static_cast<unsigned long>(pid));
            // #endregion
            return true;
        }
        if (IsPlatformStopSignaled())
            return false;

        HANDLE hProcess = OpenProcess(
            PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION
                | PROCESS_VM_WRITE | PROCESS_VM_READ,
            FALSE,
            pid);
        if (!hProcess)
            return false;

        const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
        void* remote = VirtualAllocEx(hProcess, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!remote)
        {
            CloseHandle(hProcess);
            return false;
        }

        bool ok = false;
        if (WriteProcessMemory(hProcess, remote, dllPath.c_str(), bytes, nullptr))
        {
            HMODULE hKernel = GetModuleHandleW(L"kernel32.dll");
            auto* pLoad = hKernel
                ? reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(hKernel, "LoadLibraryW"))
                : nullptr;
            if (pLoad)
            {
                HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0, pLoad, remote, 0, nullptr);
                if (hThread)
                {
                    // ≤2s: иначе Refresh блокирует MsgWait хоста → /stop «не останавливается».
                    WaitForSingleObject(hThread, 2000);
                    CloseHandle(hThread);
                    ok = ModuleLoadedInProcess(pid, loadedLeafName);
                }
            }
        }

        VirtualFreeEx(hProcess, remote, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        if (ok && _wcsicmp(loadedLeafName, L"GlassCompat.dll") == 0)
        {
            std::lock_guard lock(g_mx);
            g_remoteLoadPids.insert(pid);
        }
        // #region agent log
        // GLASS-ULTRACE
        {
            char leaf[80]{};
            GlassUlTrace::WideToJson(leaf, sizeof(leaf), loadedLeafName);
            GlassUlTrace::Logf(
                "H80",
                "CoreWindowHostInject.cpp:TryRemoteLoadLibrary",
                "cw_remote_load",
                "\"target_pid\":%lu,\"leaf\":\"%s\",\"ok\":%d,\"already\":0,\"after\":%d",
                static_cast<unsigned long>(pid),
                leaf,
                ok ? 1 : 0,
                ModuleLoadedInProcess(pid, loadedLeafName) ? 1 : 0);
        }
        // #endregion
        return ok;
    }

    bool TryRemoteBootstrap(DWORD pid) noexcept
    {
        {
            std::lock_guard lock(g_mx);
            if (g_bootstrappedPids.contains(pid))
                return true;
        }

        const HMODULE remoteBase = ModuleBaseInProcess(pid, L"GlassCompat.dll");
        const HMODULE localBase = GetModuleHandleW(L"GlassCompat.dll");
        if (!remoteBase || !localBase)
            return false;

        const FARPROC localFn = GetProcAddress(localBase, "GlassCompat_RemoteBootstrap");
        if (!localFn)
            return false;

        const auto offset =
            reinterpret_cast<const BYTE*>(localFn) - reinterpret_cast<const BYTE*>(localBase);
        auto* remoteFn = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            reinterpret_cast<BYTE*>(remoteBase) + offset);

        HANDLE hProcess = OpenProcess(
            PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION
                | PROCESS_VM_WRITE | PROCESS_VM_READ,
            FALSE,
            pid);
        if (!hProcess)
            return false;

        DWORD exitCode = 0;
        bool ok = false;
        HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0, remoteFn, nullptr, 0, nullptr);
        if (hThread)
        {
            WaitForSingleObject(hThread, 15000);
            GetExitCodeThread(hThread, &exitCode);
            CloseHandle(hThread);
            // H54: exit = GlassGuiThread tid (или 1 на legacy).
            ok = (exitCode != 0);
        }
        const DWORD gle = GetLastError();
        CloseHandle(hProcess);

        if (ok)
        {
            {
                std::lock_guard lock(g_mx);
                g_bootstrappedPids.insert(pid);
                g_remoteLoadPids.insert(pid);
                if (exitCode > 1u)
                    g_remoteGuiTids[pid] = exitCode;
            }
            EnsureCwForceEvent(pid);
        }
        // #region agent log
        // GLASS-ULTRACE
        GlassUlTrace::Logf(
            "H80",
            "CoreWindowHostInject.cpp:TryRemoteBootstrap",
            "cw_bootstrap",
            "\"target_pid\":%lu,\"ok\":%d,\"exit\":%lu",
            static_cast<unsigned long>(pid),
            ok ? 1 : 0,
            static_cast<unsigned long>(exitCode));
        // #endregion
        return ok;
    }

    // Explorer WinEvent-inject: нет bootstrap exit-tid — читаем GlassCompat_GuiThreadId.
    // ok=true: RPM успешен (tid может быть 0 после FreeSelf). ok=false: нет модуля / нет доступа.
    [[nodiscard]] bool TryReadRemoteGuiThreadIdEx(DWORD pid, DWORD* outTid) noexcept
    {
        if (outTid)
            *outTid = 0;
        const HMODULE remoteBase = ModuleBaseInProcess(pid, L"GlassCompat.dll");
        const HMODULE localBase = GetModuleHandleW(L"GlassCompat.dll");
        if (!remoteBase || !localBase)
            return false;

        const FARPROC localTid = GetProcAddress(localBase, "GlassCompat_GuiThreadId");
        if (!localTid)
            return false;

        const auto offset =
            reinterpret_cast<const BYTE*>(localTid) - reinterpret_cast<const BYTE*>(localBase);
        void* remoteTid = reinterpret_cast<BYTE*>(remoteBase) + offset;

        HANDLE hProcess = OpenProcess(
            PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
        if (!hProcess)
            return false;

        DWORD tid = 0;
        SIZE_T readN = 0;
        const BOOL ok = ReadProcessMemory(hProcess, remoteTid, &tid, sizeof(tid), &readN);
        CloseHandle(hProcess);
        if (!ok || readN != sizeof(tid))
            return false;
        if (outTid)
            *outTid = tid;
        return true;
    }

    [[nodiscard]] DWORD TryReadRemoteGuiThreadId(DWORD pid) noexcept
    {
        DWORD tid = 0;
        if (!TryReadRemoteGuiThreadIdEx(pid, &tid) || tid <= 1u)
            return 0;
        return tid;
    }

    [[nodiscard]] bool IsThreadAlive(DWORD tid) noexcept
    {
        if (tid <= 1u)
            return false;
        HANDLE h = OpenThread(SYNCHRONIZE | THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
        if (!h)
            return false;
        const DWORD wr = WaitForSingleObject(h, 0);
        CloseHandle(h);
        // WAIT_TIMEOUT = ещё жив; WAIT_OBJECT_0 = уже завершён.
        return wr == WAIT_TIMEOUT;
    }

    /// Live GuiTid: успешный WPM=0 (FreeSelf) не откатывается на cache.
    [[nodiscard]] DWORD ResolveLiveGuiTid(DWORD pid) noexcept
    {
        DWORD wpmTid = 0;
        const bool wpmOk = TryReadRemoteGuiThreadIdEx(pid, &wpmTid);
        DWORD cached = 0;
        {
            std::lock_guard lock(g_mx);
            if (const auto it = g_remoteGuiTids.find(pid); it != g_remoteGuiTids.end())
                cached = it->second;
        }

        DWORD tid = 0;
        if (wpmOk)
        {
            // FreeSelf пишет 0 — не подставлять stale bootstrap tid.
            tid = (wpmTid > 1u && IsThreadAlive(wpmTid)) ? wpmTid : 0;
        }
        else if (cached > 1u && IsThreadAlive(cached))
        {
            tid = cached;
        }

        {
            std::lock_guard lock(g_mx);
            if (tid > 1u)
                g_remoteGuiTids[pid] = tid;
            else
                g_remoteGuiTids.erase(pid);
        }
        return tid;
    }

    void PostWmNullAllThreads(DWORD pid) noexcept
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE)
            return;
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        if (Thread32First(snap, &te))
        {
            do
            {
                if (te.th32OwnerProcessID != pid)
                    continue;
                (void)PostThreadMessageW(te.th32ThreadID, WM_NULL, 0, 0);
            } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);
    }

    // H54: PostThreadMessage(WM_QUIT) только на GlassGuiThread.
    // Сигнал unload без ожидания (HR6: параллельно всем CW PID).
    void SignalRemoteUnload(DWORD pid) noexcept
    {
        (void)SignalCwForceUnloadFlag(pid);
        SignalCwForceEvent(pid);
        WakeRemoteCwProcess(pid);

        const DWORD guiTid = ResolveLiveGuiTid(pid);
        if (guiTid > 1u)
        {
            (void)PostThreadMessageW(guiTid, WM_QUIT, 0, 0);
        }
        else
        {
            // FreeSelf уже сбросил GuiThreadId / поток мёртв — только WM_NULL.
            // WM_QUIT всем потокам explorer → чёрный рабочий стол.
            PostWmNullAllThreads(pid);
        }
    }

    // ShellHost: после HostShutdown + успешного Unhook всех WinEvent-hook,
    // завершённого path=1 (GuiTid=0) и снятия remote/self pins может остаться
    // только deferred INCONTEXT-ref Windows. Remote FreeLibrary — только после
    // wake-шторма; иначе callback в unmapped образе (dump 18.08).
    [[nodiscard]] bool TryRemoteDropShellHostResidualPin(DWORD pid) noexcept
    {
        const DWORD guiTid0 = ResolveLiveGuiTid(pid);
        const bool loaded0 = ModuleLoadedInProcess(pid, L"GlassCompat.dll");
        // #region agent log
        // GLASS-ULTRACE
        GlassUlTrace::Logf(
            "H84",
            "CoreWindowHostInject.cpp:TryRemoteDropShellHostResidualPin",
            "cw_residual_drop",
            "\"target_pid\":%lu,\"stage\":\"begin\",\"loaded\":%d,\"guiTid\":%lu",
            static_cast<unsigned long>(pid),
            loaded0 ? 1 : 0,
            static_cast<unsigned long>(guiTid0));
        // #endregion
        if (guiTid0 != 0)
        {
            DebugLog(
                DBG_IMPORTANT,
                L"[GlassCompat][CWInject] residual skip pid=%u (gui live)",
                static_cast<unsigned>(pid));
            return false;
        }

        if (!loaded0)
            return true;

        constexpr int kWakeRounds = 8;
        for (int i = 0; i < kWakeRounds; ++i)
        {
            PostWmNullToTrackedHwnds(pid);
            WakeRemoteCwProcess(pid);
            PostWmNullAllThreads(pid);
            Sleep(50);
            if (!ModuleLoadedInProcess(pid, L"GlassCompat.dll"))
                return true;
            if (ResolveLiveGuiTid(pid) != 0)
            {
                DebugLog(
                    DBG_IMPORTANT,
                    L"[GlassCompat][CWInject] residual skip pid=%u (gui revived)",
                    static_cast<unsigned>(pid));
                return false;
            }
        }

        if (!ModuleLoadedInProcess(pid, L"GlassCompat.dll"))
            return true;
        // #region agent log
        // GLASS-ULTRACE
        GlassUlTrace::Logf(
            "H84",
            "CoreWindowHostInject.cpp:TryRemoteDropShellHostResidualPin",
            "cw_residual_drop",
            "\"target_pid\":%lu,\"stage\":\"wake\",\"loaded\":1,\"guiTid\":%lu",
            static_cast<unsigned long>(pid),
            static_cast<unsigned long>(ResolveLiveGuiTid(pid)));
        // #endregion

        const HMODULE remoteCompat = ModuleBaseInProcess(pid, L"GlassCompat.dll");
        const HMODULE remoteKernel = ModuleBaseInProcess(pid, L"kernel32.dll");
        const HMODULE localKernel = GetModuleHandleW(L"kernel32.dll");
        if (!remoteCompat || !remoteKernel || !localKernel)
            return false;

        const FARPROC localFreeLibrary = GetProcAddress(localKernel, "FreeLibrary");
        if (!localFreeLibrary)
            return false;
        const auto offset =
            reinterpret_cast<const BYTE*>(localFreeLibrary)
            - reinterpret_cast<const BYTE*>(localKernel);
        auto* remoteFreeLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            reinterpret_cast<BYTE*>(remoteKernel) + offset);

        HANDLE process = OpenProcess(
            PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION
                | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
            FALSE,
            pid);
        if (!process)
            return false;

        HANDLE thread = CreateRemoteThread(
            process,
            nullptr,
            0,
            remoteFreeLibrary,
            remoteCompat,
            0,
            nullptr);
        if (thread)
        {
            (void)WaitForSingleObject(thread, 3000);
            CloseHandle(thread);
        }
        CloseHandle(process);

        for (int i = 0;
             i < 20 && ModuleLoadedInProcess(pid, L"GlassCompat.dll");
             ++i)
        {
            Sleep(25);
        }
        const bool still = ModuleLoadedInProcess(pid, L"GlassCompat.dll");
        // #region agent log
        // GLASS-ULTRACE
        GlassUlTrace::Logf(
            "H84",
            "CoreWindowHostInject.cpp:TryRemoteDropShellHostResidualPin",
            "cw_residual_drop",
            "\"target_pid\":%lu,\"stage\":\"free\",\"loaded\":%d,\"guiTid\":%lu",
            static_cast<unsigned long>(pid),
            still ? 1 : 0,
            static_cast<unsigned long>(ResolveLiveGuiTid(pid)));
        // #endregion
        return !still;
    }

    bool TryRemoteRequestShutdown(DWORD pid) noexcept
    {
        // #region agent log
        // GLASS-ULTRACE
        GlassUlTrace::Logf(
            "H88",
            "CoreWindowHostInject.cpp:TryRemoteRequestShutdown",
            "cw_remote_shutdown",
            "\"target_pid\":%lu,\"stage\":\"begin\",\"guiTid\":%lu,\"loaded\":%d",
            static_cast<unsigned long>(pid),
            static_cast<unsigned long>(ResolveLiveGuiTid(pid)),
            ModuleLoadedInProcess(pid, L"GlassCompat.dll") ? 1 : 0);
        // #endregion
        // Будим CwUnloadWatcher: WPM-флаг + named event + WM_QUIT.
        SignalRemoteUnload(pid);
        WakeRemoteCwProcess(pid);
        PostWmNullToTrackedHwnds(pid);

        // FreeSelf уже вышел (GuiThreadId==0): остался OS-pin — не жечь 40+60×50ms soft-wait.
        // Лог: free_self @4765 → wait1 только @118796 (~7s CRT/wait на residual).
        if (ResolveLiveGuiTid(pid) == 0)
        {
            for (int i = 0; i < 12 && ModuleLoadedInProcess(pid, L"GlassCompat.dll"); ++i)
            {
                PostWmNullToTrackedHwnds(pid);
                WakeRemoteCwProcess(pid);
                if ((i % 4) == 0)
                    PostWmNullAllThreads(pid);
                Sleep(50);
            }
            if (!ModuleLoadedInProcess(pid, L"GlassCompat.dll"))
                return true;

            wchar_t base[MAX_PATH]{};
            if (QueryProcessExeBase(pid, base, MAX_PATH) && IsShellHostExe(base))
                return TryRemoteDropShellHostResidualPin(pid);
            return false;
        }

        for (int i = 0; i < 40 && ModuleLoadedInProcess(pid, L"GlassCompat.dll"); ++i)
        {
            // FreeSelf сбросил GuiThreadId mid-wait — дальше только OS-pin storm.
            if (ResolveLiveGuiTid(pid) == 0)
            {
                for (int j = 0; j < 12 && ModuleLoadedInProcess(pid, L"GlassCompat.dll"); ++j)
                {
                    PostWmNullToTrackedHwnds(pid);
                    WakeRemoteCwProcess(pid);
                    if ((j % 4) == 0)
                        PostWmNullAllThreads(pid);
                    Sleep(50);
                }
                return !ModuleLoadedInProcess(pid, L"GlassCompat.dll");
            }
            if ((i % 10) == 0)
            {
                SignalRemoteUnload(pid);
                WakeRemoteCwProcess(pid);
            }
            Sleep(50);
        }
        if (!ModuleLoadedInProcess(pid, L"GlassCompat.dll"))
            return true;

        // Idle ShellHost: GUI не качает очередь → CreateRemoteThread(RequestShutdown)
        // будит watcher/флаг без открытия ControlCenterWindow.
        const HMODULE remoteBase = ModuleBaseInProcess(pid, L"GlassCompat.dll");
        const HMODULE localBase = GetModuleHandleW(L"GlassCompat.dll");
        if (!remoteBase || !localBase)
            return false;

        const FARPROC localFn = GetProcAddress(localBase, "GlassCompat_RequestShutdown");
        if (!localFn)
            return false;

        const auto offset =
            reinterpret_cast<const BYTE*>(localFn) - reinterpret_cast<const BYTE*>(localBase);
        auto* remoteFn = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            reinterpret_cast<BYTE*>(remoteBase) + offset);

        HANDLE hProcess = OpenProcess(
            PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION
                | PROCESS_VM_WRITE | PROCESS_VM_READ,
            FALSE,
            pid);
        if (!hProcess)
            return false;

        HANDLE hThread = CreateRemoteThread(
            hProcess, nullptr, 0, remoteFn, remoteBase, 0, nullptr);
        if (hThread)
        {
            WaitForSingleObject(hThread, 3000);
            CloseHandle(hThread);
        }
        CloseHandle(hProcess);

        for (int i = 0; i < 60 && ModuleLoadedInProcess(pid, L"GlassCompat.dll"); ++i)
        {
            if ((i % 10) == 0)
                WakeRemoteCwProcess(pid);
            Sleep(50);
        }

        const bool gone = !ModuleLoadedInProcess(pid, L"GlassCompat.dll");
        // #region agent log
        // GLASS-ULTRACE
        GlassUlTrace::Logf(
            "H88",
            "CoreWindowHostInject.cpp:TryRemoteRequestShutdown",
            "cw_remote_shutdown",
            "\"target_pid\":%lu,\"stage\":\"end\",\"guiTid\":%lu,\"loaded\":%d",
            static_cast<unsigned long>(pid),
            static_cast<unsigned long>(ResolveLiveGuiTid(pid)),
            gone ? 0 : 1);
        // #endregion
        return gone;
    }

    void InstallProcessHook(HMODULE hSelf, DWORD pid) noexcept
    {
        // tid=0: все потоки процесса. Per-thread часто мимо UI-потока CoreWindow.
        // H46: process-wide FOREGROUND ОТКАТАН — сразу после fgOk:true в логе WerFault +
        // хост перезапускается (краш AppContainer при Attach/IXDE на FOREGROUND).
        // H47: не держать g_mx вокруг SetWinEventHook (reentrancy / чужой callback).
        const auto key = MakeKey(pid, 0);
        {
            std::lock_guard lock(g_mx);
            if (g_hooks.contains(key))
                return;
        }

        HWINEVENTHOOK hook = SetWinEventHook(
            EVENT_OBJECT_CREATE,
            EVENT_OBJECT_SHOW,
            hSelf,
            GlassCompat_WinEventProc,
            pid,
            0,
            WINEVENT_INCONTEXT);

        if (!hook)
            return;

        // QS/NC: SHOW часто cloaked → пустое дерево; патч на UNCLOAKED.
        HWINEVENTHOOK uncloakHook = SetWinEventHook(
            EVENT_OBJECT_UNCLOAKED,
            EVENT_OBJECT_UNCLOAKED,
            hSelf,
            GlassCompat_WinEventProc,
            pid,
            0,
            WINEVENT_INCONTEXT);

        HWINEVENTHOOK locationHook = SetWinEventHook(
            EVENT_OBJECT_LOCATIONCHANGE,
            EVENT_OBJECT_LOCATIONCHANGE,
            hSelf,
            GlassCompat_WinEventProc,
            pid,
            0,
            WINEVENT_INCONTEXT);

        {
            std::lock_guard lock(g_mx);
            if (g_hooks.contains(key))
            {
                UnhookWinEvent(hook);
                if (uncloakHook)
                    UnhookWinEvent(uncloakHook);
                if (locationHook)
                    UnhookWinEvent(locationHook);
                return;
            }
            g_hooks.emplace(
                key, ThreadHook{ pid, 0, hook, uncloakHook, locationHook });
        }
        unsigned hooksForPid = 0;
        {
            std::lock_guard lock(g_mx);
            for (const auto& [k, th] : g_hooks)
            {
                (void)k;
                if (th.pid != pid)
                    continue;
                if (th.hook)
                    ++hooksForPid;
                if (th.uncloakHook)
                    ++hooksForPid;
                if (th.locationHook)
                    ++hooksForPid;
            }
        }
        // #region agent log
        // GLASS-ULTRACE
        GlassUlTrace::Logf(
            "H81",
            "CoreWindowHostInject.cpp:InstallProcessHook",
            "cw_hook_install",
            "\"target_pid\":%lu,\"create\":\"%p\",\"uncloak\":\"%p\",\"location\":\"%p\",\"hooks\":%u",
            static_cast<unsigned long>(pid),
            static_cast<void*>(hook),
            static_cast<void*>(uncloakHook),
            static_cast<void*>(locationHook),
            hooksForPid);
        // #endregion
    }

    struct VisibleCwEnumCtx
    {
        DWORD interestedPids[8]{};
        unsigned pidCount = 0;
    };

    // H40/H42: AppContainer Toolhelp с хоста часто неполный (только GlassCompat).
    // Не затираем уже известные UI/Wnd биты — OR со слотом карты.
    void HostPublishInjectMapSlot(DWORD pid, const wchar_t* exeName) noexcept
    {
        if (!pid)
            return;

        using namespace Shared::InjectProcessMap;
        std::uint32_t scanned = 0;
        if (ModuleLoadedInProcess(pid, L"GlassCompat.dll"))
            scanned |= ModuleCompat;
        if (ModuleLoadedInProcess(pid, L"WndCompat.dll"))
            scanned |= ModuleWnd;
        if (ModuleLoadedInProcess(pid, L"UICompat.dll"))
            scanned |= ModuleUICompat;
        if (scanned == 0)
            return;

        std::uint32_t prev = 0;
        (void)TryGetModuleBits(pid, prev);
        // Неполный snap: scanned ⊂ prev → оставляем prev|scanned (не сужаем).
        const std::uint32_t bits = prev | scanned;

        // Регистр имени как у Toolhelp (szExeFile) — не towlower для UI InjectMap.
        const bool ok = TryUpsert(
            pid,
            Kind::ActiveGui,
            1,
            State::Alive,
            bits,
            (exeName && exeName[0]) ? exeName : L"");

    }

    bool TryRemoteEnsureUICompat(DWORD pid) noexcept
    {
        const HMODULE remoteBase = ModuleBaseInProcess(pid, L"GlassCompat.dll");
        const HMODULE localBase = GetModuleHandleW(L"GlassCompat.dll");
        if (!remoteBase || !localBase)
            return false;

        const FARPROC localFn = GetProcAddress(localBase, "GlassCompat_RemoteEnsureUICompat");
        if (!localFn)
            return false;

        const auto offset =
            reinterpret_cast<const BYTE*>(localFn) - reinterpret_cast<const BYTE*>(localBase);
        auto* remoteFn = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            reinterpret_cast<BYTE*>(remoteBase) + offset);

        HANDLE hProcess = OpenProcess(
            PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION
                | PROCESS_VM_WRITE | PROCESS_VM_READ,
            FALSE,
            pid);
        if (!hProcess)
            return false;

        HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0, remoteFn, nullptr, 0, nullptr);
        const bool started = hThread != nullptr;
        if (hThread)
        {
            WaitForSingleObject(hThread, 3000);
            CloseHandle(hThread);
        }
        CloseHandle(hProcess);

        return started;
    }

    void TryRemoteUICompatShutdown(DWORD pid) noexcept
    {
        const HMODULE remoteUi = ModuleBaseInProcess(pid, L"UICompat.dll");
        const HMODULE localUi = GetModuleHandleW(L"UICompat.dll");
        if (!remoteUi || !localUi)
            return;

        const FARPROC localFn = GetProcAddress(localUi, "UICompat_Shutdown");
        if (!localFn)
            return;

        const auto offset =
            reinterpret_cast<const BYTE*>(localFn) - reinterpret_cast<const BYTE*>(localUi);
        auto* remoteFn = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            reinterpret_cast<BYTE*>(remoteUi) + offset);

        HANDLE hProcess = OpenProcess(
            PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION
                | PROCESS_VM_WRITE | PROCESS_VM_READ,
            FALSE,
            pid);
        if (!hProcess)
            return;

        // Idle ShellHost: без wake UI-поток не качает SendMessageTimeout marshal
        // → Restore не стартует. Wait должен покрывать UiTapSite 15s+30s.
        WakeRemoteCwProcess(pid);
        PostWmNullToTrackedHwnds(pid);

        HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0, remoteFn, nullptr, 0, nullptr);
        if (hThread)
        {
            // UiTapSite marshal: до 15s (+повтор 30s). 5s/8s обрывало Restore → кастом в QS.
            DWORD waitMs = 20000;
            wchar_t base[MAX_PATH]{};
            if (QueryProcessExeBase(pid, base, MAX_PATH) && IsShellHostExe(base))
                waitMs = 50'000; // 15s + 30s + запас
            const DWORD wr = WaitForSingleObject(hThread, waitMs);
            if (wr == WAIT_TIMEOUT)
            {
                DebugLog(
                    DBG_CRITICAL,
                    L"[GlassCompat][CWInject] UICompat_Shutdown wait TIMEOUT pid=%u waitMs=%u",
                    static_cast<unsigned>(pid),
                    static_cast<unsigned>(waitMs));
            }
            CloseHandle(hThread);
        }
        CloseHandle(hProcess);

    }

    BOOL CALLBACK EnumCoreWindowCatchUpProc(HWND hwnd, LPARAM lp) noexcept
    {
        auto* ctx = reinterpret_cast<VisibleCwEnumCtx*>(lp);
        // H41: Start CoreWindow часто cloaked/не IsWindowVisible до FOREGROUND —
        // visible-only catch-up мимо SMEH; берём любой живой CoreWindow interested PID.
        if (!ctx || !IsWindow(hwnd))
            return TRUE;

        wchar_t cls[96]{};
        if (GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls))) <= 0)
            return TRUE;
        // CoreWindow (SEH/Start/Search) или ControlCenterWindow (ShellHost QS).
        if (_wcsicmp(cls, L"Windows.UI.Core.CoreWindow") != 0
            && _wcsicmp(cls, L"ControlCenterWindow") != 0)
            return TRUE;

        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (!pid)
            return TRUE;

        bool interested = false;
        for (unsigned i = 0; i < ctx->pidCount; ++i)
        {
            if (ctx->interestedPids[i] == pid)
            {
                interested = true;
                break;
            }
        }
        if (!interested)
            return TRUE;

        GlassCompat::CoreWindowHostInject::NotifyRemoteCoreWindowShow(pid, hwnd);
        return TRUE;
    }

    void CatchUpCoreWindowsForPids(const VisibleCwEnumCtx& vis) noexcept
    {
        if (vis.pidCount == 0)
            return;
        // H46: EnumThreadWindows по всем tid ОТКАТАН (нагрузка + remote SHOW storm → WerFault).
        EnumWindows(
            EnumCoreWindowCatchUpProc,
            reinterpret_cast<LPARAM>(const_cast<VisibleCwEnumCtx*>(&vis)));
    }

    void RememberRemoteCwHwnd(DWORD pid, HWND hwnd) noexcept
    {
        if (!pid || !hwnd)
            return;
        std::lock_guard lock(g_mx);
        auto& vec = g_remoteCwHwnds[pid];
        for (const HWND known : vec)
        {
            if (known == hwnd)
                return;
        }
        if (vec.size() < 16)
            vec.push_back(hwnd);
    }

    // Fire-and-forget: нельзя Wait из WinEventProc хоста.
    // HR8 (лог 561350 @82458): CreateRemoteThread(RemoteCoreWindowShow) во время
    // DoInitialize → WerFault (initDone:false, нет attach). Только Post на GUI tid.
    bool TryRemoteCoreWindowShowAsync(DWORD pid, HWND hwnd) noexcept
    {
        if (!hwnd || !IsWindow(hwnd) || !ModuleLoadedInProcess(pid, L"GlassCompat.dll"))
            return false;

        // HR12: cloaked HWND тоже нужен для wake / early patch.
        RememberRemoteCwHwnd(pid, hwnd);

        // HR16: патчить пока DWM cloaked (системная анимация) — не ждать uncloak.
        // HR8 remote_show_skip cloaked ОТКАТАН (первый кадр дефолт; hide запрещён).
        DWORD guiTid = 0;
        {
            std::lock_guard lock(g_mx);
            if (const auto it = g_remoteGuiTids.find(pid); it != g_remoteGuiTids.end())
                guiTid = it->second;
        }

        bool posted = false;
        DWORD gle = 0;
        const char* via = "no_gui";
        if (guiTid > 1u)
        {
            posted = PostThreadMessageW(
                guiTid,
                GlassCompatMsg::WM_GLASS_COREWINDOW_REMOTE_SHOW,
                reinterpret_cast<WPARAM>(hwnd),
                0) != FALSE;
            gle = posted ? 0u : GetLastError();
            via = "post_gui";
        }

        return posted;
    }

    // HR16: после eager bootstrap — AttachExisting на GUI (cloaked CoreWindow до анимации).
    // HR17: один раз на pid (Refresh спамил post каждые ~4с).
    void PostRemoteAttachExisting(DWORD pid) noexcept
    {
        {
            std::lock_guard lock(g_mx);
            if (g_attachExistingPosted.contains(pid))
                return;
            g_attachExistingPosted.insert(pid);
        }

        DWORD guiTid = 0;
        {
            std::lock_guard lock(g_mx);
            if (const auto it = g_remoteGuiTids.find(pid); it != g_remoteGuiTids.end())
                guiTid = it->second;
        }
        bool posted = false;
        if (guiTid > 1u)
        {
            posted = PostThreadMessageW(
                        guiTid,
                        GlassCompatMsg::WM_GLASS_COREWINDOW_ATTACH_EXISTING,
                        0,
                        0)
                != FALSE;
        }
        if (!posted)
        {
            std::lock_guard lock(g_mx);
            g_attachExistingPosted.erase(pid);
        }
    }

    // HR12: PostMessage(WM_NULL) на известные CoreWindow (EnumWindows SEH = 0).
    // Само по себе не будит job freeze; полезно сразу после сигнала пока SEH thawed.
    void PostWmNullToTrackedHwnds(DWORD pid) noexcept
    {
        std::vector<HWND> hwnds;
        {
            std::lock_guard lock(g_mx);
            if (const auto it = g_remoteCwHwnds.find(pid); it != g_remoteCwHwnds.end())
                hwnds = it->second;
        }

        unsigned hwndOk = 0;
        unsigned tidOk = 0;
        unsigned alive = 0;
        for (const HWND hwnd : hwnds)
        {
            if (!hwnd || !IsWindow(hwnd))
                continue;
            ++alive;
            if (PostMessageW(hwnd, WM_NULL, 0, 0))
                ++hwndOk;
            DWORD wpid = 0;
            const DWORD tid = GetWindowThreadProcessId(hwnd, &wpid);
            if (tid && wpid == pid && PostThreadMessageW(tid, WM_NULL, 0, 0))
                ++tidOk;
        }

    }

    // HR8: ProcessPowerThrottling + WM_NULL потокам (ShellHost OK; SEH job-freeze — нет).
    // HR13 REJECTED: NtResumeProcess ntStatus:0 resumedThr:0 — не NtSuspend, а UWP job freeze.
    void WakeRemoteCwProcess(DWORD pid) noexcept
    {
        HANDLE hProcess = OpenProcess(
            PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_QUERY_INFORMATION,
            FALSE,
            pid);
        BOOL powerOk = FALSE;
        if (hProcess)
        {
#if defined(PROCESS_POWER_THROTTLING_CURRENT_VERSION)
            PROCESS_POWER_THROTTLING_STATE pts{};
            pts.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
            pts.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
            pts.StateMask = 0;
            powerOk = SetProcessInformation(
                hProcess,
                ProcessPowerThrottling,
                &pts,
                sizeof(pts));
#endif
            CloseHandle(hProcess);
        }

        unsigned posted = 0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap != INVALID_HANDLE_VALUE)
        {
            THREADENTRY32 te{};
            te.dwSize = sizeof(te);
            if (Thread32First(snap, &te))
            {
                do
                {
                    if (te.th32OwnerProcessID != pid)
                        continue;
                    if (PostThreadMessageW(te.th32ThreadID, WM_NULL, 0, 0))
                        ++posted;
                } while (Thread32Next(snap, &te));
            }
            CloseHandle(snap);
        }

    }
}

namespace GlassCompat::CoreWindowHostInject
{
    void GrantPeerDllsAppPackagesAccess() noexcept
    {
        if (g_aclDone)
            return;
        g_aclDone = true;

        HMODULE hSelf = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&GrantPeerDllsAppPackagesAccess),
            &hSelf);

        static const wchar_t* kDlls[] = {
            L"GlassCompat.dll",
            L"GlassCore.dll",
            L"WndCompat.dll",
            L"UICompat.dll",
            L"TaskbarCompat.dll",
            L"ExplorerCompat.dll",
        };

        unsigned ok = 0;
        for (const wchar_t* name : kDlls)
        {
            const std::wstring path = PeerDllPath(hSelf, name);
            if (!path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                if (GrantFileAppPackagesRx(path.c_str()))
                    ++ok;
            }
        }

    }

    void Refresh() noexcept
    {
        // Хост MsgWait ждёт Stop; длинный Refresh → /stop «зависает».
        if (IsPlatformStopSignaled())
            return;

        // HR17: CreateProcessW(SEH) без AppX-активации → FailFast 0xC0000409 (цикл).
        // SEH не поднимаем: ждём системный старт (NC) или уже живой процесс.

        GrantPeerDllsAppPackagesAccess();

        HMODULE hSelf = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&Refresh),
            &hSelf);
        if (!hSelf)
            return;

        const std::wstring compatPath = PeerDllPath(hSelf, L"GlassCompat.dll");

        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE)
            return;

        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe))
        {
            do
            {
                if (IsPlatformStopSignaled())
                    break;
                if (!IsCoreWindowHostExe(pe.szExeFile))
                    continue;

                const DWORD pid = pe.th32ProcessID;
                const bool loaded = ModuleLoadedInProcess(pid, L"GlassCompat.dll");

                if (!loaded)
                {
                    // HR14b: SEH AppContainer — INCONTEXT не грузит (лог: skip → glassLoaded:false
                    // навсегда, только OOC event:3). Нужен CRT LoadLibrary, но НЕ в job freeze
                    // (HR14 skip_seh_crt сломал NC custom; @85242 freeze→WerFault).
                    // HR18: перед skip — PLM thaw (IPackageDebugSettings EnableDebugging+Resume).
                    // Start/Search — тот же job freeze, что SEH.
                    if (IsPlmFreezableCwHostExe(pe.szExeFile)
                        && !IsRemoteProcessSchedulable(pid))
                    {
                        if (!(TrySehPlmWake(pid) && IsRemoteProcessSchedulable(pid)))
                        {
                            InstallProcessHook(hSelf, pid);
                            continue;
                        }
                    }
                    if (IsRemoteLoadCoolingDown(pid))
                        continue;
                    // H46: не erase g_bootstrappedPids на Toolhelp false-negative —
                    // иначе лишний LoadLibrary pin + повторный bootstrap.
                    const bool remOk = TryRemoteLoadLibrary(pid, compatPath, L"GlassCompat.dll");
                    if (!remOk)
                    {
                        MarkRemoteLoadFailed(pid);
                        continue;
                    }
                    ClearRemoteLoadFail(pid);
                }

                // S1 ui_remote_load UICompat ОТКАТАН → 0947: UICompat грузится
                // из DoInitialize/Ensure (log 561350: remote UI в ShellHost + SEH crash на 1-м NC).

                // LoadLibrary сам по себе не вызывает WinEventProc → нужен RemoteBootstrap
                // (иначе нет патча и нет FreeSelf после stop).
                bool bootOk = false;
                if (ModuleLoadedInProcess(pid, L"GlassCompat.dll"))
                    bootOk = TryRemoteBootstrap(pid);

                InstallProcessHook(hSelf, pid);

                // HR16: eager SEH/ShellHost inject при старте → AttachExisting на GUI
                // (пока CoreWindow ещё DWM-cloaked, до анимации NC).
                if (bootOk && IsCoreWindowHostExe(pe.szExeFile))
                    PostRemoteAttachExisting(pid);

                // HR18: EnableDebugging держим до ForceUnload — иначе PLM снова freeze
                // до обработки AttachExisting. Rest(Suspend+Disable) только на stop.

                // H47 kill-switch: HostPublishInjectMapSlot — хост AV (dump pid=rundll32)
                // сразу после process_wide_hook, до host_inject_map_upsert. Bisect: пропускаем.
                // H42 sticky map временно off; InjectMap bits остаются из in-process upsert.
                if (false && (bootOk || ModuleLoadedInProcess(pid, L"GlassCompat.dll")))
                    HostPublishInjectMapSlot(pid, pe.szExeFile);
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);

        // H47 kill-switch: CatchUp (remote SHOW) — второй кандидат на host AV / storm.
        if (false)
        {
            VisibleCwEnumCtx vis{};
            {
                std::lock_guard lock(g_mx);
                for (DWORD pid : g_remoteLoadPids)
                {
                    if (vis.pidCount >= 8u)
                        break;
                    if (ModuleLoadedInProcess(pid, L"GlassCompat.dll"))
                        vis.interestedPids[vis.pidCount++] = pid;
                }
                for (DWORD pid : g_bootstrappedPids)
                {
                    if (vis.pidCount >= 8u)
                        break;
                    bool dup = false;
                    for (unsigned i = 0; i < vis.pidCount; ++i)
                    {
                        if (vis.interestedPids[i] == pid)
                        {
                            dup = true;
                            break;
                        }
                    }
                    if (!dup && ModuleLoadedInProcess(pid, L"GlassCompat.dll"))
                        vis.interestedPids[vis.pidCount++] = pid;
                }
            }
            CatchUpCoreWindowsForPids(vis);
        }

    }

    void NotifyRemoteCoreWindowShow(DWORD pid, HWND hwnd) noexcept
    {
        if (!pid || !hwnd)
            return;

        // Дебаунс: один SHOW на hwnd раз в 750 ms (OOC сыплет CREATE/SHOW).
        static HWND s_lastHwnd = nullptr;
        static ULONGLONG s_lastMs = 0;
        const ULONGLONG now = GetTickCount64();
        if (hwnd == s_lastHwnd && (now - s_lastMs) < 750ull)
            return;
        s_lastHwnd = hwnd;
        s_lastMs = now;

        // H55: OOC SHOW до Refresh — glass только со 2-го показа.
        // HR15/HR15b ОТКАТАНЫ: SW_HIDE/DWMWA_CLOAK ломают анимацию NC (окно остаётся скрытым).
        RememberRemoteCwHwnd(pid, hwnd);
        if (!ModuleLoadedInProcess(pid, L"GlassCompat.dll"))
        {
            const HMODULE hSelf = GetModuleHandleW(L"GlassCompat.dll");
            const std::wstring compatPath = PeerDllPath(hSelf, L"GlassCompat.dll");
            if (!TryRemoteLoadLibrary(pid, compatPath, L"GlassCompat.dll"))
                return;
        }
        // S1 remote UICompat здесь ОТКАТАН (0947: только GlassCompat + bootstrap).
        if (ModuleLoadedInProcess(pid, L"GlassCompat.dll"))
        {
            (void)TryRemoteBootstrap(pid);
            if (const HMODULE hSelf = GetModuleHandleW(L"GlassCompat.dll"))
                InstallProcessHook(hSelf, pid);
        }

        (void)TryRemoteCoreWindowShowAsync(pid, hwnd);
    }

    void Shutdown(bool wakeProcesses) noexcept
    {
        const ULONGLONG t0 = GetTickCount64();
        std::vector<DWORD> wakePids;
        std::vector<HWINEVENTHOOK> hooks;
        {
            std::lock_guard lock(g_mx);
            for (auto& [key, th] : g_hooks)
            {
                (void)key;
                if (th.pid)
                    wakePids.push_back(th.pid);
                if (th.hook)
                    hooks.push_back(th.hook);
                if (th.uncloakHook)
                    hooks.push_back(th.uncloakHook);
                if (th.locationHook)
                    hooks.push_back(th.locationHook);
            }
            g_hooks.clear();
            g_bootstrappedPids.clear();
        }
        // HR19: UnhookWinEvent(INCONTEXT) → deferred FreeLibrary в remote CW host;
        // без pump на remote ShellHost держит GlassCompat до случайного WM_NULL.
        for (const HWINEVENTHOOK hook : hooks)
            UnhookWinEvent(hook);
        // #region agent log
        // GLASS-ULTRACE
        GlassUlTrace::Logf(
            "H81",
            "CoreWindowHostInject.cpp:Shutdown",
            "cw_hook_unhook",
            "\"pids\":%u,\"unhooked\":%u,\"wake\":%d,\"ms\":%llu",
            static_cast<unsigned>(wakePids.size()),
            static_cast<unsigned>(hooks.size()),
            wakeProcesses ? 1 : 0,
            GetTickCount64() - t0);
        // #endregion
        if (wakeProcesses)
        {
            for (const DWORD pid : wakePids)
            {
                WakeRemoteCwProcess(pid);
                PostWmNullToTrackedHwnds(pid);
            }
            if (!hooks.empty())
                Sleep(200);
        }
    }

    void FinishTaskStop() noexcept
    {
        // Balance only debugging enabled by this host. Do not create threads in
        // other processes, set remote unload flags, or force FreeLibrary here.
        std::vector<std::wstring> debugPackages;
        {
            std::lock_guard lock(g_mx);
            debugPackages.assign(g_sehPlmDebugPkgs.begin(), g_sehPlmDebugPkgs.end());
            g_sehPlmDebugPkgs.clear();
            g_remoteLoadPids.clear();
            g_remoteGuiTids.clear();
            g_remoteCwHwnds.clear();
            g_attachExistingPosted.clear();
        }
        if (!debugPackages.empty())
        {
            const HRESULT init=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
            if (IPackageDebugSettings* settings=CreatePackageDebugSettings())
            {
                for (const auto& package : debugPackages)
                    (void)settings->DisableDebugging(package.c_str());
                settings->Release();
            }
            if (SUCCEEDED(init))
                CoUninitialize();
        }
        CloseCwForceEvents();
    }

    // #region agent log
    // GLASS-ULTRACE (R4, H82): снимок кандидатов на добивание ДО ожиданий residual.
    // В прогоне 4 после Stop у части процессов оставалась ровно одна чужая ссылка на
    // GlassCompat (loader-ref инжектора). Наш поток к этому моменту уже вышел
    // (FreeLibraryAndExitThread), снять ссылку изнутри некому → модуль висит до конца
    // жизни процесса: msedgewebview2 13912 и DllHost 21396/27684/6852/2872.
    // Снимок берётся сразу после Stop, чтобы заведомо не зацепить процессы, которые
    // инжектятся ПОСЛЕ остановки (у свежего инжекта сначала появляется только compat).
    void CollectCompatCandidates(std::vector<DWORD>& out) noexcept
    {
        out.clear();
        const DWORD self = GetCurrentProcessId();
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE)
            return;
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe))
        {
            do
            {
                const DWORD pid = pe.th32ProcessID;
                if (pid == 0 || pid == self)
                    continue;
                if ((GlassUlTrace::RemoteModuleMask(pid) & 1u) == 0)
                    continue;
                out.push_back(pid);
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }

    // Добивание остатка снаружи — тот же механизм, что и кнопка «Force unload»
    // (CreateRemoteThread(FreeLibrary)). Условие безопасности: в процессе остался РОВНО
    // compat (core/wnd/explorer/taskbar/ui уже выгружены ⇒ наш shutdown завершён,
    // потоков внутри GlassCompat нет).
    unsigned SweepCompatLeftoversOnce(
        const std::vector<DWORD>& candidates,
        const std::unordered_set<DWORD>& alivePids,
        unsigned& outTried) noexcept
    {
        unsigned unloaded = 0;
        for (const DWORD pid : candidates)
        {
            const unsigned before = GlassUlTrace::RemoteModuleMask(pid);
            if (before != 1u)
                continue;
            // Процесс ещё живёт по карте инжекта (Alive) — значит его shutdown мог не
            // начаться; снаружи не трогаем, это работа path=1/2, а не sweep.
            if (alivePids.count(pid) != 0)
                continue;
            ++outTried;
            const auto res = Shared::ForceUnloadGlassModules::ForceUnload(pid);
            const unsigned after = GlassUlTrace::RemoteModuleMask(pid);
            if (after == 0)
                ++unloaded;
            // #region agent log
            char leaf[96]{};
            {
                HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                if (hProc)
                {
                    wchar_t path[MAX_PATH]{};
                    DWORD cch = _countof(path);
                    if (QueryFullProcessImageNameW(hProc, 0, path, &cch))
                    {
                        const wchar_t* base = wcsrchr(path, L'\\');
                        GlassUlTrace::WideToJson(leaf, sizeof(leaf), base ? base + 1 : path);
                    }
                    CloseHandle(hProc);
                }
            }
            GlassUlTrace::Logf(
                "H82",
                "CoreWindowHostInject.cpp:SweepCompatLeftovers",
                "cw_sweep_pid",
                "\"target_pid\":%lu,\"target_exe\":\"%s\",\"result\":%d,\"mask_before\":%u,\"mask_after\":%u",
                static_cast<unsigned long>(pid),
                leaf,
                static_cast<int>(res),
                before,
                after);
            // #endregion
        }
        return unloaded;
    }
    // #endregion

    void ForceUnloadResiduals() noexcept
    {
        const ULONGLONG residualT0 = GetTickCount64();
        // #region agent log
        std::vector<DWORD> sweepCandidates;
        CollectCompatCandidates(sweepCandidates);
        // #endregion
        // После HostShutdown + Unhook: сначала ждём штатный path=1 по StopEvent,
        // затем backup RequestShutdown (не FreeLibrary). Лог: SEH без loop_exit при
        // немедленном remote thread — даём Stop ~2с.
        std::vector<DWORD> pids;
        {
            std::lock_guard lock(g_mx);
            pids.assign(g_remoteLoadPids.begin(), g_remoteLoadPids.end());
            g_remoteLoadPids.clear();
            g_bootstrappedPids.clear();
            // g_remoteGuiTids / force events — до конца ForceUnload.
        }

        // H-R1 (log 561350 @71733): ForceUnload будил только ShellHost из
        // g_remoteLoadPids; живой SEH с WinEvent-inject residual не попадал в список
        // → GlassCompat оставался до ручного FreeLibrary.
        {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snap != INVALID_HANDLE_VALUE)
            {
                PROCESSENTRY32W pe{};
                pe.dwSize = sizeof(pe);
                if (Process32FirstW(snap, &pe))
                {
                    do
                    {
                        if (!IsCoreWindowHostExe(pe.szExeFile)
                            && _wcsicmp(pe.szExeFile, L"explorer.exe") != 0
                            && _wcsicmp(pe.szExeFile, L"WidgetService.exe") != 0
                            && _wcsicmp(pe.szExeFile, L"Widgets.exe") != 0)
                            continue;
                        const DWORD pid = pe.th32ProcessID;
                        if (!ModuleLoadedInProcess(pid, L"GlassCompat.dll"))
                            continue;
                        bool dup = false;
                        for (const DWORD known : pids)
                        {
                            if (known == pid)
                            {
                                dup = true;
                                break;
                            }
                        }
                        if (!dup)
                            pids.push_back(pid);
                    } while (Process32NextW(snap, &pe));
                }
                CloseHandle(snap);
            }
        }

        // #region agent log
        // GLASS-ULTRACE
        GlassUlTrace::Logf(
            "H88",
            "CoreWindowHostInject.cpp:ForceUnloadResiduals",
            "cw_residual_begin",
            "\"pid_count\":%u",
            static_cast<unsigned>(pids.size()));
        for (const DWORD pid : pids)
        {
            wchar_t exe[MAX_PATH]{};
            (void)QueryProcessExeBase(pid, exe, MAX_PATH);
            char leaf[80]{};
            GlassUlTrace::WideToJson(leaf, sizeof(leaf), exe);
            char mods[80]{};
            GlassUlTrace::FormatModuleMask(GlassUlTrace::RemoteModuleMask(pid), mods, sizeof(mods));
            GlassUlTrace::Logf(
                "H88",
                "CoreWindowHostInject.cpp:ForceUnloadResiduals",
                "cw_residual_pid",
                "\"target_pid\":%lu,\"target_exe\":\"%s\",\"modules\":\"%s\",\"compat\":%d,\"ui\":%d",
                static_cast<unsigned long>(pid),
                leaf,
                mods,
                ModuleLoadedInProcess(pid, L"GlassCompat.dll") ? 1 : 0,
                ModuleLoadedInProcess(pid, L"UICompat.dll") ? 1 : 0);
        }
        // #endregion

        // ShellHost/SEH/Start/Search: UICompat Restore. Explorer — НЕ UICompat_Shutdown:
        // CreateRemoteThread + XAML dump AV в Explorer (Windows.UI.Xaml.dll).
        // PLM-frozen: remote shutdown ждёт до 20s → thaw+probe; не schedulable — skip
        // (restore остаётся на path=1 StopEvent; ниже ForceUnload всё равно Resume).
        for (const DWORD pid : pids)
        {
            wchar_t exe[MAX_PATH]{};
            DWORD n = MAX_PATH;
            HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (hp)
            {
                (void)QueryFullProcessImageNameW(hp, 0, exe, &n);
                CloseHandle(hp);
            }
            const wchar_t* base = wcsrchr(exe, L'\\');
            base = base ? base + 1 : exe;
            if (_wcsicmp(base, L"explorer.exe") == 0)
            {
                continue;
            }
            if (!ModuleBaseInProcess(pid, L"UICompat.dll"))
            {
                continue;
            }
            if (IsPlmFreezableCwHostExe(base))
            {
                (void)TrySehPlmWake(pid);
                if (!IsRemoteProcessSchedulable(pid))
                {
                    continue;
                }
            }
            // Wake до Shutdown: иначе idle ShellHost не доставляет WM_GLASS_UI_XAML_SHUTDOWN.
            WakeRemoteCwProcess(pid);
            PostWmNullToTrackedHwnds(pid);
            TryRemoteUICompatShutdown(pid);
        }

        // HR14: сигнал; SEH в job freeze не исполнит без thaw.
        // HR18: PLM Resume перед unload.
        // HR18b: НЕ Suspend в конце — лог stillLoaded:true + suspend заморозил FreeSelf.
        for (const DWORD pid : pids)
        {
            (void)TrySehPlmWake(pid);
            SignalRemoteUnload(pid);
            WakeRemoteCwProcess(pid);
            PostWmNullToTrackedHwnds(pid);
        }

        // Сразу RequestShutdown (не ждать 4s soft-only): idle ShellHost/explorer
        // не качают очередь — soft Signal без CRT давал полный wait1 (~31s).
        for (const DWORD pid : pids)
        {
            if (!ModuleLoadedInProcess(pid, L"GlassCompat.dll"))
                continue;
            (void)TryRemoteRequestShutdown(pid);
        }

        // Добивка soft wake, пока residual.
        // Полный EnumThread каждый тик на explorer слишком дорог — редко.
        // OS-pin budget короче: Module32 обычно очищается сам через несколько секунд.
        // ShellHost-only residual: restore+FreeSelf быстрые — урезанный wait1.
        bool onlyShellHost = !pids.empty();
        for (const DWORD pid : pids)
        {
            wchar_t base[MAX_PATH]{};
            if (!QueryProcessExeBase(pid, base, MAX_PATH) || !IsShellHostExe(base))
            {
                onlyShellHost = false;
                break;
            }
        }
        const int kWait1Max = onlyShellHost ? 20 : 40;
        for (int i = 0; i < kWait1Max; ++i)
        {
            bool anyLoaded = false;
            bool anyLiveGui = false;
            for (const DWORD pid : pids)
            {
                if (!ModuleLoadedInProcess(pid, L"GlassCompat.dll"))
                    continue;
                anyLoaded = true;

                const DWORD guiTid = ResolveLiveGuiTid(pid);
                if (guiTid > 1u)
                    anyLiveGui = true;

                // Дешёвый wake всегда; полный EnumThread — раз в 10 тиков.
                PostWmNullToTrackedHwnds(pid);
                WakeRemoteCwProcess(pid);
                if (guiTid > 1u && (i % 10) == 9)
                    SignalRemoteUnload(pid);
                else if (guiTid == 0 && (i % 10) == 0)
                    PostWmNullAllThreads(pid);
            }
            if (!anyLoaded)
                break;
            // Только OS-pin (все GuiTid=0): не жечь полный budget.
            const int osPinBreak = onlyShellHost ? 9 : 19;
            if (!anyLiveGui && i >= osPinBreak)
                break;
            Sleep(50);
        }

        for (const DWORD pid : pids)
        {
            if (!ModuleLoadedInProcess(pid, L"GlassCompat.dll"))
                continue;
            (void)TrySehPlmWake(pid);
            SignalRemoteUnload(pid);
            WakeRemoteCwProcess(pid);
            PostWmNullToTrackedHwnds(pid);
        }

        const int kWait2Max = onlyShellHost ? 10 : 20;
        for (int i = 0; i < kWait2Max; ++i)
        {
            bool anyLoaded = false;
            for (const DWORD pid : pids)
            {
                if (ModuleLoadedInProcess(pid, L"GlassCompat.dll"))
                {
                    anyLoaded = true;
                    break;
                }
            }
            if (!anyLoaded)
                break;
            Sleep(50);
        }

        for (const DWORD pid : pids)
        {
            const bool still = ModuleLoadedInProcess(pid, L"GlassCompat.dll");
            // DisableDebugging только после снятия DLL. Если residual — оставить
            // EnableDebugging (не freeze с модулем внутри); PLM сам притормозит позже.
            if (!still)
                TrySehPlmRest(pid, false);
        }

        unsigned left = 0;
        // #region agent log
        // GLASS-ULTRACE (H82): какие именно pid остались с модулем (пусто = всё выгружено).
        char leftPidsList[512]{};
        bool firstLeft = true;
        // #endregion
        for (const DWORD pid : pids)
        {
            if (ModuleLoadedInProcess(pid, L"GlassCompat.dll"))
            {
                ++left;
                // #region agent log
                if (!firstLeft)
                    strncat_s(leftPidsList, _countof(leftPidsList), ",", _TRUNCATE);
                char one[32]{};
                _snprintf_s(one, _TRUNCATE, "%lu", static_cast<unsigned long>(pid));
                strncat_s(leftPidsList, _countof(leftPidsList), one, _TRUNCATE);
                firstLeft = false;
                // #endregion
            }
        }
        // #region agent log
        // GLASS-ULTRACE
        GlassUlTrace::Logf(
            "H88",
            "CoreWindowHostInject.cpp:ForceUnloadResiduals",
            "cw_residual_end",
            "\"pid_count\":%u,\"left\":%u,\"left_pids\":\"%s\",\"ms\":%llu",
            static_cast<unsigned>(pids.size()),
            left,
            leftPidsList,
            GetTickCount64() - residualT0);
        // #endregion

        // #region agent log
        // GLASS-ULTRACE (R4, H82): добиваем чужие ссылки снаружи. Две попытки: если процесс
        // в момент первой ещё дописывал shutdown, вторая застанет его уже «только compat».
        {
            const ULONGLONG sweepT0 = GetTickCount64();
            unsigned tried = 0;
            unsigned unloaded = 0;
            for (int pass = 0; pass < 2; ++pass)
            {
                if (pass > 0)
                    Sleep(400);
                // Alive-слоты карты инжекта: их не трогаем (shutdown ещё/ещё не начался).
                std::unordered_set<DWORD> alivePids;
                std::vector<Shared::InjectProcessMap::Slot> slots(
                    Shared::InjectProcessMap::kSlotCount);
                DWORD slotCount = 0;
                if (Shared::InjectProcessMap::TryReadSlots(
                        slots.data(), static_cast<DWORD>(slots.size()), &slotCount))
                {
                    for (DWORD i = 0; i < slotCount; ++i)
                    {
                        if (slots[i].state == Shared::InjectProcessMap::State::Alive)
                            alivePids.insert(slots[i].pid);
                    }
                }
                unsigned passTried = 0;
                unloaded += SweepCompatLeftoversOnce(sweepCandidates, alivePids, passTried);
                tried += passTried;
            }
            unsigned leftGlobal = 0;
            // #region agent log
            // GLASS-ULTRACE (R5): кто остался — иначе left=1 не с чем сопоставить.
            char leftPidsList[256]{};
            bool firstLeft = true;
            for (const DWORD pid : sweepCandidates)
            {
                if ((GlassUlTrace::RemoteModuleMask(pid) & 1u) == 0)
                    continue;
                ++leftGlobal;
                if (!firstLeft)
                    strncat_s(leftPidsList, _countof(leftPidsList), ",", _TRUNCATE);
                char one[16]{};
                _snprintf_s(one, _TRUNCATE, "%lu", static_cast<unsigned long>(pid));
                strncat_s(leftPidsList, _countof(leftPidsList), one, _TRUNCATE);
                firstLeft = false;
            }
            // #endregion
            GlassUlTrace::Logf(
                "H82",
                "CoreWindowHostInject.cpp:SweepCompatLeftovers",
                "cw_sweep_end",
                "\"passes\":2,\"candidates\":%u,\"tried\":%u,\"unloaded\":%u,\"left\":%u,\"left_pids\":\"%s\",\"ms\":%llu",
                static_cast<unsigned>(sweepCandidates.size()),
                tried,
                unloaded,
                leftGlobal,
                leftPidsList,
                GetTickCount64() - sweepT0);
        }
        // #endregion

        CloseCwForceEvents();
        {
            std::lock_guard lock(g_mx);
            g_remoteGuiTids.clear();
            g_remoteCwHwnds.clear();
            g_attachExistingPosted.clear();
            // Не clear g_sehPlmDebugPkgs: deferred EnableDebugging нужен для следующего Rest.
        }
    }
}
