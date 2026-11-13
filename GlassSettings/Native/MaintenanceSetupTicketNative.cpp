#include "NativeHost.h"

#include <GlassInstallerLaunch.h>

extern "C" void CALLBACK GlassSettings_IssueMaintenanceSetupTicketV1W(
    HWND, HINSTANCE, LPWSTR cmdLine, int) noexcept
{
#ifdef _M_IX86
#pragma comment(linker, "/EXPORT:GlassSettings_IssueMaintenanceSetupTicketV1W=_GlassSettings_IssueMaintenanceSetupTicketV1W@16")
#endif
    if (cmdLine && *cmdLine)
        ::ExitProcess(1);
    ::ExitProcess(Shared::GlassInstallerLaunch::IssueMaintenanceTicket() ? 0u : 1u);
}
