#include "SharedPch.h"
#include <OpenGlassHostLaunch.h>
#include <OpenGlassHostState.h>
#include <TaskSchedulerCom.h>

#include <oleauto.h>
#include <wil/com.h>

#include <chrono>
#include <string>
#include <thread>

#pragma comment(lib, "oleaut32.lib")

namespace Shared::OpenGlassHostLaunch
{
    namespace
    {
        constexpr wchar_t kOgHostTaskNameCurrent[] = L"OpenGlass Host";
        constexpr wchar_t kOgHostTaskNameLegacy[] = L"OpenGlass Legacy Host";

        struct ComInitGuard
        {
            HRESULT hr = E_FAIL;
            bool needUninit = false;

            ComInitGuard() noexcept
            {
                hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                if (SUCCEEDED(hr))
                    needUninit = true;
                if (hr == RPC_E_CHANGED_MODE)
                    hr = S_OK;

                (void)::CoInitializeSecurity(
                    nullptr,
                    -1,
                    nullptr,
                    nullptr,
                    RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
                    RPC_C_IMP_LEVEL_IMPERSONATE,
                    nullptr,
                    0,
                    nullptr);
            }

            ~ComInitGuard() noexcept
            {
                if (needUninit)
                    CoUninitialize();
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
                    BSTR path = nullptr;
                    if (SUCCEEDED(out->get_Path(&path)) && path)
                    {
                        *pathOut = path;
                        SysFreeString(path);
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

        [[nodiscard]] bool IsOrchestrationRunning() noexcept
        {
            return OpenGlassHostState::QueryOrchestrationUiState()
                == OpenGlassHostState::HostUiState::Running;
        }

        [[nodiscard]] bool WaitForOrchestrationState(bool shouldRun, DWORD timeoutMs) noexcept
        {
            constexpr DWORD step = 300;
            DWORD elapsed = 0;
            while (elapsed <= timeoutMs)
            {
                if (IsOrchestrationRunning() == shouldRun)
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
            wil::com_ptr<IRegisteredTask> task;
            return OpenOgHostTask(ts, task, &pathOut) && !pathOut.empty();
        }
    } // namespace

    bool TryStartup() noexcept
    {
        if (IsOrchestrationRunning())
            return true;

        if (OpenGlassHostState::IsServiceRunning()
            || OpenGlassHostState::ServiceInstalled())
        {
            SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
            if (!scm)
                return false;
            SC_HANDLE svc = OpenServiceW(
                scm,
                OpenGlassHostState::kOpenGlassHostServiceName,
                SERVICE_START | SERVICE_QUERY_STATUS);
            CloseServiceHandle(scm);
            if (!svc)
                return false;
            const BOOL started = StartServiceW(svc, 0, nullptr);
            CloseServiceHandle(svc);
            if (started || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING)
                return WaitForOrchestrationState(true, 7000);
            return false;
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

        if (!GlassShared::TaskSchedulerCom::RunTask(task.get()))
            return false;
        return WaitForOrchestrationState(true, 7000);
    }

    bool TryShutdown() noexcept
    {
        if (!IsOrchestrationRunning())
            return true;

        if (OpenGlassHostState::ServiceInstalled())
        {
            SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
            if (scm)
            {
                SC_HANDLE svc = OpenServiceW(
                    scm,
                    OpenGlassHostState::kOpenGlassHostServiceName,
                    SERVICE_STOP | SERVICE_QUERY_STATUS);
                CloseServiceHandle(scm);
                if (svc)
                {
                    SERVICE_STATUS status{};
                    (void)ControlService(svc, SERVICE_CONTROL_STOP, &status);
                    CloseServiceHandle(svc);
                    if (WaitForOrchestrationState(false, 7000))
                        return true;
                }
            }
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

        if (!GlassShared::TaskSchedulerCom::StopTask(task.get()))
            return false;
        return WaitForOrchestrationState(false, 7000);
    }
}
