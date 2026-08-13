#include <Windows.h>

#include <OpenGlassHostState.h>
#include <PlatformPresenceMonitor.h>
#include <PlatformPresenceState.h>
#include <OpenGlassDwmPolicy.h>

#include <string>

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
        if (!scm)
            return FALSE;
        SC_HANDLE svc = OpenServiceW(
            scm,
            Shared::OpenGlassHostState::kOpenGlassHostServiceName,
            SERVICE_START | SERVICE_QUERY_STATUS);
        CloseServiceHandle(scm);
        if (!svc)
            return FALSE;
        const BOOL started = StartServiceW(svc, 0, nullptr);
        CloseServiceHandle(svc);
        return started ? TRUE : (GetLastError() == ERROR_SERVICE_ALREADY_RUNNING);
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

        SERVICE_STATUS status{};
        const BOOL ok = ControlService(svc, SERVICE_CONTROL_STOP, &status);
        CloseServiceHandle(svc);
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
        if (!scm)
            return FALSE;
        SC_HANDLE svc = OpenServiceW(
            scm,
            Shared::OpenGlassHostState::kOpenGlassHostServiceName,
            DELETE | SERVICE_STOP | SERVICE_QUERY_STATUS);
        CloseServiceHandle(scm);
        if (!svc)
            return FALSE;

        SERVICE_STATUS status{};
        (void)ControlService(svc, SERVICE_CONTROL_STOP, &status);
        const BOOL deleted = DeleteService(svc);
        CloseServiceHandle(svc);
        return deleted ? TRUE : FALSE;
    }
}
