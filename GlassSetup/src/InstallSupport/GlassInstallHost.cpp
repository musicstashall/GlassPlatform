#include "GlassInstallHost.h"
#include "GlassInstallLog.h"
#include "GlassInstallUtil.h"
#include <shlwapi.h>
#include <winsvc.h>

namespace {

namespace GP = GlassShared::GlassPlatformTask;

bool StopGlassPlatformImpl(GP::PlatformArch arch) noexcept {
    if (GP::IsGlassPlatformRunning(arch)) {
        const bool signaled = GP::SignalPlatformStop(arch);
        GlassInstallLog::LineFormat(L"HostStop: SignalPlatformStop %s",
            signaled ? L"ok" : L"failed");
    }
    const ULONGLONG deadline = GetTickCount64() + 45000;
    int emptyResidualStreak = 0;
    while (GetTickCount64() < deadline) {
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
    const std::wstring schtasks = GlassInstallUtil::System64ExePath(L"schtasks.exe");
    GlassInstallUtil::RunHidden(schtasks.c_str(), L"/End /TN \"OpenGlass Host\"");
    GlassInstallUtil::RunHidden(schtasks.c_str(), L"/End /TN \"OpenGlass Legacy Host\"");
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
    const ULONGLONG start = GetTickCount64();
    while (GlassInstallUtil::ImageRunning(L"OpenGlassHost.exe") && GetTickCount64() - start < 20000)
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

bool EnsureGlassAutorunTask(
    GP::PlatformArch arch,
    const std::wstring& installDir,
    bool enableTask,
    bool applyPowerIndependence) noexcept {
    const std::wstring dir = GlassInstallUtil::TrimTrailingSlashes(installDir);
    if (dir.empty()) return false;
    const std::wstring dll = dir + L"\\GlassCompat.dll";
    if (!PathFileExistsW(dll.c_str())) {
        GlassInstallLog::LineFormat(L"HostSetup: GlassCompat.dll missing at %s", dll.c_str());
        return false;
    }
    const std::wstring rundll = GlassInstallUtil::RundllPathForTaskArch(arch);
    const wchar_t* taskName = GP::GlassAutorunTaskName(arch);
    if (!taskName || !taskName[0]) return false;

    const std::wstring schtasks = GlassInstallUtil::System64ExePath(L"schtasks.exe");
    const std::wstring cmd = L"\"" + schtasks + L"\" /Create /TN \"" + taskName
        + L"\" /TR \"\\\"" + rundll + L"\\\" \\\"" + dll + L"\\\",Main /start\" /SC ONLOGON /RL HIGHEST";
    DWORD exitCode = 1;
    const bool created = GlassInstallUtil::RunHiddenCommand(cmd, &exitCode, 25000) && exitCode == 0;
    GP::InvalidateGlassAutorunTaskCache();
    if (!created) {
        GlassInstallLog::LineFormat(L"HostSetup: schtasks /Create failed for %s (exit=%lu)", taskName, exitCode);
        return false;
    }
    if (applyPowerIndependence)
        (void)GP::ApplyAutorunTaskPowerIndependence(arch);
    if (!enableTask)
        (void)GP::SetGlassAutorunTaskSchedulerEnabled(arch, false);
    GP::InvalidateGlassAutorunTaskCache();
    GlassInstallLog::LineFormat(L"HostSetup: ensured autorun task %s (enabled=%d)", taskName, enableTask ? 1 : 0);
    return true;
}

bool EnsureOpenGlassService(
    const std::wstring& openGlassDir,
    DWORD desiredStartType) noexcept {
    const std::wstring dir = GlassInstallUtil::TrimTrailingSlashes(openGlassDir);
    if (dir.empty()) return false;
    const std::wstring exe = dir + L"\\OpenGlassHost.exe";
    if (!PathFileExistsW(exe.c_str())) {
        GlassInstallLog::LineFormat(L"HostSetup: OpenGlassHost.exe missing at %s", exe.c_str());
        return false;
    }

    const std::wstring binPath = L"\"" + exe + L"\"";
    const DWORD startType = (desiredStartType == SERVICE_NO_CHANGE) ? SERVICE_AUTO_START : desiredStartType;

    const SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        GlassInstallLog::LineFormat(L"HostSetup: OpenSCManager failed %lu", GetLastError());
        return false;
    }

    // Creation must never overwrite a service that appeared since the snapshot.
    SC_HANDLE svc = OpenServiceW(scm, L"OpenGlassHost", SERVICE_QUERY_STATUS);
    const DWORD openError=svc?ERROR_SUCCESS:GetLastError();
    if (svc) {
        CloseServiceHandle(svc); CloseServiceHandle(scm);
        GlassInstallLog::Line(L"HostSetup: refusing to replace existing OpenGlassHost service");
        SetLastError(ERROR_SERVICE_EXISTS);
        return false;
    }
    if (openError!=ERROR_SERVICE_DOES_NOT_EXIST) {
        CloseServiceHandle(scm); SetLastError(openError);
        return false;
    }

    svc = CreateServiceW(
        scm,
        L"OpenGlassHost",
        L"OpenGlassHost",
        SERVICE_QUERY_STATUS | SERVICE_START | SERVICE_CHANGE_CONFIG | DELETE,
        SERVICE_WIN32_OWN_PROCESS,
        startType,
        SERVICE_ERROR_NORMAL,
        binPath.c_str(),
        nullptr, nullptr, nullptr, nullptr, nullptr);
    const DWORD err = svc ? 0 : GetLastError();
    if (svc) CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    if (!svc) {
        GlassInstallLog::LineFormat(L"HostSetup: CreateService OpenGlassHost failed %lu", err);
        return false;
    }
    GlassInstallLog::Line(L"HostSetup: created OpenGlassHost service");
    return true;
}

bool StartOpenGlassService() noexcept {
    const SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    const SC_HANDLE svc = OpenServiceW(scm, L"OpenGlassHost", SERVICE_START | SERVICE_QUERY_STATUS);
    CloseServiceHandle(scm);
    if (!svc) return false;
    const BOOL started = StartServiceW(svc, 0, nullptr);
    const DWORD err = started ? ERROR_SUCCESS : GetLastError();
    CloseServiceHandle(svc);
    return started != FALSE || err == ERROR_SERVICE_ALREADY_RUNNING;
}

bool RemoveOpenGlassServiceAndTasks(bool removeTasks) noexcept {
    // Setup rollback removes only its new service, never separately created tasks.
    bool ok=true;
    if (removeTasks) ok=StopOpenGlassHost();
    const std::wstring schtasks=GlassInstallUtil::System64ExePath(L"schtasks.exe");
    if (removeTasks) for (const wchar_t* name : {L"OpenGlass Host",L"OpenGlass Legacy Host"}) {
        const std::wstring tn=std::wstring(L" /TN \"")+name+L"\"";
        if (GlassInstallUtil::RunHidden(schtasks.c_str(),L"/Query"+tn)) {
            if (!GlassInstallUtil::RunHidden(schtasks.c_str(),L"/Delete"+tn+L" /F")) ok=false;
        }
    }
    const SC_HANDLE scm=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT);
    if (!scm) return false;
    const SC_HANDLE svc=OpenServiceW(scm,L"OpenGlassHost",SERVICE_STOP|SERVICE_QUERY_STATUS|DELETE);
    const DWORD openError=svc?ERROR_SUCCESS:GetLastError();
    CloseServiceHandle(scm);
    if (!svc) return ok && (openError==ERROR_SERVICE_DOES_NOT_EXIST || openError==ERROR_SERVICE_MARKED_FOR_DELETE);
    SERVICE_STATUS status{};
    ControlService(svc,SERVICE_CONTROL_STOP,&status);
    const ULONGLONG start=GetTickCount64();
    bool stopped=false;
    while (GetTickCount64()-start<15000) {
        if (!QueryServiceStatus(svc,&status)) { ok=false; break; }
        if (status.dwCurrentState==SERVICE_STOPPED) { stopped=true; break; }
        Sleep(250);
    }
    if (!stopped) { GlassInstallLog::Line(L"HostStop: service did not stop"); ok=false; }
    if (!DeleteService(svc)) {
        const DWORD error=GetLastError();
        if (error!=ERROR_SERVICE_MARKED_FOR_DELETE) { GlassInstallLog::LineFormat(L"HostStop: DeleteService failed %lu",error); ok=false; }
    }
    CloseServiceHandle(svc);
    return ok;
}

bool RemovePlatformAutorunTasks(bool removeX64,bool removeX86) noexcept {
    const std::wstring schtasks=GlassInstallUtil::System64ExePath(L"schtasks.exe");
    auto erase=[&](GP::PlatformArch arch) -> bool {
        const wchar_t* name=GP::GlassAutorunTaskName(arch);
        if (!name || !*name) return false;
        GP::InvalidateGlassAutorunTaskCache();
        if (!GP::GlassAutorunTaskExists(arch)) return true;
        const std::wstring args=std::wstring(L"/Delete /TN \"")+name+L"\" /F";
        const bool removed=GlassInstallUtil::RunHidden(schtasks.c_str(),args);
        GP::InvalidateGlassAutorunTaskCache();
        GlassInstallLog::LineFormat(L"HostStop: removing autorun task %s: %d",name,removed?1:0);
        return removed && !GP::GlassAutorunTaskExists(arch);
    };
    bool ok=true;
    if (removeX64 && !erase(GP::PlatformArch::X64)) ok=false;
    if (removeX86 && !erase(GP::PlatformArch::X86)) ok=false;
    return ok;
}

} // namespace GlassInstallHost
