#include <Windows.h>

#include <GlassPlatformTask.h>
#include <OpenGlassHostState.h>
#include <TaskSchedulerCom.h>
#include <TrayXamlPolicyMap.h>

#include <oleauto.h>
#include <wil/com.h>

#include <chrono>
#include <string>
#include <thread>

#pragma comment(lib, "oleaut32.lib")

namespace
{
    constexpr wchar_t kOgHostTaskNameCurrent[] = L"OpenGlass Host";
    constexpr wchar_t kOgHostTaskNameLegacy[] = L"OpenGlass Legacy Host";

    /// Как GlassPlatformTask: без CoInitializeSecurity; Uninit только при S_OK.
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

    [[nodiscard]] bool OpenOgHostTaskByExactName(
        GlassShared::TaskSchedulerCom& ts,
        const wchar_t* taskName,
        wil::com_ptr<IRegisteredTask>& out,
        std::wstring* pathOut) noexcept
    {
        if (!taskName)
            return false;
        out.reset();
        if (pathOut)
            pathOut->clear();

        if (ts.GetRegisteredTask(L"\\", taskName, out))
        {
            if (pathOut && out)
            {
                BSTR p = nullptr;
                if (SUCCEEDED(out->get_Path(&p)) && p)
                {
                    *pathOut = p;
                    ::SysFreeString(p);
                }
            }
            return true;
        }

        return ts.FindTaskByNameRecursive(taskName, out, pathOut);
    }

    [[nodiscard]] bool OpenOgHostTask(
        GlassShared::TaskSchedulerCom& ts,
        wil::com_ptr<IRegisteredTask>& out,
        std::wstring* pathOut) noexcept
    {
        if (OpenOgHostTaskByExactName(ts, kOgHostTaskNameCurrent, out, pathOut))
            return true;
        return OpenOgHostTaskByExactName(ts, kOgHostTaskNameLegacy, out, pathOut);
    }

    [[nodiscard]] bool QueryOpenGlassRunningCom() noexcept
    {
        return Shared::OpenGlassHostState::QueryOrchestrationUiState()
            == Shared::OpenGlassHostState::HostUiState::Running;
    }

    [[nodiscard]] bool WaitForOpenGlassState(bool shouldRun, DWORD timeoutMs) noexcept
    {
        constexpr DWORD step = 300;
        DWORD elapsed = 0;
        while (elapsed <= timeoutMs)
        {
            if (QueryOpenGlassRunningCom() == shouldRun)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(step));
            elapsed += step;
        }
        return false;
    }

    [[nodiscard]] bool RunSchtasksChangeEnable(const wchar_t* taskPathForTn, bool enable) noexcept
    {
        return GlassShared::RunSchtasksOnTask(
            taskPathForTn, enable ? L"/Change /ENABLE" : L"/Change /DISABLE");
    }

    [[nodiscard]] bool RunSchtasksEnableFallbackBoth(bool enable) noexcept
    {
        if (RunSchtasksChangeEnable(kOgHostTaskNameCurrent, enable))
            return true;
        return RunSchtasksChangeEnable(kOgHostTaskNameLegacy, enable);
    }

    [[nodiscard]] bool ResolveLegacyHostPath(std::wstring& pathOut) noexcept
    {
        ComInitGuard com;
        if (FAILED(com.hr))
            return false;
        GlassShared::TaskSchedulerCom ts;
        if (!ts.Connect())
            return false;
        wil::com_ptr<IRegisteredTask> t;
        return OpenOgHostTask(ts, t, &pathOut) && !pathOut.empty();
    }

    [[nodiscard]] int QueryOpenGlassHostUiStateNative() noexcept
    {
        const auto st = Shared::OpenGlassHostState::QueryOrchestrationUiState();
        return static_cast<int>(st);
    }

    [[nodiscard]] bool TryStartOpenGlassHostNoWaitInternal() noexcept
    {
        if (Shared::OpenGlassHostState::IsServiceRunning()
            || Shared::OpenGlassHostState::ServiceInstalled())
        {
            SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
            if (!scm)
                return false;
            SC_HANDLE svc = OpenServiceW(
                scm,
                Shared::OpenGlassHostState::kOpenGlassHostServiceName,
                SERVICE_START | SERVICE_QUERY_STATUS);
            CloseServiceHandle(scm);
            if (!svc)
                return false;
            const BOOL started = StartServiceW(svc, 0, nullptr);
            CloseServiceHandle(svc);
            return started || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
        }

        ComInitGuard com;
        if (FAILED(com.hr))
            return false;
        GlassShared::TaskSchedulerCom ts;
        if (!ts.Connect())
            return false;
        wil::com_ptr<IRegisteredTask> task;
        if (!OpenOgHostTask(ts, task, nullptr))
            return false;

        bool enabled = true;
        (void)GlassShared::TaskSchedulerCom::GetTaskSchedulerEnabled(task.get(), &enabled);
        if (!enabled)
        {
            task.reset();
            std::wstring path;
            if (ResolveLegacyHostPath(path))
                (void)RunSchtasksChangeEnable(path.c_str(), true);
            else
                (void)RunSchtasksEnableFallbackBoth(true);

            if (!ts.Connect())
                return false;
            if (!OpenOgHostTask(ts, task, nullptr))
                return false;
        }

        return GlassShared::TaskSchedulerCom::RunTask(task.get());
    }
}

extern "C"
{
    __declspec(dllexport) int WINAPI GlassSettings_QueryOpenGlassHostUiState() noexcept
    {
        return QueryOpenGlassHostUiStateNative();
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_LegacyHostTaskExists() noexcept
    {
        return Shared::OpenGlassHostState::OrchestrationInstalled() ? TRUE : FALSE;
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_TryStartOpenGlassHostNoWait() noexcept
    {
        return TryStartOpenGlassHostNoWaitInternal() ? TRUE : FALSE;
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_TryLaunchOpenGlassStartup() noexcept
    {
        if (!TryStartOpenGlassHostNoWaitInternal())
            return FALSE;
        return WaitForOpenGlassState(true, 7000) ? TRUE : FALSE;
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_TryLaunchOpenGlassShutdown() noexcept
    {
        if (Shared::OpenGlassHostState::ServiceInstalled())
        {
            SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
            if (scm)
            {
                SC_HANDLE svc = OpenServiceW(
                    scm,
                    Shared::OpenGlassHostState::kOpenGlassHostServiceName,
                    SERVICE_STOP | SERVICE_QUERY_STATUS);
                CloseServiceHandle(scm);
                if (svc)
                {
                    SERVICE_STATUS status{};
                    (void)ControlService(svc, SERVICE_CONTROL_STOP, &status);
                    CloseServiceHandle(svc);
                    if (WaitForOpenGlassState(false, 7000))
                        return TRUE;
                }
            }
        }

        ComInitGuard com;
        if (FAILED(com.hr))
            return FALSE;
        GlassShared::TaskSchedulerCom ts;
        if (!ts.Connect())
            return FALSE;
        wil::com_ptr<IRegisteredTask> task;
        if (!OpenOgHostTask(ts, task, nullptr))
            return FALSE;

        if (!GlassShared::TaskSchedulerCom::StopTask(task.get()))
            return FALSE;
        return WaitForOpenGlassState(false, 7000) ? TRUE : FALSE;
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_SetLegacyHostTaskSchedulerEnabled(BOOL enable) noexcept
    {
        std::wstring path;
        if (ResolveLegacyHostPath(path))
            return RunSchtasksChangeEnable(path.c_str(), enable != FALSE) ? TRUE : FALSE;
        return RunSchtasksEnableFallbackBoth(enable != FALSE) ? TRUE : FALSE;
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_RemoveLegacyHostTaskCompletely() noexcept
    {
        ComInitGuard com;
        if (FAILED(com.hr))
            return FALSE;
        GlassShared::TaskSchedulerCom ts;
        if (!ts.Connect())
            return FALSE;

        wil::com_ptr<IRegisteredTask> task;
        std::wstring path;
        if (!OpenOgHostTask(ts, task, &path))
            return FALSE;

        (void)GlassShared::TaskSchedulerCom::StopTask(task.get());
        task.reset();

        bool delOk = false;
        if (!path.empty())
            delOk = ts.DeleteTaskByFullPath(path.c_str());
        else
        {
            delOk = ts.DeleteTaskByFullPath(kOgHostTaskNameCurrent)
                || ts.DeleteTaskByFullPath(kOgHostTaskNameLegacy);
        }
        return delOk ? TRUE : FALSE;
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_TryGetOpenGlassServiceBinPath(
        wchar_t* buffer,
        DWORD cchBuffer) noexcept
    {
        if (!buffer || cchBuffer == 0)
            return FALSE;

        buffer[0] = L'\0';

        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!scm)
            return FALSE;

        SC_HANDLE svc = OpenServiceW(
            scm,
            Shared::OpenGlassHostState::kOpenGlassHostServiceName,
            SERVICE_QUERY_CONFIG);
        CloseServiceHandle(scm);
        if (!svc)
            return FALSE;

        DWORD configBytes = 0;
        (void)QueryServiceConfigW(svc, nullptr, 0, &configBytes);
        if (configBytes == 0)
        {
            CloseServiceHandle(svc);
            return FALSE;
        }

        auto* cfg = static_cast<QUERY_SERVICE_CONFIGW*>(
            HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, configBytes));
        if (!cfg)
        {
            CloseServiceHandle(svc);
            return FALSE;
        }

        BOOL ok = FALSE;
        if (QueryServiceConfigW(svc, cfg, configBytes, &configBytes) && cfg->lpBinaryPathName)
        {
            if (wcsncpy_s(buffer, cchBuffer, cfg->lpBinaryPathName, _TRUNCATE) == 0)
                ok = TRUE;
        }

        HeapFree(GetProcessHeap(), 0, cfg);
        CloseServiceHandle(svc);
        return ok;
    }

    namespace
    {
        [[nodiscard]] GlassShared::GlassPlatformTask::PlatformArch ToPlatformArch(int arch) noexcept
        {
            return arch == static_cast<int>(GlassShared::GlassPlatformTask::PlatformArch::X86)
                ? GlassShared::GlassPlatformTask::PlatformArch::X86
                : GlassShared::GlassPlatformTask::PlatformArch::X64;
        }
    }

    __declspec(dllexport) int WINAPI GlassSettings_QueryGlassAutorunUiState() noexcept
    {
        return static_cast<int>(GlassShared::GlassPlatformTask::QueryGlassAutorunUiState());
    }

    __declspec(dllexport) int WINAPI GlassSettings_QueryGlassAutorunUiStateForArch(int arch) noexcept
    {
        return static_cast<int>(GlassShared::GlassPlatformTask::QueryGlassAutorunUiState(ToPlatformArch(arch)));
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_GlassAutorunTaskExists() noexcept
    {
        return GlassShared::GlassPlatformTask::GlassAutorunTaskExists() ? TRUE : FALSE;
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_GlassAutorunTaskExistsForArch(int arch) noexcept
    {
        return GlassShared::GlassPlatformTask::GlassAutorunTaskExists(ToPlatformArch(arch)) ? TRUE : FALSE;
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_SignalPlatformStop() noexcept
    {
        return GlassShared::GlassPlatformTask::SignalPlatformStop() ? TRUE : FALSE;
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_SignalPlatformStopForArch(int arch) noexcept
    {
        return GlassShared::GlassPlatformTask::SignalPlatformStop(ToPlatformArch(arch)) ? TRUE : FALSE;
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_WaitForPlatformStopped(DWORD timeoutMs) noexcept
    {
        return GlassShared::GlassPlatformTask::WaitForPlatformStopped(timeoutMs) ? TRUE : FALSE;
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_WaitForPlatformStoppedForArch(int arch, DWORD timeoutMs) noexcept
    {
        return GlassShared::GlassPlatformTask::WaitForPlatformStopped(ToPlatformArch(arch), timeoutMs)
            ? TRUE
            : FALSE;
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_StopOrphanAutorunTaskInstanceForArch(int arch) noexcept
    {
        return GlassShared::GlassPlatformTask::StopOrphanAutorunTaskInstance(ToPlatformArch(arch))
            ? TRUE
            : FALSE;
    }

    __declspec(dllexport) void WINAPI GlassSettings_StartPlatform() noexcept
    {
        GlassShared::GlassPlatformTask::StartPlatform();
    }

    __declspec(dllexport) void WINAPI GlassSettings_StartPlatformForArch(int arch) noexcept
    {
        GlassShared::GlassPlatformTask::StartPlatform(ToPlatformArch(arch));
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_SetGlassAutorunTaskSchedulerEnabled(BOOL enable) noexcept
    {
        return GlassShared::GlassPlatformTask::SetGlassAutorunTaskSchedulerEnabled(enable != FALSE)
            ? TRUE
            : FALSE;
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_SetGlassAutorunTaskSchedulerEnabledForArch(
        int arch, BOOL enable) noexcept
    {
        return GlassShared::GlassPlatformTask::SetGlassAutorunTaskSchedulerEnabled(
                   ToPlatformArch(arch), enable != FALSE)
            ? TRUE
            : FALSE;
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_PublishTrayXamlPolicy() noexcept
    {
        return Shared::TrayXamlPolicyMap::TryRepublishFromRegistry() ? TRUE : FALSE;
    }
}
