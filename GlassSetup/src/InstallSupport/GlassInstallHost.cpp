#include "GlassInstallHost.h"
#include "GlassInstallLog.h"
#include "GlassInstallUtil.h"
#include <winsvc.h>

namespace {

namespace GP = GlassShared::GlassPlatformTask;

bool StopGlassPlatformImpl(GP::PlatformArch arch) noexcept {
    if (GP::IsGlassPlatformRunning(arch)) {
        const bool signaled = GP::SignalPlatformStop(arch);
        GlassInstallLog::LineFormat(L"HostStop: SignalPlatformStop %s",
            signaled ? L"ok" : L"failed");
    }
    const DWORD deadline = GetTickCount() + 45000;
    int emptyResidualStreak = 0;
    while (GetTickCount() < deadline) {
        if (!GP::IsGlassPlatformRunning(arch)) {
            (void)GP::StopOrphanAutorunTaskInstance(arch);
            if (!GP::RefreshInjectResidualSticky(arch)) {
                if (++emptyResidualStreak >= 6)
                    return true;
            } else {
                emptyResidualStreak = 0;
            }
        }
        Sleep(250);
    }
    return !GP::IsGlassPlatformRunning(arch);
}

} // namespace

namespace GlassInstallHost {

bool StopGlassPlatform(GP::PlatformArch arch) noexcept {
    return StopGlassPlatformImpl(arch);
}

bool StopOpenGlassHost() noexcept {
    GlassInstallUtil::RunHidden(L"schtasks.exe", L"/End /TN \"OpenGlass Host\"");
    GlassInstallUtil::RunHidden(L"schtasks.exe", L"/End /TN \"OpenGlass Legacy Host\"");
    const SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scm) {
        const SC_HANDLE svc = OpenServiceW(scm, L"OpenGlassHost", SERVICE_STOP | SERVICE_QUERY_STATUS);
        CloseServiceHandle(scm);
        if (svc) {
            SERVICE_STATUS status{};
            ControlService(svc, SERVICE_CONTROL_STOP, &status);
            CloseServiceHandle(svc);
        }
    }
    const DWORD start = GetTickCount();
    while (GlassInstallUtil::ImageRunning(L"OpenGlassHost.exe") && GetTickCount() - start < 20000)
        Sleep(300);
    return !GlassInstallUtil::ImageRunning(L"OpenGlassHost.exe");
}

bool StopAllForUninstall(
    const std::wstring& x64InstallDir,
    const std::wstring& x86InstallDir,
    bool stopX64,
    bool stopX86,
    bool stopOpenGlass) noexcept {
    (void)x64InstallDir;
    (void)x86InstallDir;
    GlassInstallUtil::CloseGlassSettingsProcesses();
    bool ok = true;
    if (stopX64 && !StopGlassPlatformImpl(GP::PlatformArch::X64)) ok = false;
    if (stopX86 && !StopGlassPlatformImpl(GP::PlatformArch::X86)) ok = false;
    if (stopOpenGlass && !StopOpenGlassHost()) ok = false;
    return ok;
}

void RemoveOpenGlassServiceAndTasks() noexcept {
    (void)StopOpenGlassHost();
    GlassInstallUtil::RunHidden(L"schtasks.exe", L"/Delete /TN \"OpenGlass Host\" /F");
    GlassInstallUtil::RunHidden(L"schtasks.exe", L"/Delete /TN \"OpenGlass Legacy Host\" /F");
    const SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return;
    const SC_HANDLE svc = OpenServiceW(scm, L"OpenGlassHost",
        SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    CloseServiceHandle(scm);
    if (!svc) return;
    SERVICE_STATUS status{};
    ControlService(svc, SERVICE_CONTROL_STOP, &status);
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < 15000) {
        if (!QueryServiceStatus(svc, &status)) break;
        if (status.dwCurrentState == SERVICE_STOPPED) break;
        Sleep(250);
    }
    if (DeleteService(svc))
        GlassInstallLog::Line(L"HostStop: OpenGlassHost service deleted");
    else
        GlassInstallLog::LineFormat(L"HostStop: DeleteService failed %lu", GetLastError());
    CloseServiceHandle(svc);
}

void RemovePlatformAutorunTasks(bool removeX64, bool removeX86) noexcept {
    auto erase = [](GP::PlatformArch arch) {
        const wchar_t* name = GP::GlassAutorunTaskName(arch);
        if (!name || !name[0]) return;
        std::wstring args = L"/Delete /TN \"";
        args += name;
        args += L"\" /F";
        GlassInstallUtil::RunHidden(L"schtasks.exe", args);
        GP::InvalidateGlassAutorunTaskCache();
        GlassInstallLog::LineFormat(L"HostStop: removed autorun task %s", name);
    };
    if (removeX64) erase(GP::PlatformArch::X64);
    if (removeX86) erase(GP::PlatformArch::X86);
}

} // namespace GlassInstallHost
