#include "SharedPch.h"

#include <framework.h>
#include <GlassPlatformTask.h>
#include <InjectModuleScan.h>
#include <GlassModulePath.h>
#include <PlatformPresenceState.h>
#include <ReloadRenderFlags.h>
#include <TrayXamlPolicyMap.h>
#include <TaskSchedulerCom.h>

#include <oleauto.h>

#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "oleaut32.lib")

namespace GlassShared::GlassPlatformTask
{
    namespace
    {
        constexpr wchar_t kMutexX64[] = L"GlassPlatform.Instance.Mutex.x64";
        constexpr wchar_t kMutexX86[] = L"GlassPlatform.Instance.Mutex.x86";
        constexpr wchar_t kStopX64[] = L"GlassPlatform.StopEvent.x64";
        constexpr wchar_t kStopX86[] = L"GlassPlatform.StopEvent.x86";
        constexpr wchar_t kShutdownX64[] = L"GlassPlatform.HostShutdown.x64";
        constexpr wchar_t kShutdownX86[] = L"GlassPlatform.HostShutdown.x86";
        constexpr wchar_t kHostAliveX64[] = L"GlassPlatform.HostAlive.x64";
        constexpr wchar_t kHostAliveX86[] = L"GlassPlatform.HostAlive.x86";
        constexpr wchar_t kHostPidX64[] = L"Local\\GlassHostPid.x64";
        constexpr wchar_t kHostPidX86[] = L"Local\\GlassHostPid.x86";
        constexpr wchar_t kBusyX64[] = L"Local\\GlassPlatform.LifecycleBusy.x64";
        constexpr wchar_t kBusyX86[] = L"Local\\GlassPlatform.LifecycleBusy.x86";
        constexpr wchar_t kTaskX64[] = L"GlassPlatform.Win32 Autorun Task (x64)";
        constexpr wchar_t kTaskX86[] = L"GlassPlatform.Win32 Autorun Task (x86)";

        // Держим named busy alive: CloseHandle после SetEvent уничтожает объект → ShellExt меню не видит busy.
        HANDLE g_busyHoldX64 = nullptr;
        HANDLE g_busyHoldX86 = nullptr;

        [[nodiscard]] HANDLE& BusyHoldSlot(PlatformArch arch) noexcept
        {
            return arch == PlatformArch::X64 ? g_busyHoldX64 : g_busyHoldX86;
        }

        [[nodiscard]] const wchar_t* LifecycleBusyEventName(PlatformArch arch) noexcept
        {
            return arch == PlatformArch::X64 ? kBusyX64 : kBusyX86;
        }

        /// Hosted UI (WinUI) уже владеет STA: не трогаем CoInitializeSecurity и
        /// CoUninitialize только если именно мы первые подняли COM на потоке (S_OK).
        struct ComInitGuard
        {
            HRESULT hr = E_FAIL;
            bool needUninit = false;

            ComInitGuard() noexcept
            {
                hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                if (hr == S_OK)
                    needUninit = true;
                else if (hr == S_FALSE || hr == RPC_E_CHANGED_MODE)
                    hr = S_OK;
            }

            ~ComInitGuard() noexcept
            {
                if (needUninit)
                    ::CoUninitialize();
            }
        };

        [[nodiscard]] bool RunSchtasksCmdLine(const std::wstring& cmdLine) noexcept
        {
            std::vector<wchar_t> buf(cmdLine.begin(), cmdLine.end());
            buf.push_back(L'\0');

            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi{};
            if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                    CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
                return false;

            CloseHandle(pi.hThread);
            WaitForSingleObject(pi.hProcess, 20000);
            DWORD exitCode = 1;
            GetExitCodeProcess(pi.hProcess, &exitCode);
            CloseHandle(pi.hProcess);
            return exitCode == 0;
        }

        [[nodiscard]] bool RunSchtasksVerbOnGlassTask(PlatformArch arch, const wchar_t* verb) noexcept
        {
            wchar_t sysdir[MAX_PATH]{};
            if (GetSystemDirectoryW(sysdir, MAX_PATH) == 0)
                return false;

            std::wstring exe = std::wstring(sysdir) + L"\\schtasks.exe";
            std::wstring cmd = L"\"" + exe + L"\" " + verb + L" /TN \"" + GlassAutorunTaskName(arch) + L"\"";
            return RunSchtasksCmdLine(cmd);
        }

        struct BstrHolder
        {
            BSTR value{};

            explicit BstrHolder(const wchar_t* text) noexcept
                : value(::SysAllocString(text))
            {
            }

            ~BstrHolder() noexcept
            {
                if (value)
                    ::SysFreeString(value);
            }

            BstrHolder(const BstrHolder&) = delete;
            BstrHolder& operator=(const BstrHolder&) = delete;

            [[nodiscard]] explicit operator bool() const noexcept { return value != nullptr; }
        };

        /// Набор флагов, из-за которых Планировщик завершает хост без StopEvent.
        [[nodiscard]] bool AreTaskSettingsPowerIndependent(ITaskSettings* settings) noexcept
        {
            if (!settings)
                return false;

            VARIANT_BOOL disallowOnBatteries = VARIANT_TRUE;
            VARIANT_BOOL stopOnBatteries = VARIANT_TRUE;
            VARIANT_BOOL allowHardTerminate = VARIANT_TRUE;
            if (FAILED(settings->get_DisallowStartIfOnBatteries(&disallowOnBatteries))
                || FAILED(settings->get_StopIfGoingOnBatteries(&stopOnBatteries))
                || FAILED(settings->get_AllowHardTerminate(&allowHardTerminate)))
            {
                return false;
            }
            if (disallowOnBatteries != VARIANT_FALSE
                || stopOnBatteries != VARIANT_FALSE
                || allowHardTerminate != VARIANT_FALSE)
            {
                return false;
            }

            BSTR limit = nullptr;
            if (FAILED(settings->get_ExecutionTimeLimit(&limit)))
                return false;
            const bool noTimeLimit = limit != nullptr && _wcsicmp(limit, L"PT0S") == 0;
            if (limit)
                ::SysFreeString(limit);
            if (!noTimeLimit)
                return false;

            wil::com_ptr<IIdleSettings> idle;
            if (FAILED(settings->get_IdleSettings(idle.put())) || !idle)
                return false;

            VARIANT_BOOL stopOnIdleEnd = VARIANT_TRUE;
            if (FAILED(idle->get_StopOnIdleEnd(&stopOnIdleEnd)))
                return false;
            return stopOnIdleEnd == VARIANT_FALSE;
        }

        [[nodiscard]] std::wstring TryParseGlassCompatDllFromExecArgs(const wchar_t* args) noexcept
        {
            if (!args || !*args)
                return {};

            const wchar_t* open = wcschr(args, L'"');
            if (!open)
                return {};
            ++open;

            const wchar_t* dll = wcsstr(open, L"GlassCompat.dll");
            if (!dll)
                return {};

            const wchar_t* end = dll + wcslen(L"GlassCompat.dll");
            if (end <= open)
                return {};

            return std::wstring(open, static_cast<size_t>(end - open));
        }

        [[nodiscard]] GlassShared::GlassModulePath::ModuleArch ToModuleArch(PlatformArch arch) noexcept
        {
            return arch == PlatformArch::X64
                ? GlassShared::GlassModulePath::ModuleArch::X64
                : GlassShared::GlassModulePath::ModuleArch::X86;
        }

        [[nodiscard]] bool OpenAutorunTaskDefinition(
            PlatformArch arch,
            TaskSchedulerCom& ts,
            wil::com_ptr<ITaskDefinition>& outDef,
            wil::com_ptr<ITaskSettings>& outSettings) noexcept
        {
            if (!ts.Connect())
                return false;

            wil::com_ptr<IRegisteredTask> task;
            if (!ts.GetRegisteredTask(L"\\", GlassAutorunTaskName(arch), task) || !task)
                return false;

            wil::com_ptr<ITaskDefinition> def;
            if (FAILED(task->get_Definition(def.put())) || !def)
                return false;

            wil::com_ptr<ITaskSettings> settings;
            if (FAILED(def->get_Settings(settings.put())) || !settings)
                return false;

            outDef = std::move(def);
            outSettings = std::move(settings);
            return true;
        }
    }

    const wchar_t* InstanceMutexName(PlatformArch arch) noexcept
    {
        return arch == PlatformArch::X64 ? kMutexX64 : kMutexX86;
    }

    const wchar_t* StopEventName(PlatformArch arch) noexcept
    {
        return arch == PlatformArch::X64 ? kStopX64 : kStopX86;
    }

    const wchar_t* HostShutdownEventName(PlatformArch arch) noexcept
    {
        return arch == PlatformArch::X64 ? kShutdownX64 : kShutdownX86;
    }

    const wchar_t* HostAliveMutexName(PlatformArch arch) noexcept
    {
        return arch == PlatformArch::X64 ? kHostAliveX64 : kHostAliveX86;
    }

    const wchar_t* HostPidMappingName(PlatformArch arch) noexcept
    {
        return arch == PlatformArch::X64 ? kHostPidX64 : kHostPidX86;
    }

    const wchar_t* GlassAutorunTaskName() noexcept
    {
        return GlassAutorunTaskName(NativePlatformArch());
    }

    const wchar_t* GlassAutorunTaskName(PlatformArch arch) noexcept
    {
        return arch == PlatformArch::X64 ? kTaskX64 : kTaskX86;
    }

    bool IsGlassPlatformRunning() noexcept
    {
        return IsGlassPlatformRunning(NativePlatformArch());
    }

    bool IsGlassPlatformRunning(PlatformArch arch) noexcept
    {
        HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, InstanceMutexName(arch));
        if (h)
        {
            CloseHandle(h);
            return true;
        }
        return false;
    }

    bool GlassAutorunTaskExists() noexcept
    {
        return GlassAutorunTaskExists(NativePlatformArch());
    }

    bool GlassAutorunTaskExists(PlatformArch arch) noexcept
    {
        ComInitGuard com;
        if (FAILED(com.hr))
            return false;

        TaskSchedulerCom ts;
        if (!ts.Connect())
            return false;
        return ts.TaskExists(L"\\", GlassAutorunTaskName(arch));
    }

    AutorunUiState QueryGlassAutorunUiState() noexcept
    {
        return QueryGlassAutorunUiState(NativePlatformArch());
    }

    AutorunUiState QueryGlassAutorunUiState(PlatformArch arch) noexcept
    {
        ComInitGuard com;
        if (FAILED(com.hr))
            return AutorunUiState::NoTask;

        TaskSchedulerCom ts;
        if (!ts.Connect())
            return AutorunUiState::NoTask;

        wil::com_ptr<IRegisteredTask> task;
        if (!ts.GetRegisteredTask(L"\\", GlassAutorunTaskName(arch), task))
            return AutorunUiState::NoTask;

        VARIANT_BOOL enabled = VARIANT_FALSE;
        ITaskDefinition* defRaw = nullptr;
        if (SUCCEEDED(task->get_Definition(&defRaw)) && defRaw)
        {
            wil::com_ptr<ITaskDefinition> def;
            def.attach(defRaw);
            ITaskSettings* settingsRaw = nullptr;
            if (SUCCEEDED(def->get_Settings(&settingsRaw)) && settingsRaw)
            {
                wil::com_ptr<ITaskSettings> settings;
                settings.attach(settingsRaw);
                (void)settings->get_Enabled(&enabled);
            }
        }

        if (enabled != VARIANT_TRUE)
            return AutorunUiState::Disabled;
        // Живая платформа = mutex хоста. TASK_STATE_RUNNING без mutex —
        // зомби rundll32 (ForceUnload / MessageBox) — не Running в UI.
        if (IsGlassPlatformRunning(arch))
            return AutorunUiState::Running;
        return AutorunUiState::Ready;
    }

    void StartPlatform() noexcept
    {
        StartPlatform(NativePlatformArch());
    }

    void StartPlatform(PlatformArch arch) noexcept
    {
        ComInitGuard com;
        if (FAILED(com.hr))
        {
            (void)RunSchtasksVerbOnGlassTask(arch, L"/Run");
            return;
        }

        TaskSchedulerCom ts;
        if (!ts.Connect())
        {
            (void)RunSchtasksVerbOnGlassTask(arch, L"/Run");
            return;
        }

        wil::com_ptr<IRegisteredTask> task;
        if (!ts.GetRegisteredTask(L"\\", GlassAutorunTaskName(arch), task))
        {
            (void)RunSchtasksVerbOnGlassTask(arch, L"/Run");
            return;
        }

        bool enabled = true;
        (void)TaskSchedulerCom::GetTaskSchedulerEnabled(task.get(), &enabled);
        if (!enabled)
        {
            task.reset();
            (void)SetGlassAutorunTaskSchedulerEnabled(arch, true);
            if (!ts.Connect())
            {
                (void)RunSchtasksVerbOnGlassTask(arch, L"/Run");
                return;
            }
            if (!ts.GetRegisteredTask(L"\\", GlassAutorunTaskName(arch), task))
            {
                (void)RunSchtasksVerbOnGlassTask(arch, L"/Run");
                return;
            }
        }

        if (!TaskSchedulerCom::RunTask(task.get()))
            (void)RunSchtasksVerbOnGlassTask(arch, L"/Run");
    }

    bool SignalPlatformStop() noexcept
    {
        return SignalPlatformStop(NativePlatformArch());
    }

    bool SignalPlatformStop(PlatformArch arch) noexcept
    {
        HANDLE hEvt = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, StopEventName(arch));
        if (!hEvt)
            return false;
        const BOOL ok = ::SetEvent(hEvt);
        ::CloseHandle(hEvt);
        return ok != FALSE;
    }

    void StopPlatform() noexcept
    {
        StopPlatform(NativePlatformArch());
    }

    void StopPlatform(PlatformArch arch) noexcept
    {
        (void)SignalPlatformStop(arch);
    }

    bool StopOrphanAutorunTaskInstance(PlatformArch arch) noexcept
    {
        // Живой хост должен уйти по StopEvent; StopTask только для зомби без mutex.
        if (IsGlassPlatformRunning(arch))
            return false;
        ComInitGuard com;
        if (FAILED(com.hr))
            return RunSchtasksVerbOnGlassTask(arch, L"/End");

        TaskSchedulerCom ts;
        if (!ts.Connect())
            return RunSchtasksVerbOnGlassTask(arch, L"/End");

        wil::com_ptr<IRegisteredTask> task;
        if (!ts.GetRegisteredTask(L"\\", GlassAutorunTaskName(arch), task))
            return false;

        TASK_STATE st = TASK_STATE_UNKNOWN;
        (void)task->get_State(&st);
        if (st != TASK_STATE_RUNNING)
            return true;

        const bool ok = TaskSchedulerCom::StopTask(task.get());
        if (!ok)
            return RunSchtasksVerbOnGlassTask(arch, L"/End");
        return true;
    }

    bool WaitForPlatformStopped(DWORD timeoutMs) noexcept
    {
        return WaitForPlatformStopped(NativePlatformArch(), timeoutMs);
    }

    bool WaitForPlatformStopped(PlatformArch arch, DWORD timeoutMs) noexcept
    {
        // UI Ready = !mutex. HostShutdown Completes раньше FreeSelf — ждём mutex,
        // HostShutdown только ускоряет цикл (не считаем успехом сам по себе).
        if (!IsGlassPlatformRunning(arch))
            return true;

        HANDLE hShutdown = ::OpenEventW(SYNCHRONIZE, FALSE, HostShutdownEventName(arch));
        const DWORD start = ::GetTickCount();
        bool shutdownSeen = false;
        DWORD shutdownAtMs = 0;
        for (;;)
        {
            if (!IsGlassPlatformRunning(arch))
            {
                if (hShutdown)
                    ::CloseHandle(hShutdown);
                return true;
            }

            const DWORD elapsed = ::GetTickCount() - start;
            if (elapsed >= timeoutMs)
                break;

            const DWORD remain = timeoutMs - elapsed;
            // После HostShutdown mutex ещё держит ForceUnload — опрос чаще.
            const DWORD sliceCap = shutdownSeen ? 50u : 250u;
            const DWORD slice = remain < sliceCap ? remain : sliceCap;
            if (hShutdown && !shutdownSeen)
            {
                if (::WaitForSingleObject(hShutdown, slice) == WAIT_OBJECT_0)
                {
                    shutdownSeen = true;
                    shutdownAtMs = ::GetTickCount() - start;
                }
            }
            else
            {
                ::Sleep(slice);
            }
        }


        if (hShutdown)
            ::CloseHandle(hShutdown);
        return !IsGlassPlatformRunning(arch);
    }

    bool IsLifecycleBusy(PlatformArch arch) noexcept
    {
        HANDLE h = ::OpenEventW(SYNCHRONIZE, FALSE, LifecycleBusyEventName(arch));
        if (!h)
            return false;
        const bool busy = ::WaitForSingleObject(h, 0) == WAIT_OBJECT_0;
        ::CloseHandle(h);
        // Старт уже успел (battery queue → delayed run): снять busy только если
        // это НЕ stop. На stop mutex ещё Running десятки секунд — auto-clear
        // На stop mutex ещё Running десятки секунд — auto-clear
        // разблокировал меню/тумблеры во время ForceUnload.
        if (busy && IsGlassPlatformRunning(arch))
        {
            bool stopSignaled = false;
            if (HANDLE hStop = ::OpenEventW(SYNCHRONIZE, FALSE, StopEventName(arch)))
            {
                stopSignaled = ::WaitForSingleObject(hStop, 0) == WAIT_OBJECT_0;
                ::CloseHandle(hStop);
            }
            if (!stopSignaled)
            {
                EndLifecycleBusy(arch);
                return false;
            }
        }
        return busy;
    }

    bool TryBeginLifecycleBusy(PlatformArch arch) noexcept
    {
        HANDLE& hold = BusyHoldSlot(arch);
        HANDLE h = ::CreateEventW(nullptr, TRUE, FALSE, LifecycleBusyEventName(arch));
        if (!h)
            return false;
        // Уже signaled — другой владелец держит busy.
        if (::WaitForSingleObject(h, 0) == WAIT_OBJECT_0)
        {
            ::CloseHandle(h);
            return false;
        }
        if (::SetEvent(h) == FALSE)
        {
            ::CloseHandle(h);
            return false;
        }
        // Не закрываем — иначе named event исчезает (ShellExt menu / IsLifecycleBusy).
        if (hold && hold != h)
            ::CloseHandle(hold);
        hold = h;
        Shared::PlatformPresenceState::PulsePresenceChanged();
        return true;
    }

    void EndLifecycleBusy(PlatformArch arch) noexcept
    {
        HANDLE& hold = BusyHoldSlot(arch);
        if (hold)
        {
            (void)::ResetEvent(hold);
            ::CloseHandle(hold);
            hold = nullptr;
        }
        else
        {
            // Владелец в другом процессе (GlassSettings Guard) — только Reset.
            HANDLE h = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, LifecycleBusyEventName(arch));
            if (h)
            {
                (void)::ResetEvent(h);
                ::CloseHandle(h);
            }
        }
        Shared::PlatformPresenceState::PulsePresenceChanged();
    }

    bool RefreshInjectResidualSticky(PlatformArch arch) noexcept
    {
        const bool wantWow64 = (arch == PlatformArch::X86);
        return Shared::InjectModuleScan::RefreshInjectResidualSticky(wantWow64);
    }

    bool HasShellInjectResiduals(PlatformArch arch) noexcept
    {
        // Только post-stop: при Running Compat в explorer/SEH — норма, не блокируем меню.
        if (IsGlassPlatformRunning(arch))
            return false;
        return RefreshInjectResidualSticky(arch);
    }

    InjectLifecycleUi QueryInjectLifecycleUi(PlatformArch arch) noexcept
    {
        const bool running = IsGlassPlatformRunning(arch);
        bool stopSignaled = false;
        if (HANDLE hStop = ::OpenEventW(SYNCHRONIZE, FALSE, StopEventName(arch)))
        {
            stopSignaled = ::WaitForSingleObject(hStop, 0) == WAIT_OBJECT_0;
            ::CloseHandle(hStop);
        }
        const bool busy = IsLifecycleBusy(arch);

        InjectLifecycleUi state = InjectLifecycleUi::Idle;
        // Stop в полёте (mutex ещё жив) или busy start/stop — Stopping.
        if (busy || (running && stopSignaled))
            state = InjectLifecycleUi::Stopping;
        else if (running)
            state = InjectLifecycleUi::Running;
        else if (RefreshInjectResidualSticky(arch))
            state = InjectLifecycleUi::Residual;
        else
            state = InjectLifecycleUi::Idle;

        return state;
    }

    bool SignalReloadRender(DWORD flags) noexcept
    {
        if (!IsGlassPlatformRunning())
            return false;

        // AppContainer UICompat читает TrayXamlPolicy map раньше HKCU — обновить до сигнала.
        if ((flags & kReloadRenderFlagTaskbarXaml) != 0)
            (void)Shared::TrayXamlPolicyMap::TryRepublishFromRegistry();

        HANDLE hSem = ::OpenSemaphoreW(
            SEMAPHORE_MODIFY_STATE | SYNCHRONIZE, FALSE, kReloadRenderSemaphoreName);
        HANDLE hMap = ::OpenFileMappingW(FILE_MAP_WRITE, FALSE, kReloadGenerationMappingName);
        HANDLE hFlagsMap = ::OpenFileMappingW(FILE_MAP_WRITE, FALSE, kReloadRenderFlagsMappingName);
        if (!hSem || !hMap)
        {
            if (hFlagsMap)
                ::CloseHandle(hFlagsMap);
            if (hSem)
                ::CloseHandle(hSem);
            if (hMap)
                ::CloseHandle(hMap);
            return false;
        }

        while (::WaitForSingleObject(hSem, 0) == WAIT_OBJECT_0)
        {
        }

        if (hFlagsMap)
        {
            void* pFlagsView = ::MapViewOfFile(hFlagsMap, FILE_MAP_WRITE, 0, 0, sizeof(DWORD));
            if (pFlagsView)
            {
                *static_cast<DWORD*>(pFlagsView) = flags;
                ::UnmapViewOfFile(pFlagsView);
            }
        }

        void* pView = ::MapViewOfFile(hMap, FILE_MAP_WRITE, 0, 0, sizeof(DWORD));
        if (pView)
        {
            ::InterlockedIncrement(reinterpret_cast<LONG*>(pView));
            ::UnmapViewOfFile(pView);
        }

        constexpr LONG kGuiThreadReleaseBudget = 512;
        const BOOL released = ::ReleaseSemaphore(hSem, kGuiThreadReleaseBudget, nullptr);

        if (hFlagsMap)
            ::CloseHandle(hFlagsMap);
        ::CloseHandle(hSem);
        ::CloseHandle(hMap);
        return released != FALSE;
    }

    void WriteReloadRootLiveScope(const LPARAM liveRootScope) noexcept
    {
        HANDLE hMap = ::OpenFileMappingW(FILE_MAP_WRITE, FALSE, kReloadRootLiveScopeMappingName);
        if (!hMap)
            return;

        void* pView = ::MapViewOfFile(hMap, FILE_MAP_WRITE, 0, 0, sizeof(DWORD));
        if (pView)
        {
            *static_cast<DWORD*>(pView) = static_cast<DWORD>(liveRootScope);
            ::UnmapViewOfFile(pView);
        }
        ::CloseHandle(hMap);
    }

    LPARAM ReadReloadRootLiveScope() noexcept
    {
        HANDLE hMap = ::OpenFileMappingW(FILE_MAP_READ, FALSE, kReloadRootLiveScopeMappingName);
        if (!hMap)
            return 0;

        void* pView = ::MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, sizeof(DWORD));
        if (!pView)
        {
            ::CloseHandle(hMap);
            return 0;
        }

        const LPARAM scope = static_cast<LPARAM>(*static_cast<DWORD*>(pView));
        ::UnmapViewOfFile(pView);
        ::CloseHandle(hMap);
        return scope;
    }

    bool SetGlassAutorunTaskSchedulerEnabled(bool enable) noexcept
    {
        return SetGlassAutorunTaskSchedulerEnabled(NativePlatformArch(), enable);
    }

    bool SetGlassAutorunTaskSchedulerEnabled(PlatformArch arch, bool enable) noexcept
    {
        wchar_t sysdir[MAX_PATH]{};
        if (GetSystemDirectoryW(sysdir, MAX_PATH) == 0)
            return false;
        std::wstring exe = std::wstring(sysdir) + L"\\schtasks.exe";
        std::wstring cmd = L"\"" + exe + L"\" /Change /TN \"" + GlassAutorunTaskName(arch) + L"\" ";
        cmd += enable ? L"/ENABLE" : L"/DISABLE";
        return RunSchtasksCmdLine(cmd);
    }

    bool IsAutorunTaskPowerIndependent(PlatformArch arch) noexcept
    {
        ComInitGuard com;
        if (FAILED(com.hr))
            return false;

        TaskSchedulerCom ts;
        wil::com_ptr<ITaskDefinition> def;
        wil::com_ptr<ITaskSettings> settings;
        if (!OpenAutorunTaskDefinition(arch, ts, def, settings))
            return false;

        return AreTaskSettingsPowerIndependent(settings.get());
    }

    bool ApplyAutorunTaskPowerIndependence(PlatformArch arch) noexcept
    {
        ComInitGuard com;
        if (FAILED(com.hr))
            return false;

        TaskSchedulerCom ts;
        wil::com_ptr<ITaskDefinition> def;
        wil::com_ptr<ITaskSettings> settings;
        if (!OpenAutorunTaskDefinition(arch, ts, def, settings))
            return false;

        if (AreTaskSettingsPowerIndependent(settings.get()))
            return true;

        (void)settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
        (void)settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
        // Хост завершается сам по StopEvent; hard terminate обходит UnhookWinEvent.
        (void)settings->put_AllowHardTerminate(VARIANT_FALSE);
        (void)settings->put_RunOnlyIfIdle(VARIANT_FALSE);
        (void)settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);

        // Дефолт — 72 часа, после которых Планировщик убивает хост.
        BstrHolder noTimeLimit(L"PT0S");
        if (noTimeLimit)
            (void)settings->put_ExecutionTimeLimit(noTimeLimit.value);

        wil::com_ptr<IIdleSettings> idle;
        if (SUCCEEDED(settings->get_IdleSettings(idle.put())) && idle)
        {
            (void)idle->put_StopOnIdleEnd(VARIANT_FALSE);
            (void)idle->put_RestartOnIdle(VARIANT_FALSE);
        }

        BstrHolder rootPath(L"\\");
        BstrHolder taskName(GlassAutorunTaskName(arch));
        BstrHolder emptySddl(L"");
        if (!rootPath || !taskName || !emptySddl)
            return false;

        wil::com_ptr<ITaskFolder> root;
        if (FAILED(ts.Service()->GetFolder(rootPath.value, root.put())) || !root)
            return false;

        VARIANT vEmptyUser{};
        VARIANT vEmptyPassword{};
        VARIANT vSddl{};
        ::VariantInit(&vEmptyUser);
        ::VariantInit(&vEmptyPassword);
        ::VariantInit(&vSddl);
        vSddl.vt = VT_BSTR;
        vSddl.bstrVal = emptySddl.value;

        // TASK_LOGON_NONE — принципал берётся из самого определения задачи.
        wil::com_ptr<IRegisteredTask> updated;
        const HRESULT hr = root->RegisterTaskDefinition(
            taskName.value,
            def.get(),
            TASK_UPDATE,
            vEmptyUser,
            vEmptyPassword,
            TASK_LOGON_NONE,
            vSddl,
            updated.put());

        return SUCCEEDED(hr) && updated != nullptr;
    }

    std::wstring TryGetAutorunGlassCompatInstallRoot(PlatformArch arch) noexcept
    {
        ComInitGuard com;
        if (FAILED(com.hr))
            return {};

        TaskSchedulerCom ts;
        wil::com_ptr<IRegisteredTask> task;
        if (!ts.Connect()
            || !ts.GetRegisteredTask(L"\\", GlassAutorunTaskName(arch), task)
            || !task)
        {
            return {};
        }

        wil::com_ptr<ITaskDefinition> def;
        if (FAILED(task->get_Definition(def.put())) || !def)
            return {};

        wil::com_ptr<IActionCollection> actions;
        if (FAILED(def->get_Actions(actions.put())) || !actions)
            return {};

        LONG count = 0;
        if (FAILED(actions->get_Count(&count)) || count < 1)
            return {};

        for (LONG i = 1; i <= count; ++i)
        {
            IAction* rawAction = nullptr;
            if (FAILED(actions->get_Item(i, &rawAction)) || !rawAction)
                continue;
            wil::com_ptr<IAction> action;
            action.attach(rawAction);

            TASK_ACTION_TYPE type = TASK_ACTION_EXEC;
            if (FAILED(action->get_Type(&type)) || type != TASK_ACTION_EXEC)
                continue;

            wil::com_ptr<IExecAction> exec;
            if (FAILED(action->QueryInterface(IID_PPV_ARGS(exec.put()))) || !exec)
                continue;

            std::wstring dllPath;
            BSTR args = nullptr;
            if (SUCCEEDED(exec->get_Arguments(&args)) && args)
            {
                dllPath = TryParseGlassCompatDllFromExecArgs(args);
                ::SysFreeString(args);
            }

            if (dllPath.empty())
            {
                BSTR path = nullptr;
                if (SUCCEEDED(exec->get_Path(&path)) && path)
                {
                    if (wcsstr(path, L"GlassCompat.dll") != nullptr)
                        dllPath = path;
                    ::SysFreeString(path);
                }
            }

            if (dllPath.empty())
                continue;

            const std::wstring root =
                GlassShared::GlassModulePath::GetInstallRootFromPath(dllPath.c_str());
            if (root.empty())
                continue;

            const std::wstring resolved = GlassShared::GlassModulePath::ResolveGlassModulePath(
                root.c_str(),
                L"GlassCompat.dll",
                ToModuleArch(arch));
            if (!resolved.empty())
                return root;
        }

        return {};
    }
}
