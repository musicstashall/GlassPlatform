#include "InstallEngine.h"
#include "InstallLog.h"
#include "WizardProgress.h"
#include "InstallSupport/GlassInstallHost.h"
#include "InstallSupport/GlassInstallMsi.h"
#include "InstallSupport/GlassInstallUtil.h"
#include "InstallSupport/GlassInstallUnlock.h"
#include "InstallSupport/GlassInstallWatch.h"
#include "../res/resource.h"
#include <GlassPlatformTask.h>
#include <OpenGlassHostLaunch.h>
#include <OpenGlassHostState.h>
#include <msi.h>
#include <msiquery.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <shellapi.h>
#include <winsvc.h>
#include <cstring>
#include <iterator>
#include <vector>

namespace {

namespace GP = GlassShared::GlassPlatformTask;

struct ResourceBlob {
    UINT id = 0;
    const wchar_t* fileName = nullptr;
    bool needed = false;
};

bool ServiceStopped(const wchar_t* serviceName) noexcept {
    const SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return true;
    const SC_HANDLE svc = OpenServiceW(scm, serviceName, SERVICE_QUERY_STATUS);
    CloseServiceHandle(scm);
    if (!svc) return true;
    SERVICE_STATUS_PROCESS status{};
    DWORD needed = 0;
    const BOOL ok = QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
        reinterpret_cast<BYTE*>(&status), sizeof(status), &needed);
    CloseServiceHandle(svc);
    if (!ok) return false;
    return status.dwCurrentState == SERVICE_STOPPED;
}

bool QueryServiceConfigStartType(const wchar_t* serviceName, bool& exists, DWORD& startType) noexcept {
    exists = false;
    startType = SERVICE_AUTO_START;
    const SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    const SC_HANDLE svc = OpenServiceW(scm, serviceName, SERVICE_QUERY_CONFIG);
    const DWORD openError=svc?ERROR_SUCCESS:GetLastError();
    CloseServiceHandle(scm);
    if (!svc) {
        if (openError==ERROR_SERVICE_DOES_NOT_EXIST) return true;
        SetLastError(openError);
        return false;
    }
    exists = true;
    DWORD needed = 0;
    (void)QueryServiceConfigW(svc, nullptr, 0, &needed);
    if (!needed || needed>=65536) {
        const DWORD error=GetLastError();
        CloseServiceHandle(svc); SetLastError(error?error:ERROR_INVALID_DATA); return false;
    }
    std::vector<BYTE> buf(needed);
    auto* cfg = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buf.data());
    if (!QueryServiceConfigW(svc, cfg, needed, &needed)) {
        const DWORD error=GetLastError();
        CloseServiceHandle(svc); SetLastError(error); return false;
    }
    startType = cfg->dwStartType;
    CloseServiceHandle(svc);
    return true;
}

bool ProductWasSnapshotted(const GlassInstallRollback::SessionState& state, const wchar_t* code) noexcept {
    if (!code) return false;
    for (const auto& product : state.oldProducts)
        if (_wcsicmp(product.productCode.c_str(), code) == 0) return true;
    return false;
}

std::wstring LongTempPath(const std::wstring& path) noexcept {
    if (path.size() < 4 || path.rfind(L"\\\\?\\", 0) == 0) return path;
    if (path.size() < MAX_PATH) return path;
    return L"\\\\?\\" + path;
}

bool RemoveDirectoryTree(const std::wstring& dir) noexcept {
    const std::wstring root = LongTempPath(dir);
    const DWORD attr = GetFileAttributesW(root.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return true;
    if ((attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        SetFileAttributesW(root.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (DeleteFileW(root.c_str())) return true;
        InstallLog::LineFormat(L"DeleteFile failed %s (%lu)", root.c_str(), GetLastError());
        return false;
    }
    std::wstring pattern = root;
    if (pattern.back() != L'\\') pattern += L'\\';
    pattern += L'*';
    WIN32_FIND_DATAW fd{};
    const HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
    bool ok = true;
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == L'.' && (!fd.cFileName[1] || (fd.cFileName[1] == L'.' && !fd.cFileName[2])))
                continue;
            std::wstring child = root;
            if (child.back() != L'\\') child += L'\\';
            child += fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!RemoveDirectoryTree(child)) ok = false;
            } else {
                SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                if (!DeleteFileW(child.c_str())) {
                    InstallLog::LineFormat(L"DeleteFile failed %s (%lu)", child.c_str(), GetLastError());
                    ok = false;
                }
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    SetFileAttributesW(root.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!RemoveDirectoryW(root.c_str())) {
        InstallLog::LineFormat(L"RemoveDirectory failed %s (%lu)", root.c_str(), GetLastError());
        ok = false;
    }
    return ok;
}

void RemoveEmptyChildDir(const std::wstring& parent, const wchar_t* child) noexcept {
    if (parent.empty() || !child || !child[0]) return;
    std::wstring path = GlassInstallUtil::TrimTrailingSlashes(parent) + L"\\" + child;
    const DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) == 0) return;
    std::wstring pattern = path + L"\\*";
    WIN32_FIND_DATAW fd{};
    const HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    bool empty = true;
    do {
        if (fd.cFileName[0] == L'.' && (!fd.cFileName[1] || (fd.cFileName[1] == L'.' && !fd.cFileName[2])))
            continue;
        empty = false;
        break;
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    if (!empty) return;
    if (RemoveDirectoryW(path.c_str()))
        InstallLog::LineFormat(L"Removed empty directory %s", path.c_str());
}

bool CompatDllExists(const std::wstring& root) noexcept {
    if (root.empty()) return false;
    const std::wstring dll = GlassInstallUtil::TrimTrailingSlashes(root) + L"\\GlassCompat.dll";
    return PathFileExistsW(dll.c_str()) != FALSE;
}

std::wstring PlatformRoot(GP::PlatformArch arch) noexcept {
    const bool x64 = arch == GP::PlatformArch::X64;
    std::wstring root;
    if (GlassInstallUtil::QueryPlatformInstallPath(x64, root) && CompatDllExists(root))
        return root;
    root = GP::TryGetAutorunGlassCompatInstallRoot(arch);
    if (CompatDllExists(root))
        return root;
    PWSTR folder=nullptr;
    HRESULT hr=SHGetKnownFolderPath(x64?FOLDERID_ProgramFilesX64:FOLDERID_ProgramFilesX86,KF_FLAG_DEFAULT,nullptr,&folder);
    if (FAILED(hr) && !x64) hr=SHGetKnownFolderPath(FOLDERID_ProgramFiles,KF_FLAG_DEFAULT,nullptr,&folder);
    if (FAILED(hr) || !folder) return {};
    root = std::wstring(folder) + L"\\GlassPlatform";
    CoTaskMemFree(folder);
    return CompatDllExists(root) ? root : std::wstring{};
}

bool OpenGlassPresent(const std::wstring& openGlassDir) noexcept {
    if (GlassInstallUtil::ImageRunning(L"OpenGlassHost.exe"))
        return true;
    if (GlassInstallMsi::IsProductInstalled(GlassInstallMsi::kProductOpenGlass) ||
        GlassInstallMsi::IsProductInstalled(GlassInstallMsi::kLegacyOpenGlass))
        return true;
    if (!openGlassDir.empty() && PathFileExistsW((GlassInstallUtil::TrimTrailingSlashes(openGlassDir) + L"\\OpenGlassHost.exe").c_str()))
        return true;
    const SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    const SC_HANDLE svc = OpenServiceW(scm, L"OpenGlassHost", SERVICE_QUERY_STATUS);
    CloseServiceHandle(scm);
    if (!svc) return false;
    CloseServiceHandle(svc);
    return true;
}

enum class ArchPresence { Running, TaskStopped, Files, Absent };

ArchPresence ClassifyArch(GP::PlatformArch arch) noexcept {
    if (GP::IsGlassPlatformRunning(arch))
        return ArchPresence::Running;
    if (GP::GlassAutorunTaskExists(arch))
        return ArchPresence::TaskStopped;
    const wchar_t* prod = (arch == GP::PlatformArch::X64)
        ? GlassInstallMsi::kProductPlatformX64
        : GlassInstallMsi::kProductPlatformX86;
    const wchar_t* legacy = (arch == GP::PlatformArch::X64)
        ? GlassInstallMsi::kLegacyPlatformX64 : GlassInstallMsi::kLegacyPlatformX86;
    if (GlassInstallMsi::IsProductInstalled(prod) || GlassInstallMsi::IsProductInstalled(legacy))
        return ArchPresence::Files;
    if (!PlatformRoot(arch).empty())
        return ArchPresence::Files;
    return ArchPresence::Absent;
}

bool IsProtectedProcessName(const wchar_t* exe) noexcept {
    if (!exe || !exe[0]) return true;
    const wchar_t* blocked[] = {
        L"System", L"Registry", L"smss.exe", L"csrss.exe", L"wininit.exe",
        L"services.exe", L"lsass.exe", L"winlogon.exe", L"dwm.exe",
        L"fontdrvhost.exe", L"Memory Compression", L"Secure System",
    };
    for (const wchar_t* name : blocked)
        if (_wcsicmp(exe, name) == 0) return true;
    return false;
}

} // namespace

InstallEngine::~InstallEngine() noexcept {
    cancelRequested_.store(true, std::memory_order_release);
    if (holdGate_) {
        holdAction_ = HoldAction::Cancel;
        SetEvent(holdGate_);
    }
    if (errorPageShown_) SetEvent(errorPageShown_);
    if (decisionGate_) SetEvent(decisionGate_);
    if (thread_) {
        WaitForSingleObject(thread_, INFINITE);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    if (holdGate_) {
        CloseHandle(holdGate_);
        holdGate_ = nullptr;
    }
    if (decisionGate_) { CloseHandle(decisionGate_); decisionGate_=nullptr; }
    if (errorPageShown_) {
        CloseHandle(errorPageShown_);
        errorPageShown_ = nullptr;
    }
    if (!preserveSnapshot_) GlassInstallRollback::CleanupSnapshot(rollbackSession_);
    if (!tempDir_.empty()) {
        (void)RemoveDirectoryTree(tempDir_);
        tempDir_.clear();
    }
}

bool InstallEngine::PauseForCancelDecision() noexcept {
    AcquireSRWLockExclusive(&statusLock_);
    const bool allowed=started_ && !finished_.load() && !launchingPhase_.load() &&
        !rollingBack_.load() && !cancelRequested_.load() && decisionGate_;
    if (allowed) ResetEvent(decisionGate_);
    ReleaseSRWLockExclusive(&statusLock_);
    return allowed;
}

void InstallEngine::ResumeAfterCancelDecision() noexcept {
    if (decisionGate_) SetEvent(decisionGate_);
}

void InstallEngine::RequestCancel() noexcept {
    // Coordinate the commit/launch boundary with the worker: never accept cancellation
    // after the final launch has begun, and never lose a wake-up in the unlocker.
    AcquireSRWLockExclusive(&statusLock_);
    const bool accepted=started_ && !finished_.load() && !launchingPhase_.load() && !rollingBack_.load();
    if (accepted) {
        cancelRequested_.store(true,std::memory_order_release);
        holdAction_=HoldAction::Cancel;
        holdKind_=HoldKind::None;
    }
    ReleaseSRWLockExclusive(&statusLock_);
    if (!accepted) return;
    InstallLog::Line(L"InstallEngine: user requested cancel");
    if (holdGate_) SetEvent(holdGate_);
    if (decisionGate_) SetEvent(decisionGate_);
    if (errorPageShown_) SetEvent(errorPageShown_);
}

bool InstallEngine::CapturePriorHostState() noexcept {
    prior_ = {};
    GP::InvalidateGlassAutorunTaskCache();
    prior_.x64TaskExisted = GP::GlassAutorunTaskExists(GP::PlatformArch::X64);
    prior_.x64TaskEnabled = GP::QueryGlassAutorunUiState(GP::PlatformArch::X64) != GP::AutorunUiState::Disabled;
    prior_.x64WasRunning = GP::IsGlassPlatformRunning(GP::PlatformArch::X64);

    prior_.x86TaskExisted = GP::GlassAutorunTaskExists(GP::PlatformArch::X86);
    prior_.x86TaskEnabled = GP::QueryGlassAutorunUiState(GP::PlatformArch::X86) != GP::AutorunUiState::Disabled;
    prior_.x86WasRunning = GP::IsGlassPlatformRunning(GP::PlatformArch::X86);

    if (!QueryServiceConfigStartType(L"OpenGlassHost", prior_.ogServiceExisted, prior_.ogServiceStartType)) {
        const DWORD error=GetLastError();
        InstallLog::LineFormat(L"Could not inspect prior OpenGlass service state (%lu)",error);
        SetFailure(labels_.snapshotFailed.empty()?L"Could not inspect the existing service; no changes were made.":labels_.snapshotFailed,error);
        return false;
    }
    prior_.ogLegacyTaskExisted = Shared::OpenGlassHostState::LegacyHostTaskExists();
    if (prior_.ogLegacyTaskExisted && !prior_.ogServiceExisted)
        prior_.ogLegacyTaskEnabled = Shared::OpenGlassHostState::QueryOrchestrationUiState()!=Shared::OpenGlassHostState::HostUiState::Disabled;
    prior_.ogWasRunning = GlassInstallUtil::ImageRunning(L"OpenGlassHost.exe") || !ServiceStopped(L"OpenGlassHost");
    prior_.shortcutExisted = GlassInstallUtil::StartMenuShortcutExists();
    return true;
}

bool InstallEngine::RestorePriorHosts(bool selectedOnly) noexcept {
    if (!hostsStopped_) return true;
    bool ok=true;
    auto restoreArch=[&](GP::PlatformArch arch,const std::wstring& root,bool existed,bool enabled,bool running,bool stopSignalled) -> bool {
        if (!running) return true;
        if (stopSignalled) (void)GP::WaitForPlatformStopped(arch,30000);
        if (GP::IsGlassPlatformRunning(arch)) return true;
        if (existed && enabled) GP::StartPlatform(arch);
        else {
            // A manually started host may have no task or a disabled task. Do not enable it.
            const std::wstring rundll=arch==GP::PlatformArch::X64?GlassInstallUtil::NativeRundll64():GlassInstallUtil::RundllPathForTaskArch(arch);
            const std::wstring dll=GlassInstallUtil::TrimTrailingSlashes(root)+L"\\GlassCompat.dll";
            if (rundll.empty() || !PathFileExistsW(dll.c_str())) return false;
            std::wstring command=L"\""+rundll+L"\" \""+dll+L"\",Main /start";
            STARTUPINFOW si{}; si.cb=sizeof(si); si.dwFlags=STARTF_USESHOWWINDOW; si.wShowWindow=SW_HIDE;
            PROCESS_INFORMATION pi{};
            if (!CreateProcessW(rundll.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi)) return false;
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        }
        const ULONGLONG start=GetTickCount64();
        while (!GP::IsGlassPlatformRunning(arch) && GetTickCount64()-start<30000) Sleep(200);
        return GP::IsGlassPlatformRunning(arch);
    };
    // During success only restore manually running *selected* platforms whose
    // tasks were disabled. Unselected products are being removed, not restarted.
    if (!selectedOnly || (selection_.platform64 && prior_.x64WasRunning && !prior_.x64TaskEnabled))
        if (!restoreArch(GP::PlatformArch::X64,x64Dir_,prior_.x64TaskExisted,prior_.x64TaskEnabled,prior_.x64WasRunning,x64StopSignalled_)) ok=false;
    if (!selectedOnly || (selection_.platform32 && prior_.x86WasRunning && !prior_.x86TaskEnabled))
        if (!restoreArch(GP::PlatformArch::X86,x86Dir_,prior_.x86TaskExisted,prior_.x86TaskEnabled,prior_.x86WasRunning,x86StopSignalled_)) ok=false;
    if (selectedOnly) return ok; // selected OpenGlass is handled by the launch phase
    if (!prior_.ogServiceExisted && prior_.ogLegacyTaskExisted && prior_.ogWasRunning && ogStopSignalled_) {
        // TryStartup enables a disabled legacy task; never change that user preference.
        if (!prior_.ogLegacyTaskEnabled || !Shared::OpenGlassHostLaunch::TryStartup()) ok=false;
    }
    if (prior_.ogServiceExisted) {
        const SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (scm) {
            const SC_HANDLE svc = OpenServiceW(scm, L"OpenGlassHost", SERVICE_START);
            CloseServiceHandle(scm);
            if (svc) {
                // Existing service configuration was never changed; do not overwrite it.
                if (prior_.ogWasRunning && ogStopSignalled_) {
                    const BOOL started=StartServiceW(svc,0,nullptr);
                    if (!started && GetLastError()!=ERROR_SERVICE_ALREADY_RUNNING) ok=false;
                }
                CloseServiceHandle(svc);
            } else if (prior_.ogWasRunning && ogStopSignalled_) ok=false;
        } else if (prior_.ogWasRunning && ogStopSignalled_) ok=false;
    }
    return ok;
}

DWORD WINAPI InstallEngine::WorkerThread(LPVOID p) noexcept {
            auto* self = static_cast<InstallEngine*>(p);
            const bool priorStateOk=self->CapturePriorHostState();
            if (priorStateOk) self->DetermineCleanInstall();
            if (self->forceClean_)
                InstallLog::Line(L"Clean reinstall: remnants will be removed before install");

            bool ok = priorStateOk && !self->IsCancelRequested() && self->ExtractPayload(self->instance_);
            bool snapshotFailed = !priorStateOk && !self->IsCancelRequested();

            // Снимок до стопа хостов: если на диске нет места или нажата Отмена, система не затрагивается.
            if (ok) {
                // Snapshot even on a clean/reinstall path: pre-existing user files must survive cancellation.
                ok = self->CreateRollbackSnapshot();
                snapshotFailed = !ok && !self->IsCancelRequested();
            }

            // Inspect and back up non-MSI auxiliaries before stopping hosts or
            // touching MSI. Unsupported credentials or foreign tasks fail closed.
            if (ok && !self->IsCancelRequested()) {
                ok = self->PrepareProductRemoval();
                snapshotFailed = !ok && !self->IsCancelRequested();
            }

            if (ok && !self->IsCancelRequested()) {
                if (self->forceClean_) {
                    GlassInstallUtil::CloseGlassSettingsProcesses();
                    ok = self->StopRunningHosts();
                    if (ok && !self->IsCancelRequested()) ok = self->ReleaseLockedModules();
                    if (ok && !self->IsCancelRequested()) {
                        self->installAttempted_ = true;
                        ok = self->PurgeRemnants();
                    }
                    if (ok) self->cleanInstall_ = true;
                } else if (!self->cleanInstall_) {
                    GlassInstallUtil::CloseGlassSettingsProcesses();
                    ok = self->StopRunningHosts();
                    if (ok && !self->IsCancelRequested()) ok = self->ReleaseLockedModules();
                } else {
                    GlassInstallUtil::CloseGlassSettingsProcesses();
                }
            }

            // GlassSettings ставится строго ПОСЛЕ ReleaseLockedModules — до MSI ни один пакет не тронут.
            if (ok && !self->IsCancelRequested()) {
                self->installAttempted_ = true;
                ok = self->RunMsiSequence();
            }

            // Финальная стадия запуска компонентов (Page::Launching >= 5 секунд).
            bool beginLaunch=false;
            while (ok) {
                if (self->decisionGate_) WaitForSingleObject(self->decisionGate_,INFINITE);
                AcquireSRWLockExclusive(&self->statusLock_);
                if (self->cancelRequested_.load(std::memory_order_acquire)) {
                    ReleaseSRWLockExclusive(&self->statusLock_);
                    break;
                }
                const bool ready=!self->decisionGate_ || WaitForSingleObject(self->decisionGate_,0)==WAIT_OBJECT_0;
                if (ready) {
                    self->launchingPhase_.store(true,std::memory_order_release);
                    beginLaunch=true;
                }
                ReleaseSRWLockExclusive(&self->statusLock_);
                if (ready) break;
            }
            if (beginLaunch) {
                const ULONGLONG launchStart = GetTickCount64();
                ok = self->LaunchAndVerifySelectedProducts();
                if (ok) {
                    constexpr ULONGLONG kMinLaunchMs = 5000;
                    for (;;) {
                        const ULONGLONG elapsed = GetTickCount64() - launchStart;
                        if (elapsed >= kMinLaunchMs)
                            break;
                        const ULONGLONG remain = kMinLaunchMs - elapsed;
                        Sleep(remain > 150 ? 150 : static_cast<DWORD>(remain));
                    }
                    // No cancellation is accepted after the launch boundary.
                    // The last step may delete a service whose credentials cannot
                    // be snapshotted; no fallible work follows that commit.
                    const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
                    std::wstring removalError;
                    ok = GlassInstallRemoval::Commit(self->removalSession_, ru, removalError);
                    if (!ok) self->SetFailure(removalError.empty()
                        ? (ru ? L"Не удалось завершить удаление компонентов." : L"Could not finish removing components.")
                        : removalError, 0);
                    else self->SetStatus(self->labels_.finalize, 10000);
                }
            }

            MsiEnableLogW(0, nullptr, 0);

            const bool cancelled = self->IsCancelRequested() || self->quietClose_.load();
            bool restored=true;
            const auto restoreRemovalArtifacts = [&]() {
                const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
                std::wstring detail;
                if (!GlassInstallRemoval::Restore(self->removalSession_, ru, detail)) {
                    restored=false;
                    InstallLog::LineFormat(L"Could not restore removal auxiliaries: %s", detail.c_str());
                    AcquireSRWLockExclusive(&self->statusLock_);
                    if (!detail.empty()) {
                        if (!self->failureDetail_.empty()) self->failureDetail_ += L"\r\n\r\n";
                        self->failureDetail_ += detail;
                    }
                    ReleaseSRWLockExclusive(&self->statusLock_);
                }
            };
            if (!ok || cancelled) {
                if (cancelled) {
                    self->rollingBack_.store(true, std::memory_order_release);
                    if (self->installAttempted_) {
                        if (self->rollbackSession_.hadPriorInstall) {
                            restored=self->PerformRollback();
                            restoreRemovalArtifacts();
                            if (!self->RestorePriorHosts()) restored=false;
                        } else {
                            restored=self->UndoCleanInstall();
                            restoreRemovalArtifacts();
                            if (!self->RestorePriorHosts()) restored=false;
                        }
                    } else {
                        // Отмена до изменения MSI/файлов — возвращаем остановленные хосты в прежнее состояние.
                        if (!self->RestorePriorHosts()) restored=false;
                    }
                    self->rollingBack_.store(false, std::memory_order_release);
                    self->userCancelled_.store(true, std::memory_order_release);
                    ok = false;
                } else {
                    const bool cleanAttempt = self->installAttempted_ && !self->rollbackSession_.hadPriorInstall;
                    if (self->rollbackSession_.hadPriorInstall && !snapshotFailed && self->installAttempted_) {
                        InterlockedExchange(&self->errorPageRequested_, 1);
                        if (self->errorPageShown_)
                            WaitForSingleObject(self->errorPageShown_, 20000);
                        self->rollingBack_.store(true, std::memory_order_release);
                        restored=self->PerformRollback();
                        restoreRemovalArtifacts();
                        if (!self->RestorePriorHosts()) restored=false;
                        self->rollingBack_.store(false, std::memory_order_release);
                    } else if (snapshotFailed) {
                        InstallLog::Line(L"Snapshot failed; installation was not started");
                        InterlockedExchange(&self->errorPageRequested_, 1);
                    } else if (cleanAttempt) {
                        InterlockedExchange(&self->errorPageRequested_, 1);
                        if (self->errorPageShown_)
                            WaitForSingleObject(self->errorPageShown_, 20000);
                        self->rollingBack_.store(true, std::memory_order_release);
                        restored=self->UndoCleanInstall();
                        restoreRemovalArtifacts();
                        if (!self->RestorePriorHosts()) restored=false;
                        self->rollingBack_.store(false, std::memory_order_release);
                    } else {
                        InstallLog::LineFormat(L"Install failed before MSI changes: %s", self->error_.c_str());
                        if (!self->RestorePriorHosts()) restored=false;
                        InterlockedExchange(&self->errorPageRequested_, 1);
                    }
                }
            }

            self->rollbackSucceeded_.store(restored,std::memory_order_release);
            if (restored) GlassInstallRollback::CleanupSnapshot(self->rollbackSession_);
            else {
                self->preserveSnapshot_=true;
                const std::wstring backup=self->rollbackSession_.workspaceDir;
                AcquireSRWLockExclusive(&self->statusLock_);
                self->rollbackNote_=self->labels_.rollbackFailed;
                if (!backup.empty()) self->rollbackNote_+=L"\r\nBackup: "+backup;
                ReleaseSRWLockExclusive(&self->statusLock_);
                InstallLog::LineFormat(L"Rollback failed; retaining backup: %s",backup.c_str());
            }
            if (!self->tempDir_.empty()) {
                (void)RemoveDirectoryTree(self->tempDir_);
                self->tempDir_.clear();
            }

            self->succeeded_ = ok && !self->userCancelled_.load(std::memory_order_acquire);
            self->finished_.store(true,std::memory_order_release);
            self->launchingPhase_.store(false,std::memory_order_release);
            return 0;
}

bool InstallEngine::Start(
    HINSTANCE instance,
    const std::wstring& x64Dir,
    const std::wstring& x86Dir,
    const std::wstring& openGlassDir,
    ComponentSelection selection,
    bool forceClean,
    PhaseLabels labels,
    StatusFn onStatus) noexcept {
    if (started_) return false;
    started_ = true;
    instance_ = instance;
    labels_ = std::move(labels);
    status_ = std::move(onStatus);
    selection_ = selection;
    forceClean_ = forceClean;
    x64Dir_ = x64Dir;
    x86Dir_ = x86Dir;
    openGlassDir_ = openGlassDir;
    cancelRequested_.store(false, std::memory_order_relaxed);
    userCancelled_.store(false, std::memory_order_relaxed);
    rollingBack_.store(false, std::memory_order_relaxed);
    launchingPhase_.store(false, std::memory_order_relaxed);

    selection_=GlassProducts::NormalizeInstall(selection_,GlassInstallUtil::IsOs64Bit());

    GlassInstallUtil::ConfigureEmbeddedUnlocker(instance, IDR_UNLOCKER_X64);

    holdGate_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    decisionGate_ = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    errorPageShown_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!holdGate_ || !decisionGate_ || !errorPageShown_) {
        SetFailure(L"CreateEvent", 0);
        finished_ = true;
        return false;
    }
    InstallLog::Initialize();
    InstallLog::Line(L"InstallEngine::Start");
    InstallLog::LineFormat(L"  x64Dir=%s (sel=%d)", x64Dir_.c_str(), selection_.platform64 ? 1 : 0);
    InstallLog::LineFormat(L"  x86Dir=%s (sel=%d)", x86Dir_.c_str(), selection_.platform32 ? 1 : 0);
    InstallLog::LineFormat(L"  glassSettings=%d openGlass=%d (%s)",
        selection_.glassSettings ? 1 : 0, selection_.openGlass ? 1 : 0, openGlassDir_.c_str());
    progressUnits_ = 0;
    statusLine_ = labels_.extract;

    thread_ = CreateThread(nullptr, 0, &InstallEngine::WorkerThread, this, 0, nullptr);
    if (!thread_) {
        SetFailure(L"CreateThread", 0);
        finished_ = true;
        return false;
    }
    return true;
}

bool InstallEngine::Poll() noexcept {
    if (!started_) return false;
    if (finished_) {
        if (thread_) {
            WaitForSingleObject(thread_, INFINITE);
            CloseHandle(thread_);
            thread_ = nullptr;
            if (succeeded_) SetStatus(L"", 10000);
        }
        return true;
    }
    if (thread_) {
        const DWORD wait = WaitForSingleObject(thread_, 0);
        if (wait == WAIT_OBJECT_0) {
            finished_ = true;
            CloseHandle(thread_);
            thread_ = nullptr;
            if (succeeded_) SetStatus(L"", 10000);
            return true;
        }
    }
    return false;
}

std::wstring InstallEngine::ErrorText() const noexcept {
    AcquireSRWLockShared(const_cast<PSRWLOCK>(&statusLock_));
    std::wstring text = failureDetail_;
    if (!rollbackNote_.empty()) {
        if (!text.empty()) text += L"\r\n\r\n";
        text += rollbackNote_;
    }
    if (text.empty()) text = error_;
    ReleaseSRWLockShared(const_cast<PSRWLOCK>(&statusLock_));
    return text;
}

std::wstring InstallEngine::FailureDetail() const noexcept {
    AcquireSRWLockShared(const_cast<PSRWLOCK>(&statusLock_));
    const std::wstring copy = failureDetail_;
    ReleaseSRWLockShared(const_cast<PSRWLOCK>(&statusLock_));
    return copy;
}

std::wstring InstallEngine::MsiMessage() const noexcept {
    AcquireSRWLockShared(const_cast<PSRWLOCK>(&statusLock_));
    const std::wstring copy = msiMessage_;
    ReleaseSRWLockShared(const_cast<PSRWLOCK>(&statusLock_));
    return copy;
}

std::wstring InstallEngine::RollbackNote() const noexcept {
    AcquireSRWLockShared(const_cast<PSRWLOCK>(&statusLock_));
    const std::wstring copy = rollbackNote_;
    ReleaseSRWLockShared(const_cast<PSRWLOCK>(&statusLock_));
    return copy;
}

unsigned InstallEngine::MsiExitCode() const noexcept {
    AcquireSRWLockShared(const_cast<PSRWLOCK>(&statusLock_));
    const unsigned code = msiExitCode_;
    ReleaseSRWLockShared(const_cast<PSRWLOCK>(&statusLock_));
    return code;
}

bool InstallEngine::TakeErrorPageRequest() noexcept {
    return InterlockedExchange(&errorPageRequested_, 0) != 0;
}

void InstallEngine::NotifyErrorPageShown() noexcept {
    if (errorPageShown_) SetEvent(errorPageShown_);
}

void InstallEngine::RememberMsiMessage(const wchar_t* text) noexcept {
    if (!text || !text[0]) return;
    AcquireSRWLockExclusive(&statusLock_);
    msiMessage_ = text;
    ReleaseSRWLockExclusive(&statusLock_);
}

void InstallEngine::SetFailure(const std::wstring& text, unsigned msiCode) noexcept {
    AcquireSRWLockExclusive(&statusLock_);
    failureDetail_ = text;
    error_ = text;
    msiExitCode_ = msiCode;
    ReleaseSRWLockExclusive(&statusLock_);
    InstallLog::Line(text.c_str());
}

std::wstring InstallEngine::StatusLine() const noexcept {
    AcquireSRWLockShared(const_cast<PSRWLOCK>(&statusLock_));
    const std::wstring copy = statusLine_;
    ReleaseSRWLockShared(const_cast<PSRWLOCK>(&statusLock_));
    return copy;
}

bool InstallEngine::IsMarquee() const noexcept {
    AcquireSRWLockShared(const_cast<PSRWLOCK>(&statusLock_));
    const bool copy = marquee_;
    ReleaseSRWLockShared(const_cast<PSRWLOCK>(&statusLock_));
    return copy;
}

void InstallEngine::SetStatus(const std::wstring& line, int percent) noexcept {
    AcquireSRWLockExclusive(&statusLock_);
    if (!line.empty()) statusLine_ = line;
    if (percent >= 0) {
        progressUnits_ = percent;
        marquee_ = false;
    } else {
        marquee_ = true;
    }
    ReleaseSRWLockExclusive(&statusLock_);
    if (status_) status_(line, percent);
}

InstallEngine::HoldKind InstallEngine::CurrentHold() const noexcept {
    AcquireSRWLockShared(const_cast<PSRWLOCK>(&statusLock_));
    const HoldKind copy = holdKind_;
    ReleaseSRWLockShared(const_cast<PSRWLOCK>(&statusLock_));
    return copy;
}

std::wstring InstallEngine::HoldReport() const noexcept {
    AcquireSRWLockShared(const_cast<PSRWLOCK>(&statusLock_));
    const std::wstring copy = holdReport_;
    ReleaseSRWLockShared(const_cast<PSRWLOCK>(&statusLock_));
    return copy;
}

void InstallEngine::SignalHold(HoldAction action) noexcept {
    AcquireSRWLockExclusive(&statusLock_);
    holdAction_ = action;
    holdKind_ = HoldKind::None;
    ReleaseSRWLockExclusive(&statusLock_);
    if (action == HoldAction::Cancel)
        cancelRequested_.store(true, std::memory_order_release);
    if (holdGate_) SetEvent(holdGate_);
}

bool InstallEngine::StopGlassPlatform(GP::PlatformArch arch, const std::wstring& statusLabel,
    const wchar_t* logTag, const std::function<bool(const wchar_t*)>& fail) noexcept {
    SetStatus(statusLabel, -1);
    if (GP::IsGlassPlatformRunning(arch)) {
        const bool signaled = GP::SignalPlatformStop(arch);
        if (arch==GP::PlatformArch::X64) x64StopSignalled_=signaled;
        else x86StopSignalled_=signaled;
        InstallLog::LineFormat(L"%s: SignalPlatformStop %s", logTag, signaled ? L"ok" : L"failed");
    } else {
        InstallLog::LineFormat(L"%s: host already stopped", logTag);
    }

    const ULONGLONG deadline = GetTickCount64() + 45000;
    int emptyResidualStreak = 0;
    while (GetTickCount64() < deadline) {
        if (IsCancelRequested())
            return false;
        if (!GP::IsGlassPlatformRunning(arch)) {
            (void)GP::StopOrphanAutorunTaskInstance(arch);
            if (!GP::RefreshInjectResidualSticky(arch)) {
                if (++emptyResidualStreak >= 6)
                    break;
            } else {
                emptyResidualStreak = 0;
            }
        }
        Sleep(250);
    }
    if (IsCancelRequested())
        return false;
    if (GP::IsGlassPlatformRunning(arch)) {
        InstallLog::LineFormat(L"%s: host still running", logTag);
        return fail(L"GlassPlatform host");
    }
    InstallLog::LineFormat(L"%s: stopped", logTag);
    return true;
}

bool InstallEngine::StopRunningHosts() noexcept {
    const bool ru = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN;
    auto fail = [this, ru](const wchar_t* what) {
        SetFailure((ru ? std::wstring(L"Не удалось дождаться остановки: ")
                       : std::wstring(L"Timed out waiting to stop: ")) + what, 0);
        SetStatus(L"", 0);
        return false;
    };

    if (cleanInstall_) {
        InstallLog::Line(L"Clean install — skip stop and module scan");
        return true;
    }

    hostsStopped_ = true;
    const ArchPresence x64 = ClassifyArch(GP::PlatformArch::X64);
    const ArchPresence x86 = ClassifyArch(GP::PlatformArch::X86);
    const bool openGlass = OpenGlassPresent(openGlassDir_);

    if (x64 == ArchPresence::Running) {
        if (!StopGlassPlatform(GP::PlatformArch::X64, labels_.stop64, L"x64", fail))
            return false;
    } else {
        InstallLog::Line(x64 == ArchPresence::Absent
            ? L"x64: no task and no files"
            : L"x64: host not running — skip stop");
    }
    if (x86 == ArchPresence::Running) {
        if (!StopGlassPlatform(GP::PlatformArch::X86, labels_.stop86, L"x86", fail))
            return false;
    } else {
        InstallLog::Line(x86 == ArchPresence::Absent
            ? L"x86: no task and no files"
            : L"x86: host not running — skip stop");
    }

    if (openGlass) {
        SetStatus(labels_.stopOg, -1);
        InstallLog::Line(L"OpenGlass: TryShutdown");
        ogStopSignalled_=true;
        (void)Shared::OpenGlassHostLaunch::TryShutdown();
        (void)GlassInstallHost::StopOpenGlassHost();
        const ULONGLONG ogStart = GetTickCount64();
        while (GlassInstallUtil::ImageRunning(L"OpenGlassHost.exe") || !ServiceStopped(L"OpenGlassHost")) {
            if (IsCancelRequested())
                return false;
            if (GetTickCount64() - ogStart >= 45000)
                return fail(L"OpenGlassHost");
            Sleep(250);
        }
    } else {
        InstallLog::Line(L"OpenGlass: not installed — skip shutdown");
    }

    InstallLog::Line(L"Hosts stopped");
    return true;
}

bool InstallEngine::ReleaseLockedModules() noexcept {
    using ModuleHolder = GlassInstallUnlock::ModuleHolder;
    auto publish = [&](HoldKind kind, const std::vector<ModuleHolder>& holders) {
        AcquireSRWLockExclusive(&statusLock_);
        if (!cancelRequested_.load(std::memory_order_acquire)) {
            if (holdGate_) ResetEvent(holdGate_);
            holdKind_ = kind;
            holdReport_ = GlassInstallUnlock::FormatHolders(holders);
            holdAction_ = HoldAction::None;
        }
        ReleaseSRWLockExclusive(&statusLock_);
    };

    SetStatus(labels_.unload.empty() ? L"…" : labels_.unload, -1);
    bool nativeFailed = false;
    std::vector<DWORD> settingsClosed;
    auto nudgeSettings = [&](const std::vector<ModuleHolder>& list) {
        for (const auto& holder : list) {
            if (_wcsicmp(holder.exe.c_str(), L"GlassSettings.exe") != 0) continue;
            bool seen = false;
            for (DWORD pid : settingsClosed) {
                if (pid == holder.pid) { seen = true; break; }
            }
            if (seen) continue;
            settingsClosed.push_back(holder.pid);
            InstallLog::LineFormat(L"Unlocker: GlassSettings pid %lu — WM_CLOSE before MSI", holder.pid);
            GlassInstallUnlock::CloseProcessWindows(holder.pid);
        }
    };

    std::vector<ModuleHolder> holders =
        GlassInstallUnlock::CollectHolders(x64Dir_, x86Dir_, scanNativeX64_, nativeFailed);
    nudgeSettings(holders);
    if (!holders.empty() && !IsCancelRequested()) {
        InstallLog::LineFormat(L"Unlocker: %u processes still hold Glass modules after stop",
            static_cast<unsigned>(holders.size()));
        for (const auto& holder : holders) {
            InstallLog::LineFormat(L"  holder %s pid %lu%s: %s",
                holder.exe.c_str(),
                holder.pid,
                GlassInstallUnlock::TargetIsWow64(holder.pid) ? L" wow64" : L"",
                holder.modules.c_str());
        }
        const bool cleared = GlassInstallWatch::WaitUntilClearOrTimeout(
            30000, x64Dir_, x86Dir_, scanNativeX64_, holders, nativeFailed, &cancelRequested_);
        nudgeSettings(holders);
        if (cleared) holders.clear();
    }
    if (IsCancelRequested())
        return false;
    if (nativeFailed) {
        if (forceClean_) {
            InstallLog::Line(L"Clean reinstall: native x64 scan unavailable, purge continues");
        } else {
            SetFailure(labels_.nativeScanFailed.empty()
                ? L"64-bit process scan failed." : labels_.nativeScanFailed, 0);
            return false;
        }
    }
    if (holders.empty()) {
        InstallLog::Line(L"Unlocker: all Glass modules unloaded");
        return true;
    }

    for (;;) {
        if (IsCancelRequested())
            return false;
        InstallLog::Line(L"Unlocker: asking user to force-unload");
        publish(HoldKind::Modules, holders);
        if (IsCancelRequested()) return false;
        WaitForSingleObject(holdGate_, INFINITE);
        if (holdAction_ == HoldAction::Cancel || IsCancelRequested()) {
            quietClose_ = true;
            return false;
        }
        SetStatus(labels_.forceUnload.empty() ?
            (labels_.unload.empty() ? L"…" : labels_.unload) : labels_.forceUnload, -1);
        for (const auto& holder : holders)
            (void)GlassInstallUnlock::ForceUnloadHolder(holder, x64Dir_, x86Dir_);
        const ULONGLONG retryStart = GetTickCount64();
        do {
            if (IsCancelRequested()) return false;
            Sleep(1000);
            holders = GlassInstallUnlock::CollectHolders(x64Dir_, x86Dir_, scanNativeX64_, nativeFailed);
        } while (!holders.empty() && GetTickCount64() - retryStart < 8000);
        if (holders.empty()) {
            InstallLog::Line(L"Unlocker: force unload cleared modules");
            return true;
        }

        InstallLog::Line(L"Unlocker: modules remain after force unload");
        publish(HoldKind::Processes, holders);
        if (IsCancelRequested()) return false;
        WaitForSingleObject(holdGate_, INFINITE);
        if (holdAction_ == HoldAction::Cancel || IsCancelRequested()) {
            quietClose_ = true;
            return false;
        }
        SetStatus(labels_.unlockProcesses.empty() ?
            (labels_.unload.empty() ? L"…" : labels_.unload) : labels_.unlockProcesses, -1);
        for (const auto& holder : holders) {
            if (IsProtectedProcessName(holder.exe.c_str())) {
                InstallLog::LineFormat(L"Unlocker: skip protected %s", holder.exe.c_str());
                continue;
            }
            GlassInstallUnlock::UnlockProcess(holder.pid, holder.exe.c_str());
        }
        holders = GlassInstallUnlock::CollectHolders(x64Dir_, x86Dir_, scanNativeX64_, nativeFailed);
        if (holders.empty()) {
            InstallLog::Line(L"Unlocker: processes released modules");
            return true;
        }
    }
}

bool InstallEngine::EnsureTempWorkspace() noexcept {
    if (!tempDir_.empty()) return true;
    if (!GlassInstallUtil::CreateSecureTempDirectory(L"GlassSetup",tempDir_)) return false;
    InstallLog::LineFormat(L"Temp workspace: %s", tempDir_.c_str());
    return true;
}

bool InstallEngine::ExtractPayload(HINSTANCE instance) noexcept {
    if (!EnsureTempWorkspace()) {
        SetFailure(L"Не удалось создать каталог во временной папке.", 0);
        return false;
    }

    SetStatus(labels_.extract, 0);
#ifdef GLASS_EMBED_MSI
    const bool os64 = GlassInstallUtil::IsOs64Bit();
    const ResourceBlob blobs[] = {
        {IDR_MSI_X64,       L"Glass-x64.msi",         os64 && selection_.platform64},
        {IDR_MSI_X86,       L"Glass-x86.msi",         selection_.platform32},
        {IDR_MSI_GS_X64,    L"GlassSettings-x64.msi", os64 && selection_.glassSettings},
        {IDR_MSI_GS_X86,    L"GlassSettings-x86.msi", !os64 && selection_.glassSettings},
        {IDR_MSI_OPENGLASS, L"OpenGlass-x64.msi",     os64 && selection_.openGlass},
    };
    int neededTotal = 0;
    for (const auto& b : blobs) if (b.needed) ++neededTotal;
    if (neededTotal < 1) neededTotal = 1;

    int step = 0;
    for (const auto& blob : blobs) {
        if (!blob.needed) continue;
        if (IsCancelRequested()) return false;
        wchar_t dest[MAX_PATH]{};
        if (!PathCombineW(dest, tempDir_.c_str(), blob.fileName)) return false;
        if (!GlassInstallUtil::ExtractResourceToFile(instance, blob.id, RT_RCDATA, dest, &cancelRequested_)) {
            if (IsCancelRequested()) return false;
            std::wstring msg = L"Missing embedded MSI: ";
            msg += blob.fileName;
            SetFailure(msg, 0);
            return false;
        }
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (GetFileAttributesExW(dest, GetFileExInfoStandard, &info)) {
            const ULONGLONG size = (static_cast<ULONGLONG>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
            InstallLog::LineFormat(L"Extracted %s (%llu bytes) -> %s", blob.fileName, size, dest);
        }
        ++step;
        SetStatus(labels_.extract, (step * 400) / neededTotal);
    }
    SetStatus(labels_.extract, 400);
    return true;
#else
    (void)instance;
    SetFailure(L"Release build required (embedded MSI missing).", 0);
    return false;
#endif
}

bool InstallEngine::RunOneMsi(const std::wstring& msiPath, const std::wstring& installDir,
    const std::wstring& label, int progressBase, int progressSpan) noexcept {
    if (IsCancelRequested()) return false;
    if (!PathFileExistsW(msiPath.c_str())) {
        SetFailure(std::wstring(L"MSI not found: ") + msiPath, 0);
        return false;
    }
    SetStatus(label, progressBase);
    AcquireSRWLockExclusive(&statusLock_);
    msiMessage_.clear();
    ReleaseSRWLockExclusive(&statusLock_);

    const std::wstring msiLog = InstallLog::NextMsiVerboseLogPath(label.c_str());
    if (!msiLog.empty()) MsiEnableLogW(INSTALLLOGMODE_VERBOSE, msiLog.c_str(), INSTALLLOGATTRIBUTES_FLUSHEACHLINE);
    const std::wstring productCode = GlassInstallMsi::ResolveProductCode(msiPath);
    GlassInstallMsi::ProgressSink sink;
    sink.label = label; sink.base = progressBase; sink.span = progressSpan;
    sink.cancelRequested = &cancelRequested_;
    sink.onError = [this](const std::wstring& text) { RememberMsiMessage(text.c_str()); };
    sink.onProgress = [this](const std::wstring& lbl, int units) { SetStatus(lbl, units); };
    if (!productCode.empty() && GlassInstallMsi::IsProductInstalled(productCode.c_str())) {
        if (!ProductWasSnapshotted(rollbackSession_, productCode.c_str())) {
            MsiEnableLogW(0,nullptr,0);
            const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
            SetFailure(ru ? L"Продукт появился после создания резервной копии. Повторите установку."
                          : L"The product appeared after the snapshot. Run Setup again.", 0);
            return false;
        }
        InstallLog::LineFormat(L"Existing product %s detected — uninstall before upgrade", productCode.c_str());
        GlassInstallRollback::NoteModified(rollbackSession_,productCode.c_str());
        UINT removeCode=ERROR_SUCCESS;
        if (!GlassInstallMsi::UninstallProduct(productCode.c_str(), &sink, &removeCode)) {
            MsiEnableLogW(0,nullptr,0);
            if (removeCode == ERROR_INSTALL_USEREXIT) cancelRequested_.store(true,std::memory_order_release);
            if (!IsCancelRequested()) {
                const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
                SetFailure(ru ? L"Не удалось удалить предыдущую версию продукта." : L"Could not remove the previous MSI product.", removeCode);
            }
            return false;
        }
    }
    if (IsCancelRequested()) {
        MsiEnableLogW(0, nullptr, 0);
        return false;
    }

    const std::wstring props = GlassInstallMsi::BuildInstallPropertyString(installDir, false);
    InstallLog::LineFormat(L"MsiInstallProduct %s", msiPath.c_str());
    InstallLog::LineFormat(L"  %s", props.c_str());
    InstallLog::LineFormat(L"MSI verbose log: %s", msiLog.c_str());

    UINT exitCode = GlassInstallMsi::InstallProduct(msiPath, props, &sink);
    if (exitCode == ERROR_PRODUCT_VERSION && !IsCancelRequested()) {
        InstallLog::Line(L"MSI 1638 — forced uninstall and retry");
        MsiEnableLogW(INSTALLLOGMODE_VERBOSE,msiLog.c_str(),INSTALLLOGATTRIBUTES_FLUSHEACHLINE);
        if (!productCode.empty() && GlassInstallMsi::IsProductInstalled(productCode.c_str())) {
            if (!ProductWasSnapshotted(rollbackSession_,productCode.c_str())) {
                MsiEnableLogW(0,nullptr,0);
                const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
                SetFailure(ru ? L"Продукт появился после создания резервной копии. Повторите установку."
                              : L"The product appeared after the snapshot. Run Setup again.", 0);
                return false;
            }
            GlassInstallRollback::NoteModified(rollbackSession_,productCode.c_str());
            UINT removeCode=ERROR_SUCCESS;
            if (!GlassInstallMsi::UninstallProduct(productCode.c_str(), &sink, &removeCode)) {
                MsiEnableLogW(0,nullptr,0);
                if (removeCode == ERROR_INSTALL_USEREXIT) cancelRequested_.store(true,std::memory_order_release);
                if (!IsCancelRequested()) SetFailure(L"Could not remove the previous MSI product",removeCode);
                return false;
            }
        }
        if (IsCancelRequested()) { MsiEnableLogW(0,nullptr,0); return false; }
        exitCode = GlassInstallMsi::InstallProduct(msiPath, props, &sink);
    }
    MsiEnableLogW(0, nullptr, 0);
    InstallLog::LineFormat(L"MsiInstallProduct exit code %u (%s)", exitCode, label.c_str());

    if (GlassInstallMsi::MsiSuccess(exitCode) && !productCode.empty())
        GlassInstallRollback::NoteInstalled(rollbackSession_,productCode.c_str());
    if (exitCode == ERROR_INSTALL_USEREXIT || IsCancelRequested()) {
        cancelRequested_.store(true, std::memory_order_release);
        return false;
    }
    if (!GlassInstallMsi::MsiSuccess(exitCode)) {
        std::wstring detail = MsiMessage();
        if (detail.empty()) {
            detail = L"MSI exit ";
            detail += std::to_wstring(exitCode);
            detail += L": ";
            detail += label;
        }
        SetFailure(detail, exitCode);
        InstallLog::LineFormat(L"See MSI log: %s", msiLog.c_str());
        return false;
    }
    if (!productCode.empty()) {
        GlassInstallMsi::PatchArpUninstallString(productCode, installDir);
    }
    SetStatus(label, progressBase + progressSpan);
    return true;
}

bool InstallEngine::RunMsiSequence() noexcept {
    const bool is64 = GlassInstallUtil::IsOs64Bit();
    const bool ru = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN;
    struct Step {
        std::wstring msiFile;
        std::wstring dir;
        std::wstring label;
        const wchar_t* removeCode = nullptr;
    };
    std::vector<Step> steps;
    const auto addRemoval = [&](const wchar_t* code, const wchar_t* name) {
        if (!GlassInstallMsi::IsProductInstalled(code)) return true;
        // A product that appeared after the snapshot has no recoverable backup.
        if (!ProductWasSnapshotted(rollbackSession_, code)) {
            SetFailure(ru ? L"Состав установленных продуктов изменился после создания копии. Повторите установку."
                          : L"Installed products changed after the snapshot. Run Setup again.", 0);
            return false;
        }
        const std::wstring prefix = labels_.removing.empty() ? (ru ? L"Удаление: " : L"Removing: ") : labels_.removing;
        steps.push_back({L"", L"", prefix + name, code});
        return true;
    };
    // GlassSettings is one UI choice but can have either or both MSI codes.
    if (!selection_.glassSettings &&
        (!addRemoval(GlassInstallMsi::kProductSettingsX64, L"GlassSettings (x64)") ||
         !addRemoval(GlassInstallMsi::kProductSettingsX86, L"GlassSettings (x86)"))) return false;
    if (is64 && !selection_.openGlass &&
        (!addRemoval(GlassInstallMsi::kProductOpenGlass, L"OpenGlass") ||
         !addRemoval(GlassInstallMsi::kLegacyOpenGlass, L"OpenGlass"))) return false;
    if (!selection_.platform32 &&
        (!addRemoval(GlassInstallMsi::kProductPlatformX86, L"Glass Platform (x86)") ||
         !addRemoval(GlassInstallMsi::kLegacyPlatformX86, L"Glass Platform (x86)"))) return false;

    if (is64 && selection_.platform64)
        steps.push_back({L"Glass-x64.msi", x64Dir_, labels_.platform64});
    if (selection_.platform32)
        steps.push_back({L"Glass-x86.msi", x86Dir_, labels_.platform32});
    if (selection_.glassSettings) {
        if (is64) steps.push_back({L"GlassSettings-x64.msi", x64Dir_, labels_.glassSettings64});
        else steps.push_back({L"GlassSettings-x86.msi", x86Dir_, labels_.glassSettings86});
    }
    if (is64 && selection_.openGlass)
        steps.push_back({L"OpenGlass-x64.msi", openGlassDir_, labels_.openGlass});

    constexpr int kProgressStart = 800;
    constexpr int kProgressEnd = 9000;
    const int span = steps.empty() ? 0 : (kProgressEnd - kProgressStart) / static_cast<int>(steps.size());
    int base = kProgressStart;
    for (const auto& step : steps) {
        if (IsCancelRequested()) return false;
        if (step.removeCode) {
            // Note BEFORE calling MSI: cancellation or an error can still have
            // partially changed its registration and must be recoverable.
            GlassInstallRollback::NoteModified(rollbackSession_, step.removeCode);
            SetStatus(step.label, base);
            GlassInstallMsi::ProgressSink sink;
            sink.label=step.label; sink.base=base; sink.span=span; sink.cancelRequested=&cancelRequested_;
            sink.onError=[this](const std::wstring& text) { RememberMsiMessage(text.c_str()); };
            sink.onProgress=[this](const std::wstring& text, int units) { SetStatus(text, units); };
            const std::wstring log=InstallLog::NextMsiVerboseLogPath(step.label.c_str());
            if (!log.empty()) MsiEnableLogW(INSTALLLOGMODE_VERBOSE, log.c_str(), INSTALLLOGATTRIBUTES_FLUSHEACHLINE);
            UINT exitCode=ERROR_SUCCESS;
            const bool removed=GlassInstallMsi::UninstallProduct(step.removeCode, &sink, &exitCode);
            MsiEnableLogW(0, nullptr, 0);
            if (IsCancelRequested() || exitCode==ERROR_INSTALL_USEREXIT) {
                cancelRequested_.store(true, std::memory_order_release);
                return false;
            }
            if (!removed || GlassInstallMsi::IsProductInstalled(step.removeCode)) {
                const std::wstring detail = (ru ? L"Не удалось удалить продукт: " : L"Could not remove product: ") + step.label;
                SetFailure(detail, exitCode);
                InstallLog::LineFormat(L"Remove MSI failed %s (%u); log %s", step.removeCode, exitCode, log.c_str());
                return false;
            }
            base += span;
            SetStatus(step.label, base);
            continue;
        }

        if (step.msiFile == L"Glass-x64.msi") RemoveEmptyChildDir(x64Dir_, L"x64");
        else if (step.msiFile == L"Glass-x86.msi") RemoveEmptyChildDir(x86Dir_, L"x86");

        // WiX MajorUpgrade may remove legacy product codes during this transaction.
        const wchar_t* legacy=step.msiFile==L"Glass-x64.msi"?GlassInstallMsi::kLegacyPlatformX64:
            step.msiFile==L"Glass-x86.msi"?GlassInstallMsi::kLegacyPlatformX86:
            step.msiFile==L"OpenGlass-x64.msi"?GlassInstallMsi::kLegacyOpenGlass:nullptr;
        if (legacy && GlassInstallMsi::IsProductInstalled(legacy)) {
            if (!ProductWasSnapshotted(rollbackSession_, legacy)) {
                SetFailure(ru ? L"Установленный продукт изменился после создания копии." : L"An installed product changed after the snapshot.", 0);
                return false;
            }
            GlassInstallRollback::NoteModified(rollbackSession_,legacy);
        }
        wchar_t msiPath[MAX_PATH]{};
        if (!PathCombineW(msiPath, tempDir_.c_str(), step.msiFile.c_str())) return false;
        if (!RunOneMsi(msiPath, step.dir, step.label, base, span)) return false;
        base += span;

        if (step.msiFile == L"Glass-x64.msi") GlassInstallUtil::WritePlatformInstallPath(true, x64Dir_);
        else if (step.msiFile == L"Glass-x86.msi") GlassInstallUtil::WritePlatformInstallPath(false, x86Dir_);
        else if (step.msiFile == L"OpenGlass-x64.msi") GlassInstallUtil::WriteOpenGlassInstallPath(x64Dir_, openGlassDir_);
        else if (step.msiFile == L"GlassSettings-x64.msi" || step.msiFile == L"GlassSettings-x86.msi") {
            const std::wstring gsDir = GlassInstallUtil::TrimTrailingSlashes(step.dir) + L"\\GlassSettings";
            (void)GlassInstallUtil::CreateStartMenuShortcut(gsDir + L"\\GlassSettings.exe", gsDir);
        }
    }

    // These exact values were captured in the rollback snapshot. Keep other
    // registry settings and user files intact, and only prune empty folders.
    if ((remove_.platform32 && !GlassInstallUtil::ClearPlatformInstallPath(false)) ||
        (remove_.openGlass && !GlassInstallUtil::ClearOpenGlassInstallPath())) {
        SetFailure(ru ? L"Не удалось удалить пути снятых продуктов из реестра."
                      : L"Could not clear the removed products' registry paths.", GetLastError());
        return false;
    }
    if (remove_.platform32 && !x86Dir_.empty()) (void)RemoveDirectoryW(x86Dir_.c_str());
    if (remove_.openGlass && !openGlassDir_.empty()) (void)RemoveDirectoryW(openGlassDir_.c_str());
    InstallLog::Line(L"InstallEngine: desired MSI selection applied");
    return true;
}

bool InstallEngine::LaunchAndVerifySelectedProducts() noexcept {
    const bool is64 = GlassInstallUtil::IsOs64Bit();

    SetStatus(labels_.finalize, 9200);
    auto configureAndStartArch = [this](
        GP::PlatformArch arch,
        const std::wstring& dir,
        bool taskExistedBefore,
        bool taskEnabledBefore,
        bool wasRunningBefore,
        const std::wstring& label,
        int pct) -> bool {
        if (!taskExistedBefore) {
            if (!GlassInstallHost::EnsureGlassAutorunTask(arch, dir, true, true)) {
                InstallLog::LineFormat(L"Autostart: failed to register task for arch %d", static_cast<int>(arch));
                return false;
            }
            if (arch==GP::PlatformArch::X64) x64TaskCreatedBySetup_=true;
            else x86TaskCreatedBySetup_=true;
        }
        // Do not re-enable a user-disabled task, even if its host was running before setup.
        const bool shouldStart = taskExistedBefore ? (wasRunningBefore && taskEnabledBefore) : true;
        if (!shouldStart) {
            InstallLog::LineFormat(L"Autostart: task disabled by user prior to upgrade, skip start arch %d",
                static_cast<int>(arch));
            return true;
        }
        SetStatus(label, pct);
        if (!GP::IsGlassPlatformRunning(arch)) {
            InstallLog::LineFormat(L"Autostart: StartPlatform arch %d", static_cast<int>(arch));
            GP::StartPlatform(arch);
            const ULONGLONG start = GetTickCount64();
            while (!GP::IsGlassPlatformRunning(arch)) {
                if (GetTickCount64() - start >= GP::kDefaultPlatformShutdownWaitMs)
                    return false;
                Sleep(200);
            }
        }
        InstallLog::LineFormat(L"Autostart: platform running arch %d", static_cast<int>(arch));
        return true;
    };

    if (is64 && selection_.platform64) {
        if (!configureAndStartArch(GP::PlatformArch::X64, x64Dir_,
                prior_.x64TaskExisted, prior_.x64TaskEnabled, prior_.x64WasRunning,
                labels_.start64, 9500)) {
            SetFailure(labels_.platformDidNotStart.empty()
                ? L"Platform did not start." : labels_.platformDidNotStart, 0);
            return false;
        }
    }

    if (selection_.platform32) {
        if (!configureAndStartArch(GP::PlatformArch::X86, x86Dir_,
                prior_.x86TaskExisted, prior_.x86TaskEnabled, prior_.x86WasRunning,
                labels_.start86, 9750)) {
            SetFailure(labels_.platformDidNotStart.empty()
                ? L"Platform did not start." : labels_.platformDidNotStart, 0);
            return false;
        }
    }

    if (is64 && selection_.openGlass) {
        SetStatus(labels_.startOg, 9900);
        if (!prior_.ogServiceExisted && !prior_.ogLegacyTaskExisted) {
            if (!GlassInstallHost::EnsureOpenGlassService(openGlassDir_, SERVICE_AUTO_START)) {
                SetFailure(L"Could not register OpenGlassHost service",0);
                return false;
            }
            ogServiceCreatedBySetup_=true;
        }
        const bool shouldStartOg = (prior_.ogServiceExisted || prior_.ogLegacyTaskExisted)
            ? prior_.ogWasRunning && prior_.ogServiceStartType != SERVICE_DISABLED && prior_.ogLegacyTaskEnabled : true;
        if (shouldStartOg) {
            InstallLog::Line(L"Autostart: OpenGlass startup");
            const bool startupAccepted=Shared::OpenGlassHostLaunch::TryStartup();
            if (!startupAccepted) {
                SetFailure(L"OpenGlassHost could not be started.",0);
                return false;
            }
            const ULONGLONG start = GetTickCount64();
            while (startupAccepted && !GlassInstallUtil::ImageRunning(L"OpenGlassHost.exe")) {
                if (GetTickCount64() - start >= 15000)
                    break;
                Sleep(250);
            }
            const bool running=GlassInstallUtil::ImageRunning(L"OpenGlassHost.exe");
            InstallLog::LineFormat(L"Autostart: OpenGlass running=%d",running?1:0);
            if (!running) {
                SetFailure(L"OpenGlassHost did not start within the timeout.",0);
                return false;
            }
        }
    }

    if (!RestorePriorHosts(true)) {
        SetFailure(L"Could not restart a previously running component.",0);
        return false;
    }
    // Register last. Existing shell registration is never replaced by this setup;
    // do not provision or trust developer certificates on a user's machine.
    if (cleanInstall_ && !rollbackSession_.hadPriorInstall &&
        !GlassInstallUtil::InvokeShellExtRegistration(true,x64Dir_,x86Dir_,&shellRegisteredBySetup_)) {
        SetFailure(labels_.shellRegisterFailed.empty()?L"Could not register the Windows shell extension.":labels_.shellRegisterFailed,0);
        return false;
    }
    SetStatus(labels_.finalize, 9950);
    return true;
}

bool InstallEngine::DetermineCleanInstall() noexcept {
    const ArchPresence x64 = ClassifyArch(GP::PlatformArch::X64);
    const ArchPresence x86 = ClassifyArch(GP::PlatformArch::X86);
    const bool gsInstalled = GlassInstallMsi::IsProductInstalled(GlassInstallMsi::kProductSettingsX64)
        || GlassInstallMsi::IsProductInstalled(GlassInstallMsi::kProductSettingsX86);
    const bool openGlass = OpenGlassPresent(openGlassDir_) || Shared::OpenGlassHostState::LegacyHostTaskExists();
    cleanInstall_ = x64 == ArchPresence::Absent && x86 == ArchPresence::Absent
        && !gsInstalled && !openGlass && !prior_.x64TaskExisted && !prior_.x86TaskExisted
        && !prior_.ogServiceExisted && !prior_.ogLegacyTaskExisted && !prior_.ogWasRunning;
    scanNativeX64_ = GlassInstallUtil::IsOs64Bit() && (x64 != ArchPresence::Absent || gsInstalled);
    InstallLog::LineFormat(L"Clean install=%d scanNativeX64=%d", cleanInstall_ ? 1 : 0, scanNativeX64_ ? 1 : 0);
    return true;
}

bool InstallEngine::CreateRollbackSnapshot() noexcept {
    SetStatus(labels_.backup.empty() ? L"…" : labels_.backup, 450);
    const bool ok = GlassInstallRollback::CreateSnapshot(
        rollbackSession_, x64Dir_, x86Dir_, openGlassDir_, selection_.openGlass,
        [this](int units) {
            SetStatus(labels_.backup, 450 + (units * 350) / 10000);
        }, &cancelRequested_);
    if (IsCancelRequested())
        return false;
    if (!ok) {
        if (rollbackSession_.copyError == ERROR_DISK_FULL) {
            SetFailure(labels_.diskFull.empty()
                ? L"There is not enough disk space to install. Free some space and run Setup again."
                : labels_.diskFull, ERROR_DISK_FULL);
        } else {
            SetFailure(labels_.snapshotFailed.empty()
                ? L"Could not save a copy of the current installation. No files were changed."
                : labels_.snapshotFailed, rollbackSession_.copyError);
        }
    }
    SetStatus(labels_.backup, 800);
    return ok;
}

bool InstallEngine::PrepareProductRemoval() noexcept {
    ComponentSelection installed{false, false, false, false};
    installed.platform64 = ProductWasSnapshotted(rollbackSession_, GlassInstallMsi::kProductPlatformX64) ||
        ProductWasSnapshotted(rollbackSession_, GlassInstallMsi::kLegacyPlatformX64);
    installed.platform32 = ProductWasSnapshotted(rollbackSession_, GlassInstallMsi::kProductPlatformX86) ||
        ProductWasSnapshotted(rollbackSession_, GlassInstallMsi::kLegacyPlatformX86);
    installed.glassSettings = ProductWasSnapshotted(rollbackSession_, GlassInstallMsi::kProductSettingsX64) ||
        ProductWasSnapshotted(rollbackSession_, GlassInstallMsi::kProductSettingsX86);
    installed.openGlass = ProductWasSnapshotted(rollbackSession_, GlassInstallMsi::kProductOpenGlass) ||
        ProductWasSnapshotted(rollbackSession_, GlassInstallMsi::kLegacyOpenGlass);
    remove_ = GlassProducts::RemovalForInstall(selection_, installed, GlassInstallUtil::IsOs64Bit());
    if (!remove_.platform32 && !remove_.openGlass && !remove_.glassSettings) return true;
    const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
    std::wstring error;
    if (!GlassInstallRemoval::Prepare(removalSession_, remove_.platform32, remove_.openGlass, remove_.glassSettings,
            x64Dir_, x86Dir_, openGlassDir_, rollbackSession_.workspaceDir, &cancelRequested_, ru, error)) {
        if (!IsCancelRequested()) SetFailure(error.empty()
            ? (ru ? L"Не удалось подготовить безопасное удаление компонентов." : L"Could not prepare safe component removal.")
            : error, 0);
        return false;
    }
    InstallLog::LineFormat(L"Removal plan: x86=%d OpenGlass=%d Settings=%d (task backups=%u)",
        remove_.platform32 ? 1 : 0, remove_.openGlass ? 1 : 0, remove_.glassSettings ? 1 : 0,
        static_cast<unsigned>(removalSession_.tasks.size()));
    return true;
}

bool InstallEngine::PerformRollback() noexcept {
    // The stop/uninstall stages have no reliable percentage. Publish marquee
    // before exposing the reverse flag so the UI never uses stale MSI progress.
    SetStatus(labels_.rollback.empty() ? L"…" : labels_.rollback, -1);
    progressReverse_.store(true,std::memory_order_release);
    (void)GlassInstallHost::StopAllForUninstall(x64Dir_, x86Dir_, true, true, true);
    bool lifecycleOk=true;
    if (shellRegisteredBySetup_ && !GlassInstallUtil::InvokeShellExtRegistration(false,x64Dir_,x86Dir_)) lifecycleOk=false;
    if ((x64TaskCreatedBySetup_ || x86TaskCreatedBySetup_) &&
        !GlassInstallHost::RemovePlatformAutorunTasks(x64TaskCreatedBySetup_,x86TaskCreatedBySetup_)) lifecycleOk=false;
    if (ogServiceCreatedBySetup_ && !GlassInstallHost::RemoveOpenGlassServiceAndTasks(false)) lifecycleOk=false;
    if (!prior_.shortcutExisted) GlassInstallUtil::DeleteStartMenuShortcut();
    const bool filesOk = GlassInstallRollback::ExecuteRollback(
        rollbackSession_, [this](int units) {
            SetStatus(labels_.rollback, WizardProgress::RollbackRemaining(units));
        });
    const bool ok=lifecycleOk && filesOk;
    std::wstring note;
    if (!ok)
        note = labels_.rollbackFailed;
    else if (rollbackSession_.hadPriorInstall)
        note = labels_.rollbackRestored;
    else if (purgeCompleted_ || !rollbackSession_.installedThisSession.empty())
        note = labels_.rollbackRemoved;
    AcquireSRWLockExclusive(&statusLock_);
    rollbackNote_ = note;
    if (failureDetail_.empty())
        failureDetail_ = error_;
    error_ = failureDetail_;
    if (!note.empty()) {
        if (!error_.empty()) error_ += L"\r\n\r\n";
        error_ += note;
    }
    ReleaseSRWLockExclusive(&statusLock_);
    if (!note.empty()) InstallLog::Line(note.c_str());
    SetStatus(labels_.rollback, 0);
    return ok;
}

bool InstallEngine::UndoCleanInstall() noexcept {
    // Individual MSI operations do not expose reliable continuous progress.
    SetStatus(labels_.rollback.empty() ? L"…" : labels_.rollback, -1);
    progressReverse_.store(true,std::memory_order_release);
    InstallLog::Line(L"Clean install failed or cancelled; removing partial install");
    (void)GlassInstallHost::StopAllForUninstall(x64Dir_, x86Dir_, true, true, true);
    bool ok = !shellRegisteredBySetup_ || GlassInstallUtil::InvokeShellExtRegistration(false,x64Dir_,x86Dir_);
    const size_t productCount=rollbackSession_.installedThisSession.size();
    size_t productsProcessed=0;
    for (auto it = rollbackSession_.installedThisSession.rbegin();
         it != rollbackSession_.installedThisSession.rend(); ++it) {
        if (!GlassInstallMsi::UninstallProduct(it->c_str())) ok = false;
        ++productsProcessed;
        // Leave room for registry, services and tasks; final success is reported
        // separately after all cleanup and host restoration have finished.
        if (productCount) SetStatus(labels_.rollback,WizardProgress::kMaxUnits-
            static_cast<int>(productsProcessed*8000/productCount));
    }
    // MSI removes its own files. Only empty directories may be removed; never delete user content.
    if (!openGlassDir_.empty()) RemoveDirectoryW(openGlassDir_.c_str());
    if (!x64Dir_.empty()) RemoveDirectoryW(x64Dir_.c_str());
    if (!x86Dir_.empty()) RemoveDirectoryW(x86Dir_.c_str());
    if (ogServiceCreatedBySetup_ && !GlassInstallHost::RemoveOpenGlassServiceAndTasks(false)) ok=false;
    if ((x64TaskCreatedBySetup_ || x86TaskCreatedBySetup_) &&
        !GlassInstallHost::RemovePlatformAutorunTasks(x64TaskCreatedBySetup_,x86TaskCreatedBySetup_)) ok=false;
    if (!prior_.shortcutExisted) GlassInstallUtil::DeleteStartMenuShortcut();
    if (!GlassInstallRollback::RestoreRegistryValues(rollbackSession_)) ok=false;

    std::wstring note = ok ? labels_.rollbackRemoved : labels_.rollbackFailed;
    AcquireSRWLockExclusive(&statusLock_);
    rollbackNote_ = note;
    if (failureDetail_.empty())
        failureDetail_ = error_;
    error_ = failureDetail_;
    if (!note.empty()) {
        if (!error_.empty()) error_ += L"\r\n\r\n";
        error_ += note;
    }
    ReleaseSRWLockExclusive(&statusLock_);
    if (!note.empty()) InstallLog::Line(note.c_str());
    SetStatus(labels_.rollback, 0);
    return ok;
}

bool InstallEngine::PurgeRemnants() noexcept {
    SetStatus(labels_.purge.empty() ? L"…" : labels_.purge, 300);
    const wchar_t* codes[] = {
        GlassInstallMsi::kProductSettingsX64,
        GlassInstallMsi::kProductSettingsX86,
        GlassInstallMsi::kProductPlatformX64,
        GlassInstallMsi::kProductPlatformX86,
        GlassInstallMsi::kProductOpenGlass,
        L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5061}",
        L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5062}",
        L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5063}",
    };
    for (const wchar_t* code : codes) {
        if (!GlassInstallMsi::IsProductInstalled(code)) continue;
        if (IsCancelRequested()) return false;
        if (!ProductWasSnapshotted(rollbackSession_,code)) {
            const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
            SetFailure(ru ? L"Состав установленных продуктов изменился после создания копии. Повторите установку."
                          : L"Installed products changed after the snapshot. Run Setup again.", 0);
            return false;
        }
        InstallLog::LineFormat(L"Purge uninstall %s", code);
        GlassInstallRollback::NoteModified(rollbackSession_,code);
        GlassInstallMsi::ProgressSink sink;
        sink.label=labels_.purge; sink.base=300; sink.span=100;
        sink.cancelRequested=&cancelRequested_;
        sink.onError=[this](const std::wstring& text) { RememberMsiMessage(text.c_str()); };
        UINT removeCode=ERROR_SUCCESS;
        if (!GlassInstallMsi::UninstallProduct(code, &sink, &removeCode)) {
            if (removeCode == ERROR_INSTALL_USEREXIT) cancelRequested_.store(true,std::memory_order_release);
            if (!IsCancelRequested()) {
                const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
                SetFailure(ru ? L"Не удалось удалить прежний MSI-пакет." : L"Could not remove an older MSI product.", removeCode);
            }
            return false;
        }
        if (IsCancelRequested()) return false;
    }
    // MSI owns its installed files. Never recursively delete an installation
    // directory: it can contain unrelated or user-created files. Keep tasks,
    // service and shell registration intact until the reversible commit phase.
    const bool is64 = GlassInstallUtil::IsOs64Bit();
    if ((is64 && !GlassInstallUtil::ClearPlatformInstallPath(true)) ||
        !GlassInstallUtil::ClearPlatformInstallPath(false) ||
        (is64 && !GlassInstallUtil::ClearOpenGlassInstallPath())) {
        SetFailure(labels_.purgeFailed.empty() ? L"Could not clear installation paths." : labels_.purgeFailed, GetLastError());
        return false;
    }
    purgeCompleted_ = true;
    InstallLog::Line(L"Purge MSI registrations completed; user files retained");
    return true;
}
