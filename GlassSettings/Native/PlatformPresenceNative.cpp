#include <Windows.h>

#include <OpenGlassHostState.h>
#include <PlatformPresenceMonitor.h>
#include <PlatformPresenceState.h>
#include <OpenGlassDwmPolicy.h>

#include <string>

namespace
{
    bool StopServiceAndWait(SC_HANDLE svc) noexcept
    {
        const ULONGLONG deadline = GetTickCount64() + 30000;
        for (;;)
        {
            SERVICE_STATUS_PROCESS status{};
            DWORD needed = 0;
            if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                reinterpret_cast<LPBYTE>(&status), sizeof(status), &needed)) return false;
            if (status.dwCurrentState == SERVICE_STOPPED) return true;
            if (status.dwCurrentState != SERVICE_STOP_PENDING)
            {
                SERVICE_STATUS stopped{};
                if (!ControlService(svc, SERVICE_CONTROL_STOP, &stopped)
                    && GetLastError() != ERROR_SERVICE_NOT_ACTIVE) return false;
            }
            if (GetTickCount64() >= deadline) return false;
            Sleep(100);
        }
    }
}

extern "C"
{
    __declspec(dllexport) BOOL WINAPI GlassSettings_TryReadPresenceSnapshot(
        Shared::PlatformPresenceState::Snapshot* out) noexcept
    {
        if (!out)
            return FALSE;
        return Shared::PlatformPresenceState::TryReadSnapshot(*out) ? TRUE : FALSE;
    }

    __declspec(dllexport) HANDLE WINAPI GlassSettings_OpenPresenceChangedEvent() noexcept
    {
        return Shared::PlatformPresenceState::OpenPresenceChangedEvent();
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_IsOpenGlassNotificationWindowPresent() noexcept
    {
        return Shared::OpenGlassDwmPolicy::IsOpenGlassNotificationWindowPresent() ? TRUE : FALSE;
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_IsOpenGlassBackdropPipelineActive() noexcept
    {
        return Shared::OpenGlassDwmPolicy::IsOpenGlassBackdropPipelineActive() ? TRUE : FALSE;
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_ShouldApplyDwmCompositionPolicy() noexcept
    {
        return Shared::OpenGlassDwmPolicy::ShouldApplyDwmCompositionPolicy() ? TRUE : FALSE;
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_IsOpenGlassHostProcessRunning() noexcept
    {
        return Shared::OpenGlassHostState::IsHostProcessRunning() ? TRUE : FALSE;
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_OpenGlassOrchestrationInstalled() noexcept
    {
        return Shared::OpenGlassHostState::OrchestrationInstalled() ? TRUE : FALSE;
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_IsOpenGlassServiceRunning() noexcept
    {
        return Shared::OpenGlassHostState::IsServiceRunning() ? TRUE : FALSE;
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_IsOpenGlassServiceAutostartEnabled() noexcept
    {
        return Shared::OpenGlassHostState::IsServiceAutostartEnabled() ? TRUE : FALSE;
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_SetOpenGlassServiceAutostartEnabled(BOOL enable) noexcept
    {
        return Shared::OpenGlassHostState::SetServiceAutostartEnabled(enable != FALSE) ? TRUE : FALSE;
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_InstallOpenGlassService(
        const wchar_t* exePath) noexcept
    {
        if (!exePath || !*exePath)
            return FALSE;

        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
        if (!scm)
            return FALSE;

        const std::wstring binPath = std::wstring(L"\"") + exePath + L"\"";
        SC_HANDLE existing = OpenServiceW(
            scm,
            Shared::OpenGlassHostState::kOpenGlassHostServiceName,
            SERVICE_QUERY_STATUS);
        if (existing)
        {
            CloseServiceHandle(existing);
            CloseServiceHandle(scm);
            return TRUE;
        }

        SC_HANDLE svc = CreateServiceW(
            scm,
            Shared::OpenGlassHostState::kOpenGlassHostServiceName,
            Shared::OpenGlassHostState::kOpenGlassHostServiceName,
            SERVICE_QUERY_STATUS | SERVICE_START | SERVICE_CHANGE_CONFIG | DELETE,
            SERVICE_WIN32_OWN_PROCESS,
            SERVICE_AUTO_START,
            SERVICE_ERROR_NORMAL,
            binPath.c_str(),
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr);

        const bool ok = svc != nullptr;
        if (svc)
            CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return ok ? TRUE : FALSE;
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_StartOpenGlassService() noexcept
    {
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!scm) return FALSE;
        SC_HANDLE svc = OpenServiceW(scm, Shared::OpenGlassHostState::kOpenGlassHostServiceName,
            SERVICE_START | SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG | SERVICE_CHANGE_CONFIG);
        CloseServiceHandle(scm);
        if (!svc) return FALSE;

        // Home controls execution, not the persistent startup policy. A disabled
        // service can be started manually by temporarily using demand start.
        DWORD needed = 0;
        (void)QueryServiceConfigW(svc, nullptr, 0, &needed);
        auto* cfg = needed ? static_cast<QUERY_SERVICE_CONFIGW*>(
            HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, needed)) : nullptr;
        if (!cfg || !QueryServiceConfigW(svc, cfg, needed, &needed))
        {
            if (cfg) HeapFree(GetProcessHeap(), 0, cfg);
            CloseServiceHandle(svc);
            return FALSE;
        }
        const bool disabled = cfg->dwStartType == SERVICE_DISABLED;
        HeapFree(GetProcessHeap(), 0, cfg);
        if (disabled && !ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_DEMAND_START,
            SERVICE_NO_CHANGE, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr))
        {
            CloseServiceHandle(svc);
            return FALSE;
        }
        const BOOL started = StartServiceW(svc, 0, nullptr);
        const DWORD error = started ? ERROR_SUCCESS : GetLastError();
        // Restore even when StartService fails. Capture its error before any
        // cleanup calls (CloseServiceHandle need not preserve GetLastError).
        const bool restored = !disabled || ChangeServiceConfigW(svc, SERVICE_NO_CHANGE,
            SERVICE_DISABLED, SERVICE_NO_CHANGE, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        CloseServiceHandle(svc);
        Shared::OpenGlassHostState::InvalidateCache();
        return restored && (started || error == ERROR_SERVICE_ALREADY_RUNNING);
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_StopOpenGlassService() noexcept
    {
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!scm)
            return FALSE;
        SC_HANDLE svc = OpenServiceW(
            scm,
            Shared::OpenGlassHostState::kOpenGlassHostServiceName,
            SERVICE_STOP | SERVICE_QUERY_STATUS);
        CloseServiceHandle(scm);
        if (!svc)
            return FALSE;

        const bool ok = StopServiceAndWait(svc);
        CloseServiceHandle(svc);
        Shared::OpenGlassHostState::InvalidateCache();
        return ok ? TRUE : FALSE;
    }

    __declspec(dllexport) void WINAPI GlassSettings_RequestPlatformPresenceRefresh() noexcept
    {
        Shared::PlatformPresenceMonitor::RequestRefresh();
    }

    __declspec(dllexport) void WINAPI GlassSettings_NotifyRootNoBackdropChanged(DWORD noBackdrop) noexcept
    {
        DWORD mode = 0u;
        if (noBackdrop == 0u)
            mode = Shared::OpenGlassDwmPolicy::IsOpenGlassBackdropPipelineActive() ? 1u : 2u;

        Shared::PlatformPresenceMonitor::PublishRootBackdropMode(mode);
    }

    __declspec(dllexport) BOOL WINAPI GlassSettings_RemoveOpenGlassService() noexcept
    {
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!scm) return FALSE;
        SC_HANDLE svc = OpenServiceW(scm, Shared::OpenGlassHostState::kOpenGlassHostServiceName,
            DELETE | SERVICE_STOP | SERVICE_QUERY_STATUS);
        const DWORD openError = svc ? ERROR_SUCCESS : GetLastError();
        if (!svc && openError != ERROR_SERVICE_DOES_NOT_EXIST
            && openError != ERROR_SERVICE_MARKED_FOR_DELETE)
        {
            CloseServiceHandle(scm);
            return FALSE;
        }
        bool ok = true;
        if (svc)
        {
            ok = StopServiceAndWait(svc);
            if (ok) ok = DeleteService(svc) || GetLastError() == ERROR_SERVICE_MARKED_FOR_DELETE;
            // Deletion cannot finish while our own handle is open.
            CloseServiceHandle(svc);
        }
        if (ok)
        {
            const ULONGLONG deadline = GetTickCount64() + 30000;
            for (;;)
            {
                SC_HANDLE probe = OpenServiceW(scm,
                    Shared::OpenGlassHostState::kOpenGlassHostServiceName, SERVICE_QUERY_STATUS);
                const DWORD error = probe ? ERROR_SUCCESS : GetLastError();
                if (probe) CloseServiceHandle(probe);
                if (!probe && error == ERROR_SERVICE_DOES_NOT_EXIST) break;
                if ((!probe && error != ERROR_SERVICE_MARKED_FOR_DELETE)
                    || GetTickCount64() >= deadline) { ok = false; break; }
                Sleep(100);
            }
        }
        CloseServiceHandle(scm);
        Shared::OpenGlassHostState::InvalidateCache();
        return ok ? TRUE : FALSE;
    }
}
