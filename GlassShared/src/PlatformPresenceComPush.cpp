#include "SharedPch.h"
#include <PlatformPresenceMonitor.h>
#include <OpenGlassHostState.h>

#include <atomic>

namespace Shared::PlatformPresenceMonitor
{
    namespace ComPush
    {
        std::atomic<bool> g_scmRegistered{ false };
        static SC_HANDLE g_serviceHandle = nullptr;
        static SERVICE_NOTIFY g_notify{};

        void WINAPI ServiceNotifyCallback(void* pContext) noexcept
        {
            (void)pContext;
            RequestRefresh();

            if (g_serviceHandle)
            {
                (void)NotifyServiceStatusChange(
                    g_serviceHandle,
                    SERVICE_NOTIFY_STOPPED | SERVICE_NOTIFY_RUNNING | SERVICE_NOTIFY_START_PENDING
                        | SERVICE_NOTIFY_STOP_PENDING,
                    &g_notify);
            }
        }

        void TryRegisterScmPush() noexcept
        {
            if (g_scmRegistered.exchange(true))
                return;

            if (!OpenGlassHostState::ServiceInstalled())
                return;

            SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
            if (!scm)
                return;

            g_serviceHandle = OpenServiceW(
                scm,
                OpenGlassHostState::kOpenGlassHostServiceName,
                SERVICE_QUERY_STATUS);
            CloseServiceHandle(scm);
            if (!g_serviceHandle)
                return;

            g_notify = {};
            g_notify.dwVersion = SERVICE_NOTIFY_STATUS_CHANGE;
            g_notify.pfnNotifyCallback = ServiceNotifyCallback;

            if (NotifyServiceStatusChange(
                    g_serviceHandle,
                    SERVICE_NOTIFY_STOPPED | SERVICE_NOTIFY_RUNNING | SERVICE_NOTIFY_START_PENDING
                        | SERVICE_NOTIFY_STOP_PENDING,
                    &g_notify)
                != ERROR_SUCCESS)
            {
                CloseServiceHandle(g_serviceHandle);
                g_serviceHandle = nullptr;
            }
        }
    } // namespace ComPush
}
