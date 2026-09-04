#include "SharedPch.h"
#include <OpenGlassHostState.h>
#include <OpenGlassDwmPolicy.h>
#include <TaskSchedulerCom.h>
#include <GlassPlatformTask.h>

#include <TlHelp32.h>
#include <wil/com.h>

namespace Shared::OpenGlassHostState
{
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
                hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                if (hr == S_OK)
                    needUninit = true;
                else if (hr == S_FALSE || hr == RPC_E_CHANGED_MODE)
                    hr = S_OK;
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
            wil::com_ptr<IRegisteredTask>& out) noexcept
        {
            if (!taskName)
                return false;
            out.reset();
            if (ts.GetRegisteredTask(L"\\", taskName, out))
                return true;
            return ts.FindTaskByNameRecursive(taskName, out, nullptr);
        }

        [[nodiscard]] bool OpenOgHostTask(
            GlassShared::TaskSchedulerCom& ts,
            wil::com_ptr<IRegisteredTask>& out) noexcept
        {
            if (OpenOgHostTaskByExactName(ts, kOgHostTaskNameCurrent, out))
                return true;
            return OpenOgHostTaskByExactName(ts, kOgHostTaskNameLegacy, out);
        }

        [[nodiscard]] bool TaskExistsViaSchtasks(const wchar_t* taskName) noexcept
        {
            if (!taskName || !*taskName)
                return false;

            if (taskName[0] != L'\\')
            {
                const std::wstring prefixed = std::wstring(L"\\") + taskName;
                if (GlassShared::RunSchtasksOnTask(prefixed.c_str(), L"/Query"))
                    return true;
            }
            else if (GlassShared::RunSchtasksOnTask(taskName, L"/Query"))
            {
                return true;
            }

            return GlassShared::RunSchtasksOnTask(taskName, L"/Query");
        }

        [[nodiscard]] bool LegacyHostTaskExistsViaSchtasks() noexcept
        {
            if (TaskExistsViaSchtasks(kOgHostTaskNameCurrent))
                return true;
            return TaskExistsViaSchtasks(kOgHostTaskNameLegacy);
        }

        [[nodiscard]] HostUiState QueryTaskUiState() noexcept
        {
            ComInitGuard com;
            if (FAILED(com.hr))
                return HostUiState::NoTask;

            GlassShared::TaskSchedulerCom ts;
            if (!ts.Connect())
                return HostUiState::NoTask;

            wil::com_ptr<IRegisteredTask> task;
            if (!OpenOgHostTask(ts, task))
                return HostUiState::NoTask;

            bool enabled = false;
            (void)GlassShared::TaskSchedulerCom::GetTaskSchedulerEnabled(task.get(), &enabled);

            TASK_STATE st = TASK_STATE_UNKNOWN;
            (void)task->get_State(&st);

            if (!enabled)
                return HostUiState::Disabled;
            if (st == TASK_STATE_RUNNING)
                return HostUiState::Running;
            return HostUiState::Ready;
        }

        [[nodiscard]] bool QueryServiceInstalledScm() noexcept
        {
            SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
            if (!scm)
                return false;
            SC_HANDLE svc = OpenServiceW(scm, kOpenGlassHostServiceName, SERVICE_QUERY_STATUS);
            const bool exists = svc != nullptr;
            if (svc)
                CloseServiceHandle(svc);
            CloseServiceHandle(scm);
            return exists;
        }

        [[nodiscard]] HostUiState QueryServiceUiState() noexcept
        {
            SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
            if (!scm)
                return HostUiState::NoTask;

            SC_HANDLE svc = OpenServiceW(
                scm,
                kOpenGlassHostServiceName,
                SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG);
            CloseServiceHandle(scm);
            if (!svc)
                return HostUiState::NoTask;

            DWORD startType = SERVICE_DEMAND_START;
            DWORD configBytes = 0;
            (void)QueryServiceConfigW(svc, nullptr, 0, &configBytes);
            if (configBytes != 0)
            {
                auto* cfg = static_cast<QUERY_SERVICE_CONFIGW*>(
                    HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, configBytes));
                if (cfg && QueryServiceConfigW(svc, cfg, configBytes, &configBytes))
                    startType = cfg->dwStartType;
                if (cfg)
                    HeapFree(GetProcessHeap(), 0, cfg);
            }

            if (startType == SERVICE_DISABLED)
            {
                CloseServiceHandle(svc);
                return HostUiState::Disabled;
            }

            SERVICE_STATUS_PROCESS status{};
            status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
            DWORD bytesNeeded = 0;
            const BOOL ok = QueryServiceStatusEx(
                svc,
                SC_STATUS_PROCESS_INFO,
                reinterpret_cast<LPBYTE>(&status),
                sizeof(status),
                &bytesNeeded);
            CloseServiceHandle(svc);

            if (!ok)
                return HostUiState::NoTask;

            switch (status.dwCurrentState)
            {
            case SERVICE_RUNNING:
            case SERVICE_START_PENDING:
                return HostUiState::Running;
            case SERVICE_STOPPED:
            case SERVICE_STOP_PENDING:
                return HostUiState::Ready;
            default:
                return HostUiState::Ready;
            }
        }

        [[nodiscard]] bool IsProcessNamedRunning(const wchar_t* exeName) noexcept
        {
            if (!exeName || !*exeName)
                return false;

            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snap == INVALID_HANDLE_VALUE)
                return false;

            PROCESSENTRY32W pe{};
            pe.dwSize = sizeof(pe);
            bool found = false;
            if (Process32FirstW(snap, &pe))
            {
                do
                {
                    if (_wcsicmp(pe.szExeFile, exeName) == 0)
                    {
                        found = true;
                        break;
                    }
                } while (!found && Process32NextW(snap, &pe));
            }
            CloseHandle(snap);
            return found;
        }
    } // namespace

    constexpr DWORD kCacheTtlMs = 1500;

    struct OrchestrationCache
    {
        HostUiState state = HostUiState::NoTask;
        ULONGLONG timeMs = 0;
    };

    std::atomic<OrchestrationCache> g_orchestrationCache{};
    std::atomic<ULONGLONG> g_legacyTaskCacheTimeMs{ 0 };
    std::atomic<bool> g_legacyTaskCacheValue{ false };

    void InvalidateCache() noexcept
    {
        g_orchestrationCache.store({}, std::memory_order_release);
        g_legacyTaskCacheTimeMs.store(0, std::memory_order_release);
        ::GlassShared::GlassPlatformTask::InvalidateGlassAutorunTaskCache();
    }

    bool LegacyHostTaskExists() noexcept
    {
        const auto now = GetTickCount64();
        const auto cachedTime = g_legacyTaskCacheTimeMs.load(std::memory_order_acquire);
        if (now - cachedTime < kCacheTtlMs)
            return g_legacyTaskCacheValue.load(std::memory_order_acquire);

        bool result = false;
        ComInitGuard com;
        if (SUCCEEDED(com.hr))
        {
            GlassShared::TaskSchedulerCom ts;
            if (ts.Connect())
            {
                wil::com_ptr<IRegisteredTask> task;
                if (OpenOgHostTask(ts, task))
                    result = true;
            }
        }

        if (!result)
            result = LegacyHostTaskExistsViaSchtasks();

        g_legacyTaskCacheValue.store(result, std::memory_order_release);
        g_legacyTaskCacheTimeMs.store(now, std::memory_order_release);
        return result;
    }

    bool ServiceInstalled() noexcept
    {
        return QueryServiceInstalledScm();
    }

    bool OrchestrationInstalled() noexcept
    {
        return LegacyHostTaskExists() || ServiceInstalled();
    }

    bool IsServiceRunning() noexcept
    {
        return QueryServiceUiState() == HostUiState::Running;
    }

    bool IsServiceAutostartEnabled() noexcept
    {
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!scm)
            return false;

        SC_HANDLE svc = OpenServiceW(
            scm,
            kOpenGlassHostServiceName,
            SERVICE_QUERY_CONFIG);
        CloseServiceHandle(scm);
        if (!svc)
            return false;

        DWORD startType = SERVICE_DEMAND_START;
        DWORD configBytes = 0;
        (void)QueryServiceConfigW(svc, nullptr, 0, &configBytes);
        if (configBytes != 0)
        {
            auto* cfg = static_cast<QUERY_SERVICE_CONFIGW*>(
                HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, configBytes));
            if (cfg && QueryServiceConfigW(svc, cfg, configBytes, &configBytes))
                startType = cfg->dwStartType;
            if (cfg)
                HeapFree(GetProcessHeap(), 0, cfg);
        }

        CloseServiceHandle(svc);
        return startType == SERVICE_AUTO_START;
    }

    bool SetServiceAutostartEnabled(const bool enable) noexcept
    {
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!scm)
            return false;

        SC_HANDLE svc = OpenServiceW(
            scm,
            kOpenGlassHostServiceName,
            SERVICE_CHANGE_CONFIG);
        CloseServiceHandle(scm);
        if (!svc)
            return false;

        const DWORD startType = enable ? SERVICE_AUTO_START : SERVICE_DEMAND_START;
        const BOOL ok = ChangeServiceConfigW(
            svc,
            SERVICE_NO_CHANGE,
            startType,
            SERVICE_NO_CHANGE,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr);
        CloseServiceHandle(svc);
        return ok != FALSE;
    }

    bool IsHostProcessRunning() noexcept
    {
        return IsProcessNamedRunning(kOpenGlassHostExeName);
    }

    HostUiState QueryHostUiState() noexcept
    {
        return QueryOrchestrationUiState();
    }

    HostUiState QueryOrchestrationUiState() noexcept
    {
        const auto now = GetTickCount64();
        auto cached = g_orchestrationCache.load(std::memory_order_acquire);
        if (now - cached.timeMs < kCacheTtlMs)
            return cached.state;

        const HostUiState svc = QueryServiceUiState();
        const HostUiState task = QueryTaskUiState();

        HostUiState result;
        if (svc == HostUiState::NoTask && task == HostUiState::NoTask)
            result = HostUiState::NoTask;
        else if (svc == HostUiState::Running || task == HostUiState::Running)
            result = HostUiState::Running;
        else if (svc == HostUiState::Disabled || task == HostUiState::Disabled)
        {
            if (svc == HostUiState::Disabled && task == HostUiState::Disabled)
                result = HostUiState::Disabled;
            else if (svc == HostUiState::Disabled && task == HostUiState::NoTask)
                result = HostUiState::Disabled;
            else if (task == HostUiState::Disabled && svc == HostUiState::NoTask)
                result = HostUiState::Disabled;
            else
                result = svc != HostUiState::NoTask ? svc : task;
        }
        else if (svc == HostUiState::Ready || task == HostUiState::Ready)
            result = HostUiState::Ready;
        else
            result = svc != HostUiState::NoTask ? svc : task;

        g_orchestrationCache.store({ result, now }, std::memory_order_release);
        return result;
    }

    OpenGlassBackend QueryActiveBackend() noexcept
    {
        if (IsServiceRunning() || (ServiceInstalled() && QueryServiceUiState() != HostUiState::NoTask))
        {
            if (IsServiceRunning() || IsHostProcessRunning())
                return OpenGlassBackend::Service;
        }

        if (QueryTaskUiState() == HostUiState::Running)
            return OpenGlassBackend::Task;

        if (ServiceInstalled())
            return OpenGlassBackend::Service;
        if (LegacyHostTaskExists())
            return OpenGlassBackend::Task;

        return OpenGlassBackend::None;
    }

    bool IsPipelineAffectingBorders() noexcept
    {
        return OpenGlassDwmPolicy::IsOpenGlassNotificationWindowPresent();
    }
}
