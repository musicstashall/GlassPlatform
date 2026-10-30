#include "GlassInstallHost.h"
#include "GlassInstallLog.h"
#include "GlassInstallRemoval.h"
#include "GlassInstallUtil.h"
#include <shlwapi.h>
#include <winsvc.h>
#include <vector>

namespace {

namespace GP = GlassShared::GlassPlatformTask;

bool StopGlassPlatformImpl(GP::PlatformArch arch, const std::wstring& installDir,
    const std::atomic_bool* cancelled, bool* stopAttempted, std::wstring* detail,
    bool allowRunning) noexcept {
    if (stopAttempted) *stopAttempted=false;
    if (detail) detail->clear();
    const auto isCancelled=[&]() { return cancelled && cancelled->load(std::memory_order_acquire); };
    if (isCancelled()) { SetLastError(ERROR_CANCELLED); return false; }
    bool running=false;
    if (!GlassInstallHost::QueryGlassPlatformRunning(arch,running)) return false;
    if (!running) {
        GlassInstallLog::LineFormat(L"HostStop: %s already stopped",GP::GlassAutorunTaskName(arch));
        return true;
    }
    if (!allowRunning) {
        GlassInstallLog::LineFormat(L"HostStop: %s started after its state was captured",GP::GlassAutorunTaskName(arch));
        SetLastError(ERROR_INVALID_STATE);
        return false;
    }

    bool attempted=false;
    std::wstring error;
    const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
    const bool accepted=GlassInstallRemoval::StopValidatedPlatformTask(
        arch,installDir,cancelled,attempted,ru,error);
    const DWORD requestError=accepted ? ERROR_SUCCESS : GetLastError();
    if (stopAttempted) *stopAttempted=attempted;
    if (!error.empty()) {
        GlassInstallLog::LineFormat(L"HostStop: %s",error.c_str());
        if (detail) *detail=error;
    }
    if (!attempted) return false;

    // Even a failed Scheduler call can race with shutdown. Wait for an
    // attempted stop to settle before rollback decides whether to restart.
    const ULONGLONG deadline=GetTickCount64()+45000;
    for (;;) {
        if (!GlassInstallHost::QueryGlassPlatformRunning(arch,running)) return false;
        if (!running) break;
        if (GetTickCount64()>=deadline) {
            SetLastError(isCancelled() ? ERROR_CANCELLED : ERROR_TIMEOUT);
            return false;
        }
        Sleep(250);
    }
    if (isCancelled()) { SetLastError(ERROR_CANCELLED); return false; }
    if (!accepted) { SetLastError(requestError); return false; }
    // The separate watch phase, never this task-stop stage, handles residuals.
    return true;
}

} // namespace

namespace GlassInstallHost {

bool QueryGlassPlatformRunning(GP::PlatformArch arch, bool& running) noexcept {
    running=false;
    HANDLE mutex=OpenMutexW(SYNCHRONIZE,FALSE,GP::InstanceMutexName(arch));
    if (mutex) { CloseHandle(mutex);running=true;return true; }
    return GetLastError()==ERROR_FILE_NOT_FOUND;
}

bool StopGlassPlatform(GP::PlatformArch arch, const std::wstring& installDir,
    const std::atomic_bool* cancelled, bool* stopAttempted, std::wstring* detail) noexcept {
    return StopGlassPlatformImpl(arch,installDir,cancelled,stopAttempted,detail,true);
}

bool StopOpenGlassHost(const std::atomic_bool* cancelled, bool* stopRequested) noexcept {
    if (stopRequested) *stopRequested = false;
    const auto isCancelled = [&]() { return cancelled && cancelled->load(std::memory_order_acquire); };
    const auto cancelledResult = [&]() { SetLastError(ERROR_CANCELLED); return false; };
    if (isCancelled()) return cancelledResult();

    bool running = false;
    if (!GlassInstallUtil::QueryOpenGlassRunning(running)) return false;
    if (isCancelled()) return cancelledResult();
    if (!running) {
        GlassInstallLog::Line(L"OpenGlass: service/task already stopped; no shutdown or wait");
        return true;
    }

    GlassInstallUtil::OpenGlassTaskState tasks;
    if (!GlassInstallUtil::QueryOpenGlassTasks(tasks)) return false;
    if (isCancelled()) return cancelledResult();
    if (tasks.anyRunning && (tasks.multipleTasks || tasks.multipleRunningInstances)) {
        SetLastError(ERROR_INVALID_DATA);
        return false;
    }

    const SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    const SC_HANDLE svc = OpenServiceW(scm, L"OpenGlassHost", SERVICE_QUERY_STATUS);
    const bool serviceExists = svc != nullptr;
    const DWORD openError = svc ? ERROR_SUCCESS : GetLastError();
    if (!svc && openError != ERROR_SERVICE_DOES_NOT_EXIST) {
        CloseServiceHandle(scm); SetLastError(openError); return false;
    }

    bool attempted = false;
    if (svc) {
        SERVICE_STATUS_PROCESS status{};
        DWORD needed = 0;
        const BOOL queried = QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
            reinterpret_cast<BYTE*>(&status), sizeof(status), &needed);
        const DWORD queryError = queried ? ERROR_SUCCESS : GetLastError();
        if (!queried) { CloseServiceHandle(svc); CloseServiceHandle(scm); SetLastError(queryError); return false; }
        if (isCancelled()) { CloseServiceHandle(svc); CloseServiceHandle(scm); return cancelledResult(); }
        if (status.dwCurrentState != SERVICE_STOPPED && status.dwCurrentState != SERVICE_STOP_PENDING) {
            // Do not demand SERVICE_STOP rights for a service already stopped.
            const SC_HANDLE stopper = OpenServiceW(scm, L"OpenGlassHost", SERVICE_STOP);
            const DWORD accessError = stopper ? ERROR_SUCCESS : GetLastError();
            if (!stopper) { CloseServiceHandle(svc); CloseServiceHandle(scm); SetLastError(accessError); return false; }
            if (isCancelled()) { CloseServiceHandle(stopper); CloseServiceHandle(svc); CloseServiceHandle(scm); return cancelledResult(); }
            SERVICE_STATUS stoppedStatus{};
            // A failed call can still race with the service shutting down.
            attempted = true;
            if (stopRequested) *stopRequested = true;
            const BOOL sent = ControlService(stopper, SERVICE_CONTROL_STOP, &stoppedStatus);
            const DWORD error = sent ? ERROR_SUCCESS : GetLastError();
            CloseServiceHandle(stopper);
            if (!sent && error != ERROR_SERVICE_NOT_ACTIVE) {
                CloseServiceHandle(svc); CloseServiceHandle(scm); SetLastError(error); return false;
            }
        }
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);

    if (!isCancelled() && tasks.anyRunning) {
        bool taskRequested = false;
        const bool taskOk = GlassInstallUtil::StopRunningOpenGlassTasks(taskRequested, cancelled);
        attempted = attempted || taskRequested;
        if (taskRequested && stopRequested) *stopRequested = true;
        if (!taskOk && !isCancelled()) return false;
    }
    if (isCancelled() && !attempted) return cancelledResult();
    if (!serviceExists && !tasks.found) {
        // A manually started, unowned host has no safe restart/stop backend.
        SetLastError(ERROR_NOT_SUPPORTED);
        return false;
    }

    // If a stop was sent, let it settle before rollback can restart the host;
    // otherwise a late stop could kill the just-restored instance.
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        if (!GlassInstallUtil::QueryOpenGlassRunning(running)) return false;
        if (!running) return isCancelled() ? cancelledResult() : true;
        if (isCancelled() && !attempted) return cancelledResult();
        if (GetTickCount64() - start >= 20000) {
            SetLastError(isCancelled() ? ERROR_CANCELLED : ERROR_TIMEOUT);
            return false;
        }
        Sleep(250);
    }
}

bool StopAllForUninstall(
    const std::wstring& x64InstallDir,
    const std::wstring& x86InstallDir,
    bool stopX64,
    bool stopX86,
    bool stopOpenGlass,
    const std::atomic_bool* cancelled,
    bool* stopAttemptedX64,
    bool* stopAttemptedX86,
    const bool* wasRunningX64,
    const bool* wasRunningX86) noexcept {
    if (stopAttemptedX64) *stopAttemptedX64=false;
    if (stopAttemptedX86) *stopAttemptedX86=false;
    GlassInstallUtil::CloseGlassSettingsProcesses(cancelled);
    if (cancelled && cancelled->load(std::memory_order_acquire)) return false;
    if (stopX64 && !StopGlassPlatformImpl(GP::PlatformArch::X64,x64InstallDir,cancelled,
            stopAttemptedX64,nullptr,!wasRunningX64 || *wasRunningX64)) return false;
    if (cancelled && cancelled->load(std::memory_order_acquire)) return false;
    if (stopX86 && !StopGlassPlatformImpl(GP::PlatformArch::X86,x86InstallDir,cancelled,
            stopAttemptedX86,nullptr,!wasRunningX86 || *wasRunningX86)) return false;
    if (cancelled && cancelled->load(std::memory_order_acquire)) return false;
    return !stopOpenGlass || StopOpenGlassHost(cancelled);
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

bool QueryOpenGlassServiceStartType(bool& exists, DWORD& startType) noexcept {
    exists = false;
    startType = SERVICE_AUTO_START;
    const SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    const SC_HANDLE svc = OpenServiceW(scm, L"OpenGlassHost", SERVICE_QUERY_CONFIG);
    const DWORD openError = svc ? ERROR_SUCCESS : GetLastError();
    CloseServiceHandle(scm);
    if (!svc) {
        if (openError == ERROR_SERVICE_DOES_NOT_EXIST) return true;
        SetLastError(openError);
        return false;
    }
    exists = true;
    DWORD needed = 0;
    (void)QueryServiceConfigW(svc, nullptr, 0, &needed);
    if (!needed || needed >= 65536) {
        const DWORD error = GetLastError();
        CloseServiceHandle(svc); SetLastError(error ? error : ERROR_INVALID_DATA); return false;
    }
    try {
        std::vector<BYTE> buffer(needed);
        auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
        if (!QueryServiceConfigW(svc, config, needed, &needed)) {
            const DWORD error = GetLastError();
            CloseServiceHandle(svc); SetLastError(error); return false;
        }
        startType = config->dwStartType;
    } catch (...) {
        CloseServiceHandle(svc); SetLastError(ERROR_OUTOFMEMORY); return false;
    }
    CloseServiceHandle(svc);
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

bool StartOpenGlassServicePreservingStartType(DWORD startType) noexcept {
    const SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    const bool disabled = startType == SERVICE_DISABLED;
    const SC_HANDLE svc = OpenServiceW(scm, L"OpenGlassHost", SERVICE_START | SERVICE_QUERY_STATUS |
        (disabled ? SERVICE_CHANGE_CONFIG : 0));
    CloseServiceHandle(scm);
    if (!svc) return false;
    if (disabled && !ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_DEMAND_START,
            SERVICE_NO_CHANGE, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr)) {
        GlassInstallLog::LineFormat(L"OpenGlass: could not temporarily start disabled service (%lu)", GetLastError());
        CloseServiceHandle(svc);
        return false;
    }
    const BOOL started = StartServiceW(svc, 0, nullptr);
    const DWORD startError = started ? ERROR_SUCCESS : GetLastError();
    // Disabling a running service does not stop it; restore the exact preference
    // even when StartService fails. Never silently leave autostart changed.
    const BOOL restored = !disabled || ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_DISABLED,
        SERVICE_NO_CHANGE, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    const DWORD restoreError = restored ? ERROR_SUCCESS : GetLastError();
    CloseServiceHandle(svc);
    if (!restored) {
        GlassInstallLog::LineFormat(L"OpenGlass: could not restore disabled service preference (%lu)", restoreError);
        SetLastError(restoreError);
        return false;
    }
    if (!started && startError != ERROR_SERVICE_ALREADY_RUNNING) SetLastError(startError);
    return started != FALSE || startError == ERROR_SERVICE_ALREADY_RUNNING;
}

bool StartOpenGlassService() noexcept {
    return StartOpenGlassServicePreservingStartType(SERVICE_DEMAND_START);
}

bool RestoreOpenGlassServiceStartType(DWORD startType) noexcept {
    const SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    const SC_HANDLE svc = OpenServiceW(scm, L"OpenGlassHost", SERVICE_CHANGE_CONFIG);
    CloseServiceHandle(scm);
    if (!svc) return false;
    const BOOL ok = ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, startType, SERVICE_NO_CHANGE,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    CloseServiceHandle(svc);
    if (!ok) SetLastError(error);
    return ok != FALSE;
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
