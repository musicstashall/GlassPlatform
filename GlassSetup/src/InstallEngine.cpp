#include "InstallSupport/GlassMaintenanceCache.h"
#include "InstallEngine.h"
#include "InstallLog.h"
#include "WizardProgress.h"
#include "InstallSupport/GlassInstallHost.h"
#include "InstallSupport/GlassInstallMsi.h"
#include "InstallSupport/GlassInstallUtil.h"
#include "InstallSupport/GlassInstallUnlock.h"
#include "InstallSupport/GlassUserDefaults.h"
#include "InstallSupport/GlassInstallWatch.h"
#include "../res/resource.h"
#include <GlassPlatformTask.h>
#include <InjectModuleScan.h>
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

// GP::IsGlassPlatformRunning treats all OpenMutex failures as "stopped".
// Setup must distinguish a missing mutex from access or other errors before
// modifying MSI, and keep checking until the named mutex really disappears.
bool QueryPlatformMutex(GP::PlatformArch arch, bool& running) noexcept {
    running = false;
    HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, GP::InstanceMutexName(arch));
    if (mutex) {
        CloseHandle(mutex);
        running = true;
        return true;
    }
    return GetLastError() == ERROR_FILE_NOT_FOUND;
}

ArchPresence ClassifyArch(GP::PlatformArch arch, const std::wstring& fixedRoot) noexcept {
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
    if (CompatDllExists(fixedRoot))
        return ArchPresence::Files;
    return ArchPresence::Absent;
}

bool IsProtectedProcessName(const wchar_t* exe) noexcept {
    if (!exe || !exe[0]) return true;
    const wchar_t* blocked[] = {
        L"System", L"Registry", L"smss.exe", L"csrss.exe", L"wininit.exe",
        L"services.exe", L"lsass.exe", L"winlogon.exe", L"dwm.exe",
        L"fontdrvhost.exe", L"Memory Compression", L"Secure System",
        L"GlassSetup.exe", L"unins000.exe",
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
        !rollingBack_.load() && !failureFinalizing_.load() && !cancelRequested_.load() && decisionGate_;
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
    const bool accepted=started_ && !finished_.load() && !launchingPhase_.load() &&
        !rollingBack_.load() && !failureFinalizing_.load();
    if (accepted) {
        cancelRequested_.store(true,std::memory_order_release);
        holdAction_=HoldAction::Cancel;
        holdKind_=HoldKind::None;
        unlockActivity_.End();
    }
    ReleaseSRWLockExclusive(&statusLock_);
    if (!accepted) return;
    InstallLog::Line(L"InstallEngine: user requested cancel");
    if (holdGate_) SetEvent(holdGate_);
    if (decisionGate_) SetEvent(decisionGate_);
    if (errorPageShown_) SetEvent(errorPageShown_);
}

bool InstallEngine::ChooseCancellationOrFailure(bool failed) noexcept {
    // The operation can fail while the confirmation page is open. Let the
    // user decide first; claim the failure under the same lock used by Pause
    // and RequestCancel so an accepted cancel cannot become an error page.
    for (;;) {
        if (decisionGate_) WaitForSingleObject(decisionGate_, INFINITE);
        AcquireSRWLockExclusive(&statusLock_);
        const bool ready = !decisionGate_ || WaitForSingleObject(decisionGate_, 0) == WAIT_OBJECT_0;
        if (ready) {
            const bool cancelled = cancelRequested_.load(std::memory_order_acquire) ||
                                   quietClose_.load(std::memory_order_acquire);
            if (failed && !cancelled) failureFinalizing_.store(true, std::memory_order_release);
            ReleaseSRWLockExclusive(&statusLock_);
            return cancelled;
        }
        ReleaseSRWLockExclusive(&statusLock_);
    }
}

bool InstallEngine::CapturePriorHostState() noexcept {
    prior_ = {};
    GP::InvalidateGlassAutorunTaskCache();
    prior_.x64TaskExisted = GP::GlassAutorunTaskExists(GP::PlatformArch::X64);
    prior_.x64TaskEnabled = GP::QueryGlassAutorunUiState(GP::PlatformArch::X64) != GP::AutorunUiState::Disabled;
    prior_.x86TaskExisted = GP::GlassAutorunTaskExists(GP::PlatformArch::X86);
    prior_.x86TaskEnabled = GP::QueryGlassAutorunUiState(GP::PlatformArch::X86) != GP::AutorunUiState::Disabled;
    if (!QueryPlatformMutex(GP::PlatformArch::X64, prior_.x64WasRunning) ||
        !QueryPlatformMutex(GP::PlatformArch::X86, prior_.x86WasRunning)) {
        const DWORD error = GetLastError();
        InstallLog::LineFormat(L"Could not inspect platform mutex before changes (%lu)", error);
        SetFailure(labels_.snapshotFailed.empty() ? L"Could not inspect the platform; no changes were made."
                                                 : labels_.snapshotFailed, error);
        return false;
    }

    // Existing definitions are preserved, never replaced. Inspect every
    // action, executable and argument instead of trusting a task name or just
    // the first parsed DLL path.
    const auto checkTask = [this](GP::PlatformArch arch, bool existed, bool running, const std::wstring& fixedRoot) {
        std::wstring error;
        const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
        if (GlassInstallRemoval::ValidatePlatformTask(arch,fixedRoot,existed,running,ru,error)) return true;
        InstallLog::LineFormat(L"Conflicting or unreadable platform task %s: %s",
            GP::GlassAutorunTaskName(arch),error.c_str());
        SetFailure(error,0);
        return false;
    };
    if (!checkTask(GP::PlatformArch::X64,prior_.x64TaskExisted,prior_.x64WasRunning,x64Dir_) ||
        !checkTask(GP::PlatformArch::X86,prior_.x86TaskExisted,prior_.x86WasRunning,x86Dir_)) return false;
    if ((prior_.x64WasRunning && (!prior_.x64TaskExisted || !prior_.x64TaskEnabled)) ||
        (prior_.x86WasRunning && (!prior_.x86TaskExisted || !prior_.x86TaskEnabled))) {
        // We cannot infer a manually launched host's DLL path from the resolved
        // install destination. Stopping it would make rollback guess what to run.
        const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
        InstallLog::Line(L"Running platform has no enabled autorun task; refusing to guess its rollback path");
        SetFailure(ru ? L"Платформа запущена вручную или с отключённой задачей. Остановите её и повторите установку: безопасно определить путь для отката не удалось. Изменения не внесены."
                      : L"A platform is running manually or with a disabled task. Stop it and run Setup again: its rollback path cannot be determined safely. No changes were made.", 0);
        return false;
    }

    if (!GlassInstallHost::QueryOpenGlassServiceStartType(prior_.ogServiceExisted, prior_.ogServiceStartType)) {
        const DWORD error=GetLastError();
        InstallLog::LineFormat(L"Could not inspect prior OpenGlass service state (%lu)",error);
        SetFailure(labels_.snapshotFailed.empty()?L"Could not inspect the existing service; no changes were made.":labels_.snapshotFailed,error);
        return false;
    }
    GlassInstallUtil::OpenGlassTaskState openGlassTasks;
    if (!GlassInstallUtil::QueryOpenGlassTasks(openGlassTasks)) {
        const DWORD error=GetLastError();
        InstallLog::LineFormat(L"Could not inspect OpenGlass tasks before changes (%lu)",error);
        SetFailure(labels_.snapshotFailed.empty()?L"Could not inspect OpenGlass tasks; no changes were made.":labels_.snapshotFailed,error);
        return false;
    }
    prior_.ogLegacyTaskExisted = openGlassTasks.found;
    prior_.ogLegacyTaskEnabled = !openGlassTasks.anyDisabled;
    if (prior_.ogServiceExisted || prior_.ogLegacyTaskExisted) {
        std::wstring error;
        const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
        if (!GlassInstallRemoval::ValidateOpenGlassPaths(openGlassDir_,&cancelRequested_,ru,error)) {
            if (!IsCancelRequested()) {
                InstallLog::LineFormat(L"Conflicting or unreadable OpenGlass backend: %s",error.c_str());
                SetFailure(error,0);
            }
            return false;
        }
    }
    // This later state is for rollback only; launch policy uses the separate
    // snapshot taken when the wizard was opened.
    if (!GlassInstallUtil::QueryOpenGlassRunning(prior_.ogWasRunning, &openGlassTasks)) {
        const DWORD error=GetLastError();
        InstallLog::LineFormat(L"Could not inspect running OpenGlass state before changes (%lu)",error);
        SetFailure(labels_.snapshotFailed.empty()?L"Could not inspect OpenGlass; no changes were made.":labels_.snapshotFailed,error);
        return false;
    }
    // Do not stop an externally managed or ambiguous running instance when
    // rollback cannot know which backend must be restarted.
    if (prior_.ogWasRunning && ((!prior_.ogServiceExisted && !prior_.ogLegacyTaskExisted) ||
        (prior_.ogServiceExisted && openGlassTasks.anyRunning) ||
        (openGlassTasks.anyRunning && (openGlassTasks.multipleTasks || openGlassTasks.multipleRunningInstances)))) {
        const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
        SetFailure(ru ? L"Текущее состояние OpenGlass нельзя безопасно восстановить. Остановите OpenGlass и снова запустите установщик; изменений не внесено."
                      : L"The current OpenGlass state cannot be safely restored. Stop OpenGlass and restart Setup; no changes were made.",0);
        return false;
    }
    // A disabled legacy task cannot be started without enabling it. Do not
    // silently change that preference or stop an instance we cannot restore.
    if (!prior_.ogServiceExisted && prior_.ogLegacyTaskExisted && !prior_.ogLegacyTaskEnabled &&
        (prior_.ogWasRunning || (selection_.openGlass && GlassProducts::ShouldStartOpenGlass(openGlassStartup_)))) {
        const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
        SetFailure(ru ? L"OpenGlass работает с отключённой старой задачей. Остановите OpenGlass или включите задачу и заново откройте установщик; изменений не внесено."
                      : L"OpenGlass uses a disabled legacy task. Stop OpenGlass or enable the task, then restart Setup; no changes were made.", 0);
        return false;
    }
    prior_.shortcutExisted = GlassInstallUtil::StartMenuShortcutExists();
    return true;
}

// Rollback (keptOnly=false): every host Setup stopped is brought back. Success
// (keptOnly=true): only the products Setup stopped but did not reinstall are
// restored, i.e. installed products that were left unchecked and are kept;
// the selected products are started by the launch phase instead.
bool InstallEngine::RestorePriorHosts(bool keptOnly) noexcept {
    if (!hostsStopped_) return true;
    bool ok=true;
    auto restoreArch=[&](GP::PlatformArch arch,const std::wstring& root,bool existed,bool enabled,bool running,bool stopAttempted) -> bool {
        if (!running) return true;
        // A timed-out stop may still complete later. Do not report rollback as
        // successful or restart another instance while that stop is in flight.
        if (stopAttempted && !GP::WaitForPlatformStopped(arch,30000)) return false;
        bool hostRunning = false;
        if (!QueryPlatformMutex(arch, hostRunning)) return false;
        if (hostRunning) return true;
        if (existed && enabled) {
            std::wstring error;
            const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
            if (!GlassInstallRemoval::ValidatePlatformTask(arch,root,true,false,ru,error)) {
                InstallLog::LineFormat(L"Rollback: refusing to start changed autorun task (%s): %s",
                    GP::GlassAutorunTaskName(arch),error.c_str());
                return false;
            }
            GP::StartPlatform(arch);
        } else {
            // CapturePriorHostState rejects this unsupported situation before
            // any stop; never invent a DLL path if the state changed later.
            InstallLog::Line(L"Rollback: refusing to guess a manually started platform path");
            return false;
        }
        const ULONGLONG start=GetTickCount64();
        do {
            if (!QueryPlatformMutex(arch, hostRunning)) return false;
            if (hostRunning) return true;
            Sleep(200);
        } while (GetTickCount64()-start<30000);
        return false;
    };
    if (!keptOnly || !selection_.platform64)
        if (!restoreArch(GP::PlatformArch::X64,x64Dir_,prior_.x64TaskExisted,prior_.x64TaskEnabled,prior_.x64WasRunning,x64StopAttempted_)) ok=false;
    if (!keptOnly || !selection_.platform32)
        if (!restoreArch(GP::PlatformArch::X86,x86Dir_,prior_.x86TaskExisted,prior_.x86TaskEnabled,prior_.x86WasRunning,x86StopAttempted_)) ok=false;
    if (keptOnly && selection_.openGlass) return ok; // selected OpenGlass is handled by the launch phase
    bool ogPathsSafe=true;
    if (((prior_.ogWasRunning && ogStopSignalled_) || ogServiceStartTypeMayNeedRestore_) &&
        (prior_.ogServiceExisted || prior_.ogLegacyTaskExisted)) {
        std::wstring error;
        const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
        ogPathsSafe=GlassInstallRemoval::ValidateOpenGlassPaths(openGlassDir_,nullptr,ru,error);
        if (!ogPathsSafe) { InstallLog::LineFormat(L"Rollback: refusing to start changed OpenGlass backend: %s",error.c_str());ok=false; }
    }
    if (ogPathsSafe && !prior_.ogServiceExisted && prior_.ogLegacyTaskExisted && prior_.ogWasRunning && ogStopSignalled_) {
        // Never enable a task as a side effect of rollback.
        if (!prior_.ogLegacyTaskEnabled || !GlassInstallUtil::RunEnabledOpenGlassTask()) ok=false;
        else {
            bool running=false, known=true;
            const ULONGLONG start=GetTickCount64();
            do {
                known=GlassInstallUtil::QueryOpenGlassRunning(running);
                if (!known || running || GetTickCount64()-start>=15000) break;
                Sleep(300);
            } while (true);
            if (!known || !running) ok=false;
        }
    }
    if (ogServiceStartTypeMayNeedRestore_) {
        if (ogPathsSafe && GlassInstallHost::RestoreOpenGlassServiceStartType(prior_.ogServiceStartType))
            ogServiceStartTypeMayNeedRestore_=false;
        else ok=false;
    }
    if (ogPathsSafe && prior_.ogServiceExisted && prior_.ogWasRunning && ogStopSignalled_) {
        if (prior_.ogServiceStartType==SERVICE_DISABLED) ogServiceStartTypeMayNeedRestore_=true;
        if (GlassInstallHost::StartOpenGlassServicePreservingStartType(prior_.ogServiceStartType))
            ogServiceStartTypeMayNeedRestore_=false;
        else ok=false;
        if (ogServiceStartTypeMayNeedRestore_ &&
            GlassInstallHost::RestoreOpenGlassServiceStartType(prior_.ogServiceStartType))
            ogServiceStartTypeMayNeedRestore_=false;
        if (ogServiceStartTypeMayNeedRestore_) ok=false;
    }
    return ok;
}

DWORD WINAPI InstallEngine::WorkerThread(LPVOID p) noexcept {
            auto* self = static_cast<InstallEngine*>(p);
            bool priorStateOk=self->CapturePriorHostState();
            if (priorStateOk) priorStateOk=self->DetermineCleanInstall();
            if (self->forceClean_)
                InstallLog::Line(L"Clean reinstall: remnants will be removed before install");

            bool ok = priorStateOk && !self->IsCancelRequested() && self->ExtractPayload(self->instance_);
            bool snapshotFailed = !priorStateOk && !self->IsCancelRequested();

            // Check the embedded helper files without loading or executing them.
            // A stale DLL must be diagnosed before stopping hosts or changing MSI.
            if (ok && !self->IsCancelRequested()) ok = self->VerifyEmbeddedNativePayload();

            // Снимок до стопа хостов: если на диске нет места или нажата Отмена, система не затрагивается.
            if (ok) {
                // Snapshot even on a clean/reinstall path: pre-existing user files must survive cancellation.
                ok = self->CreateRollbackSnapshot();
                snapshotFailed = !ok && !self->IsCancelRequested();
            }

            if (ok && !self->IsCancelRequested()) {
                GlassInstallUtil::CloseGlassSettingsProcesses(&self->cancelRequested_);
                if (!self->IsCancelRequested()) ok = self->StopRunningHosts();
            }

            // The chosen Settings product is installed after the snapshot and
            // platform-stop/mutex wait, but before the two Native monitors.
            // If it is not chosen, only embedded helpers are used; no temporary
            // Settings MSI is registered. Record changes before starting MSI so
            // cancellation or a scan failure takes the normal rollback path.
            if (ok && !self->IsCancelRequested()) ok = self->InstallGlassSettingsEarly();
            // Clean install: on the machine there is neither a platform nor tasks,
            // services or products that could have loaded Glass modules: nothing to
            // scan, nothing to release. The stage is skipped (as requested): the scan
            // used to open Process32/Module32 and, on a mixed x86/x64 machine, the
            // native monitor, for nothing.
            // If the picture is not clean, the stage still runs in full.
            if (ok && !self->IsCancelRequested()) {
                if (self->cleanInstall_) {
                    InstallLog::Line(L"Unlocker: clean install - module scan skipped");
                } else {
                    ok = self->ReleaseLockedModules();
                }
            }
            if (ok && !self->IsCancelRequested() && self->forceClean_) {
                self->installAttempted_ = true;
                self->nonSettingsChangesAttempted_ = true;
                ok = self->PurgeRemnants();
                if (ok) self->cleanInstall_ = true;
            }
            if (ok && !self->IsCancelRequested()) {
                self->installAttempted_ = true;
                self->nonSettingsChangesAttempted_ = true;
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
                // Phase B still rolls back: tasks and the service are registered
                // and validated while nothing has been started yet.
                ok = self->RegisterSelectedProducts();
                if (ok) {
                    // Commit point. From here on the products are started and the
                    // installation is final: no step below may fail the install
                    // or trigger a rollback; problems become finish-page notes.
                    InstallLog::Line(L"Launch: commit point reached, rollback is no longer possible");
                    self->LaunchSelectedProducts();
                    constexpr ULONGLONG kMinLaunchMs = 5000;
                    for (;;) {
                        const ULONGLONG elapsed = GetTickCount64() - launchStart;
                        if (elapsed >= kMinLaunchMs)
                            break;
                        const ULONGLONG remain = kMinLaunchMs - elapsed;
                        Sleep(remain > 150 ? 150 : static_cast<DWORD>(remain));
                    }
                    self->SetStatus(self->labels_.finalize, 10000);
                }
            }

            MsiEnableLogW(0, nullptr, 0);

            const bool cancelled = self->ChooseCancellationOrFailure(!ok);
            bool restored=true;
            // The per-user default lists are written before the commit point;
            // a rolled-back installation must not leave them behind.
            const auto undoUserDefaults = [&]() {
                if (!self->userDefaults_.attachListCreated && !self->userDefaults_.blockListCreated) return;
                GlassUserDefaults::UndoSeededProcessLists(self->userDefaults_);
                InstallLog::Line(L"User defaults: seeded AttachList/BlockList removed by rollback");
                self->userDefaults_ = GlassUserDefaults::SeedReport{};
            };
            if (!ok || cancelled) {
                if (cancelled) {
                    // A timeout or MSI error may have raced with the user's
                    // confirmation. Cancellation owns the outcome; retain
                    // only new errors actually produced by restoration.
                    AcquireSRWLockExclusive(&self->statusLock_);
                    self->error_.clear();
                    self->failureDetail_.clear();
                    self->msiMessage_.clear();
                    self->msiExitCode_ = 0;
                    ReleaseSRWLockExclusive(&self->statusLock_);
                    self->rollingBack_.store(true, std::memory_order_release);
                    if (self->installAttempted_) {
                        if (self->rollbackSession_.hadPriorInstall) {
                            restored=self->PerformRollback();
                            undoUserDefaults();
                            if (restored && !self->RestorePriorHosts()) restored=false;
                        } else {
                            restored=self->UndoCleanInstall();
                            undoUserDefaults();
                            if (restored && !self->RestorePriorHosts()) restored=false;
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
                        undoUserDefaults();
                        if (restored && !self->RestorePriorHosts()) restored=false;
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
                        undoUserDefaults();
                        if (restored && !self->RestorePriorHosts()) restored=false;
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
            // The local Native module has been released before rollback and no
            // helper process is needed beyond this point.
            GlassInstallUtil::CleanupEmbeddedUnlocker();

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
    GlassProducts::OpenGlassStartupState openGlassStartup,
    bool forceClean,
    PhaseLabels labels,
    StatusFn onStatus) noexcept {
    if (started_) return false;
    started_ = true;
    instance_ = instance;
    labels_ = std::move(labels);
    status_ = std::move(onStatus);
    selection_ = selection;
    openGlassStartup_ = openGlassStartup;
    forceClean_ = forceClean;
    x64Dir_ = x64Dir;
    x86Dir_ = x86Dir;
    openGlassDir_ = openGlassDir;
    cancelRequested_.store(false, std::memory_order_relaxed);
    userCancelled_.store(false, std::memory_order_relaxed);
    rollingBack_.store(false, std::memory_order_relaxed);
    failureFinalizing_.store(false, std::memory_order_relaxed);
    launchingPhase_.store(false, std::memory_order_relaxed);

    selection_=GlassProducts::NormalizeInstall(selection_,GlassInstallUtil::IsOs64Bit());
    const bool settingsInstalled=GlassInstallMsi::IsProductInstalled(GlassInstallMsi::kProductSettingsX64)
        || GlassInstallMsi::IsProductInstalled(GlassInstallMsi::kProductSettingsX86);
    const bool sutMsiInstalled=GlassInstallMsi::IsProductInstalled(GlassInstallMsi::kProductSecureUxThemeX64)
        || GlassInstallMsi::IsProductInstalled(GlassInstallMsi::kProductSecureUxThemeX86);
    bool legacySutPayload=false;
    if ((selection_.secureUxTheme || selection_.glassSettings) && settingsInstalled && !sutMsiInstalled) {
        GlassInstallUtil::SecureUxThemeState sutState=GlassInstallUtil::SecureUxThemeState::Unavailable;
        if (GlassInstallUtil::QuerySecureUxThemeState(x64Dir_,x86Dir_,&sutState,nullptr))
            legacySutPayload=sutState==GlassInstallUtil::SecureUxThemeState::Active ||
                sutState==GlassInstallUtil::SecureUxThemeState::Inactive ||
                sutState==GlassInstallUtil::SecureUxThemeState::ActivationPending ||
                sutState==GlassInstallUtil::SecureUxThemeState::DeactivationPending;
    }

    if (selection_.glassSettings && legacySutPayload && !selection_.secureUxTheme)
        selection_.secureUxThemePackageOnly=true;

    GlassInstallUtil::ConfigureEmbeddedUnlocker(instance, IDR_UNLOCKER_X64, IDR_UNLOCKER_X86);

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
    InstallLog::LineFormat(L"  glassSettings=%d openGlass=%d SecureUxTheme=%d (%s)",
        selection_.glassSettings ? 1 : 0, selection_.openGlass ? 1 : 0,selection_.secureUxTheme?1:0,openGlassDir_.c_str());
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
    if (action==HoldAction::Retry) unlockActivity_.Begin(WizardUnlock::Phase::Modules);
    else if (action==HoldAction::Unlock) unlockActivity_.Begin(WizardUnlock::Phase::Processes);
    else unlockActivity_.End();
    ReleaseSRWLockExclusive(&statusLock_);
    if (action == HoldAction::Cancel)
        cancelRequested_.store(true, std::memory_order_release);
    if (holdGate_) SetEvent(holdGate_);
}

bool InstallEngine::StopGlassPlatform(GP::PlatformArch arch, const std::wstring& statusLabel,
    const wchar_t* logTag, const std::function<bool(const wchar_t*)>& fail) noexcept {
    SetStatus(statusLabel, -1);
    bool running = false;
    if (!QueryPlatformMutex(arch, running)) return fail(L"GlassPlatform mutex state");
    const bool wasRunning = arch == GP::PlatformArch::X64 ? prior_.x64WasRunning : prior_.x86WasRunning;
    if (running && !wasRunning) {
        // Do not stop a host that started after the rollback state was captured.
        const bool ru = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN;
        SetFailure(ru ? L"Состояние платформы изменилось после создания копии. Повторите установку."
                      : L"The platform state changed after the snapshot. Run Setup again.", 0);
        return false;
    }
    if (!running) {
        InstallLog::LineFormat(L"%s: host already stopped", logTag);
        return !IsCancelRequested();
    }
    std::wstring taskError;
    const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
    const auto& fixedRoot=arch==GP::PlatformArch::X64?x64Dir_:x86Dir_;
    if (!GlassInstallRemoval::ValidatePlatformTask(arch,fixedRoot,true,true,ru,taskError)) {
        SetFailure(taskError,0);
        return false;
    }
    bool attempted=false;
    taskError.clear();
    const bool stopped=GlassInstallHost::StopGlassPlatform(
        arch,fixedRoot,&cancelRequested_,&attempted,&taskError);
    if (arch==GP::PlatformArch::X64) x64StopAttempted_=attempted;
    else x86StopAttempted_=attempted;
    if (IsCancelRequested()) return false;
    if (!stopped) {
        InstallLog::LineFormat(L"%s: task stop failed or host did not settle",logTag);
        if (!taskError.empty()) { SetFailure(taskError,0); return false; }
        return fail(L"GlassPlatform host");
    }
    // Residual modules are monitored later in ReleaseLockedModules, with
    // their own visible status and unlock pages.
    InstallLog::LineFormat(L"%s: host stopped",logTag);
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

    hostsStopped_ = true;
    // Recheck both mutexes even on a seemingly clean install: a host may have
    // appeared after the initial snapshot. Unknown mutex state fails closed.
    if (!StopGlassPlatform(GP::PlatformArch::X64, labels_.stop64, L"x64", fail)) return false;
    if (!StopGlassPlatform(GP::PlatformArch::X86, labels_.stop86, L"x86", fail)) return false;
    if (IsCancelRequested()) return false;

    bool openGlassRunning = false;
    const bool openGlassKnown = GlassInstallUtil::QueryOpenGlassRunning(openGlassRunning);
    const DWORD stateError = openGlassKnown ? ERROR_SUCCESS : GetLastError();
    if (IsCancelRequested()) return false;
    if (!openGlassKnown) {
        SetFailure(ru ? L"Не удалось проверить состояние службы или задачи OpenGlass."
                      : L"Could not check the OpenGlass service or task state.", stateError);
        return false;
    }
    if (!openGlassRunning) {
        InstallLog::Line(L"OpenGlass: service/task already stopped; no shutdown or wait");
    } else {
        // Do not stop a host that appeared after its rollback state was saved.
        if (!prior_.ogWasRunning) {
            SetFailure(ru ? L"Состояние OpenGlass изменилось после создания копии. Повторите установку."
                          : L"OpenGlass changed state after the snapshot. Run Setup again.", 0);
            return false;
        }
        GlassInstallUtil::OpenGlassTaskState currentTasks;
        bool currentServiceExists = false;
        DWORD currentStartType = SERVICE_AUTO_START;
        const bool backendKnown = GlassInstallUtil::QueryOpenGlassTasks(currentTasks) &&
            GlassInstallHost::QueryOpenGlassServiceStartType(currentServiceExists, currentStartType);
        const DWORD backendError = backendKnown ? ERROR_SUCCESS : GetLastError();
        if (IsCancelRequested()) return false;
        if (!backendKnown) {
            SetFailure(ru ? L"Не удалось повторно проверить службу или задачу OpenGlass."
                          : L"Could not recheck the OpenGlass service or task.", backendError);
            return false;
        }
        const bool wasTaskRunning = !prior_.ogServiceExisted && prior_.ogLegacyTaskExisted && prior_.ogWasRunning;
        if (currentServiceExists != prior_.ogServiceExisted ||
            (currentServiceExists && currentStartType != prior_.ogServiceStartType) ||
            currentTasks.found != prior_.ogLegacyTaskExisted ||
            (currentTasks.found && (!currentTasks.anyDisabled) != prior_.ogLegacyTaskEnabled) ||
            currentTasks.anyRunning != wasTaskRunning ||
            (currentTasks.anyRunning && (currentTasks.multipleTasks || currentTasks.multipleRunningInstances))) {
            SetFailure(ru ? L"Состояние OpenGlass изменилось после создания копии. Повторите установку."
                          : L"OpenGlass changed state after the snapshot. Run Setup again.", 0);
            return false;
        }
        std::wstring pathError;
        if (!GlassInstallRemoval::ValidateOpenGlassPaths(openGlassDir_,&cancelRequested_,ru,pathError)) {
            if (!IsCancelRequested()) SetFailure(pathError,0);
            return false;
        }
        SetStatus(labels_.stopOg, -1);
        InstallLog::Line(L"OpenGlass: stopping running service/task");
        const bool stopped = GlassInstallHost::StopOpenGlassHost(&cancelRequested_, &ogStopSignalled_);
        const DWORD stopError = stopped ? ERROR_SUCCESS : GetLastError();
        if (IsCancelRequested()) return false;
        if (!stopped) {
            if (stopError == ERROR_TIMEOUT) return fail(L"OpenGlassHost");
            SetFailure(ru ? L"Не удалось остановить службу или задачу OpenGlass."
                          : L"Could not stop the OpenGlass service or task.", stopError);
            return false;
        }
    }

    InstallLog::Line(L"Hosts stopped");
    return true;
}

bool InstallEngine::ReleaseLockedModules() noexcept {
    using ModuleHolder = GlassInstallUnlock::ModuleHolder;
    WizardUnlock::EndOnExit finishUnlock(unlockActivity_);
    auto publish = [&](HoldKind kind, const std::vector<ModuleHolder>& holders) {
        AcquireSRWLockExclusive(&statusLock_);
        if (!cancelRequested_.load(std::memory_order_acquire)) {
            // A new prompt replaces the previous active unlock animation.
            unlockActivity_.End();
            if (holdGate_) ResetEvent(holdGate_);
            holdKind_ = kind;
            holdReport_ = GlassInstallUnlock::FormatHolders(holders);
            holdAction_ = HoldAction::None;
        }
        ReleaseSRWLockExclusive(&statusLock_);
    };

    const ULONGLONG scanStarted=GetTickCount64();
    SetStatus(labels_.unload.empty() ? L"…" : labels_.unload, -1);
    bool nativeFailed = false;
    const auto failScan = [&]() {
        if (!IsCancelRequested()) SetFailure(labels_.nativeScanFailed.empty()
            ? L"Process module scan failed." : labels_.nativeScanFailed, 0);
        return false;
    };
#ifdef GLASS_SETUP_NATIVE_MONITOR
    if (!GlassInstallWatch::BeginLocalNativeMonitor()) return failScan();
    struct LocalMonitorScope {
        ~LocalMonitorScope() { GlassInstallWatch::EndLocalNativeMonitor(); }
    } localMonitorScope;
#endif
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
            GlassInstallUnlock::CloseMatchingProcessWindows(holder.pid, holder.exe.c_str());
        }
    };

    std::vector<ModuleHolder> holders;
    // Start the resident x64 rundll32 monitor before the first local x86 scan;
    // the watcher keeps both results current while modules unload naturally.
    const bool cleared = GlassInstallWatch::WaitUntilClearOrTimeout(
        30000, x64Dir_, x86Dir_, scanNativeX64_, holders, nativeFailed, &cancelRequested_);
    if (IsCancelRequested()) return false;
    if (nativeFailed) return failScan();
    if (cleared) holders.clear();
    nudgeSettings(holders);
    // A very fast empty scan must still be visible to the user. Remain
    // cancellable while the UI receives and paints the real scanning status.
    while (GetTickCount64()-scanStarted < 600) {
        if (IsCancelRequested()) return false;
        Sleep(50);
    }
    if (!holders.empty()) {
        InstallLog::LineFormat(L"Unlocker: %u processes still hold Glass modules after stop",
            static_cast<unsigned>(holders.size()));
        for (const auto& holder : holders) {
            InstallLog::LineFormat(L"  holder %s pid %lu%s: %s",
                holder.exe.c_str(), holder.pid,
                GlassInstallUnlock::TargetIsWow64(holder.pid) ? L" wow64" : L"",
                holder.modules.c_str());
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
            holders = GlassInstallUnlock::CollectHolders(x64Dir_, x86Dir_, scanNativeX64_, nativeFailed, &cancelRequested_);
            if (IsCancelRequested()) return false;
            if (nativeFailed) return failScan();
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
        holders = GlassInstallUnlock::CollectHolders(x64Dir_, x86Dir_, scanNativeX64_, nativeFailed, &cancelRequested_);
        if (IsCancelRequested()) return false;
        if (nativeFailed) return failScan();
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
    const bool os64 = scanNativeX64_; // verified before this worker stage
    const ResourceBlob blobs[] = {
        {IDR_MSI_X64,       L"Glass-x64.msi",         os64 && selection_.platform64},
        {IDR_MSI_X86,       L"Glass-x86.msi",         selection_.platform32},
        {IDR_MSI_GS_X64,    L"GlassSettings-x64.msi", os64 && selection_.glassSettings},
        {IDR_MSI_GS_X86,    L"GlassSettings-x86.msi", !os64 && selection_.glassSettings},
        {IDR_MSI_SUT_X64,   L"SecureUxTheme-x64.msi", os64 && (selection_.secureUxTheme || selection_.secureUxThemePackageOnly)},
        {IDR_MSI_SUT_X86,   L"SecureUxTheme-x86.msi", !os64 && (selection_.secureUxTheme || selection_.secureUxThemePackageOnly)},
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

bool InstallEngine::VerifyEmbeddedNativePayload() noexcept {
    const bool ru = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN;
    auto check = [&](const std::wstring& path, WORD machine, const wchar_t* arch) {
        if (IsCancelRequested()) return false;
        // Parsing the export table does not load the DLL or run DllMain. The
        // normal monitoring session still begins only after the Settings MSI.
        if (!path.empty() && GlassInstallUtil::PeMatches(path, machine,
                Shared::InjectModuleScan::kNativeInstallerProtocolExport) &&
            GlassInstallUtil::PeMatches(path, machine, GlassInstallUtil::kInstallerShellExport)) {
            InstallLog::LineFormat(L"Native preflight %s: scan and installer-shell exports present (%s)", arch, path.c_str());
            return true;
        }
        InstallLog::LineFormat(L"Native preflight %s: embedded DLL missing, unreadable or missing an installer export (path=%s)",
            arch, path.empty() ? L"<none>" : path.c_str());
        if (!IsCancelRequested()) {
            const std::wstring message = ru
                ? std::wstring(L"Встроенная библиотека GlassSettings.Native ") + arch +
                    L" отсутствует или несовместима с установщиком. Проверьте журнал и пересоберите решение: Release | x64, затем Release | x86. Установленные продукты не изменялись."
                : std::wstring(L"The embedded GlassSettings.Native ") + arch +
                    L" is missing or incompatible with this installer. Check the log and rebuild the solution: Release | x64, then Release | x86. Installed products were not changed.";
            SetFailure(message, 0);
        }
        return false;
    };

    const std::wstring x86 = GlassInstallUtil::EmbeddedNativeDllX86();
    if (!check(x86, IMAGE_FILE_MACHINE_I386, L"x86")) return false;
    if (scanNativeX64_) {
        if (IsCancelRequested()) return false;
        const std::wstring x64 = GlassInstallUtil::NativeDllForArch(GP::PlatformArch::X64, x64Dir_, x86Dir_);
        if (!check(x64, IMAGE_FILE_MACHINE_AMD64, L"x64")) return false;
    }
    return !IsCancelRequested();
}

bool InstallEngine::RecheckSnapshottedProduct(const wchar_t* productCode) noexcept {
    if (!productCode || !*productCode || IsCancelRequested()) return false;
    std::wstring detail;
    const auto context=GlassInstallMsi::CheckProductContexts(productCode,detail,&cancelRequested_);
    if (context==GlassInstallMsi::ProductPathCheck::Cancelled) return false;
    for (const auto& snap:rollbackSession_.oldProducts) {
        if (_wcsicmp(snap.productCode.c_str(),productCode)!=0) continue;
        auto paths=context;
        if (context==GlassInstallMsi::ProductPathCheck::Safe &&
            GlassInstallMsi::IsProductInstalled(productCode) && !rollbackSession_.workspaceDir.empty()) {
            // Check the currently registered LocalPackage, not only the copy
            // from the original snapshot: it may have changed in the meantime.
            const std::wstring current=rollbackSession_.workspaceDir+L"\\recheck-"+productCode+L".msi";
            if (GlassInstallMsi::CopyLocalPackage(productCode,current,&cancelRequested_))
                paths=GlassInstallMsi::CheckProductFileRoots(productCode,current,snap.installDir,detail,&cancelRequested_);
            else {
                paths=IsCancelRequested()?GlassInstallMsi::ProductPathCheck::Cancelled:GlassInstallMsi::ProductPathCheck::Unverifiable;
                detail=L"Current cached MSI is unavailable";
            }
            DeleteFileW(current.c_str());
        } else if (context==GlassInstallMsi::ProductPathCheck::Safe) {
            paths=GlassInstallMsi::ProductPathCheck::Unverifiable;
            detail=L"Snapshotted MSI is no longer installed or its workspace is unavailable";
        }
        if (paths==GlassInstallMsi::ProductPathCheck::Cancelled) return false;
        if (paths==GlassInstallMsi::ProductPathCheck::Safe) return true;
        break;
    }
    if (!IsCancelRequested()) {
        InstallLog::LineFormat(L"MSI changed after snapshot; refusing operation: %s (%s)",productCode,detail.c_str());
        const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
        SetFailure(ru ? L"Расположение или регистрация MSI изменились после создания копии. Операция остановлена; сохраните журнал и обратитесь за помощью."
                      : L"The MSI location or registration changed after the snapshot. The operation stopped; save the log and ask for help.",ERROR_INVALID_DATA);
    }
    return false;
}

bool InstallEngine::RecheckRelatedProducts(const wchar_t* productCode) noexcept {
    if (!productCode || !*productCode || IsCancelRequested()) return false;
    struct Family { const wchar_t* code; const wchar_t* legacy; const wchar_t* upgrade; };
    const Family families[]={
        {GlassInstallMsi::kProductPlatformX64,GlassInstallMsi::kLegacyPlatformX64,L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5010}"},
        {GlassInstallMsi::kProductPlatformX86,GlassInstallMsi::kLegacyPlatformX86,L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5020}"},
        {GlassInstallMsi::kProductOpenGlass,GlassInstallMsi::kLegacyOpenGlass,L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5030}"},
        {GlassInstallMsi::kProductSettingsX64,nullptr,L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5040}"},
        {GlassInstallMsi::kProductSettingsX86,nullptr,L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5050}"},
        {GlassInstallMsi::kProductSecureUxThemeX64,nullptr,L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5080}"},
        {GlassInstallMsi::kProductSecureUxThemeX86,nullptr,L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5090}"},
    };
    const Family* family=nullptr;
    for (const auto& item:families) if (_wcsicmp(item.code,productCode)==0) { family=&item;break; }
    if (family) for (DWORD i=0;;++i) {
        if (IsCancelRequested()) return false;
        wchar_t related[39]{};
        const UINT status=MsiEnumRelatedProductsW(family->upgrade,0,i,related);
        if (status==ERROR_NO_MORE_ITEMS) return true;
        if (status!=ERROR_SUCCESS) break;
        const bool known=_wcsicmp(related,family->code)==0 ||
            (family->legacy && _wcsicmp(related,family->legacy)==0);
        if (!known || !GlassInstallMsi::IsProductInstalled(related) ||
            !ProductWasSnapshotted(rollbackSession_,related)) break;
        if (!RecheckSnapshottedProduct(related)) return false;
    }
    if (!IsCancelRequested()) {
        InstallLog::LineFormat(L"Unknown or changed related MSI product: %s",productCode);
        const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
        SetFailure(ru ? L"Связанный MSI-продукт изменился или неизвестен. Установка остановлена; сохраните журнал и обратитесь за помощью."
                      : L"A related MSI product is unknown or changed. Setup stopped; save the log and ask for help.",ERROR_INVALID_DATA);
    }
    return false;
}

bool InstallEngine::RunOneMsi(const std::wstring& msiPath, const std::wstring& installDir,
    const std::wstring& label, int progressBase, int progressSpan) noexcept {
    if (IsCancelRequested()) return false;
    if (!PathFileExistsW(msiPath.c_str())) {
        SetFailure(std::wstring(L"MSI not found: ") + msiPath, 0);
        return false;
    }
    const auto targetSafe=[&]() {
        if (GlassInstallUtil::IsSafeInstallPath(installDir)) return true;
        const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
        InstallLog::LineFormat(L"MSI target or ancestor is unsafe: %s",installDir.c_str());
        SetFailure(ru ? L"Каталог установки или его родительский каталог небезопасен. Сохраните журнал и обратитесь за помощью."
                      : L"The installation directory or an ancestor is unsafe. Save the log and ask for help.",ERROR_INVALID_DATA);
        return false;
    };
    if (!targetSafe()) return false;
    SetStatus(label, progressBase);
    AcquireSRWLockExclusive(&statusLock_);
    msiMessage_.clear();
    ReleaseSRWLockExclusive(&statusLock_);

    const std::wstring msiLog = InstallLog::NextMsiVerboseLogPath(label.c_str());
    // Removal and installation are separate MSI sessions. Append both so the
    // second session does not erase evidence from the preceding uninstall.
    if (!msiLog.empty()) {
        const UINT logResult=MsiEnableLogW(INSTALLLOGMODE_VERBOSE, msiLog.c_str(),
            INSTALLLOGATTRIBUTES_FLUSHEACHLINE | INSTALLLOGATTRIBUTES_APPEND);
        InstallLog::LineFormat(L"MSI verbose log: %s (enable=%u)", msiLog.c_str(),logResult);
    }
    const std::wstring productCode = GlassInstallMsi::ResolveProductCode(msiPath);
    GlassInstallMsi::ProgressSink sink;
    sink.label = label; sink.base = progressBase; sink.span = progressSpan;
    sink.cancelRequested = &cancelRequested_;
    sink.onError = [this](const std::wstring& text) { RememberMsiMessage(text.c_str()); };
    sink.onProgress = [this](const std::wstring& lbl, int units) { SetStatus(lbl, units); };
    if (!RecheckRelatedProducts(productCode.c_str())) { MsiEnableLogW(0,nullptr,0);return false; }
    if (!productCode.empty() && GlassInstallMsi::IsProductInstalled(productCode.c_str())) {
        if (!ProductWasSnapshotted(rollbackSession_, productCode.c_str())) {
            MsiEnableLogW(0,nullptr,0);
            const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
            SetFailure(ru ? L"Продукт появился после создания резервной копии. Повторите установку."
                          : L"The product appeared after the snapshot. Run Setup again.", 0);
            return false;
        }
        if (!RecheckSnapshottedProduct(productCode.c_str())) { MsiEnableLogW(0,nullptr,0);return false; }
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

    if (!RecheckRelatedProducts(productCode.c_str()) || !targetSafe()) { MsiEnableLogW(0,nullptr,0);return false; }
    std::wstring props = GlassInstallMsi::BuildInstallPropertyString(installDir, false);
    const wchar_t* msiName=PathFindFileNameW(msiPath.c_str());
    const bool secureUxThemeMsi=msiName &&
        (_wcsicmp(msiName,L"SecureUxTheme-x64.msi")==0 || _wcsicmp(msiName,L"SecureUxTheme-x86.msi")==0);
    if (secureUxThemeMsi && selection_.glassSettings) {
        // Setup installs matching GlassSettings first, within the same MSI
        // transaction; the dependent package's RegistrySearch cannot see that
        // uncommitted registration yet. When Setup is not installing Settings,
        // the package's registry prerequisite search must validate it instead.
        props+=L" GLASSSETTINGSINSTALLED=1";
    }
    InstallLog::LineFormat(L"MsiInstallProduct %s", msiPath.c_str());
    InstallLog::LineFormat(L"  %s", props.c_str());

    UINT exitCode = GlassInstallMsi::InstallProduct(msiPath, props, &sink);
    if (exitCode == ERROR_PRODUCT_VERSION && !IsCancelRequested()) {
        InstallLog::Line(L"MSI 1638 — forced uninstall and retry");
        MsiEnableLogW(INSTALLLOGMODE_VERBOSE,msiLog.c_str(),
            INSTALLLOGATTRIBUTES_FLUSHEACHLINE | INSTALLLOGATTRIBUTES_APPEND);
        if (!productCode.empty() && GlassInstallMsi::IsProductInstalled(productCode.c_str())) {
            if (!ProductWasSnapshotted(rollbackSession_,productCode.c_str())) {
                MsiEnableLogW(0,nullptr,0);
                const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
                SetFailure(ru ? L"Продукт появился после создания резервной копии. Повторите установку."
                              : L"The product appeared after the snapshot. Run Setup again.", 0);
                return false;
            }
            if (!RecheckSnapshottedProduct(productCode.c_str())) { MsiEnableLogW(0,nullptr,0);return false; }
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
        // A failed install and the forced removal can change the UpgradeCode
        // family; MajorUpgrade must be checked again before the retry.
        if (!RecheckRelatedProducts(productCode.c_str()) || !targetSafe()) { MsiEnableLogW(0,nullptr,0);return false; }
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

bool InstallEngine::InstallGlassSettingsEarly() noexcept {
    if (!selection_.glassSettings) {
        InstallLog::Line(L"GlassSettings not selected: monitor will use embedded temporary Native DLLs");
        return true;
    }
    bool settingsRunning = false;
    const bool settingsKnown = GlassInstallUtil::QueryImageRunning(L"GlassSettings.exe", settingsRunning);
    const DWORD settingsQueryError = settingsKnown ? 0 : GetLastError();
    if (!settingsKnown || settingsRunning) {
        const bool ru = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN;
        SetFailure(!settingsKnown
            ? (ru ? L"Не удалось проверить состояние GlassSettings. Повторите установку."
                  : L"Could not check whether GlassSettings is running. Run Setup again.")
            : (ru ? L"Закройте GlassSettings и повторите установку."
                  : L"Close GlassSettings and run Setup again."),
            settingsQueryError);
        return false;
    }
    const bool is64 = scanNativeX64_; // checked before extraction and host changes
    const std::wstring file = is64 ? L"GlassSettings-x64.msi" : L"GlassSettings-x86.msi";
    const std::wstring& dir = is64 ? x64Dir_ : x86Dir_;
    const std::wstring& label = is64 ? labels_.glassSettings64 : labels_.glassSettings86;
    wchar_t path[MAX_PATH]{};
    if (!PathCombineW(path, tempDir_.c_str(), file.c_str())) {
        SetFailure(L"GlassSettings MSI path is too long.", 0);
        return false;
    }
    // RunOneMsi records a previous product before removal and a newly
    // installed product before checking cancellation. Rollback therefore also
    // works if the monitor or a later package fails.
    installAttempted_ = true;
    return RunOneMsi(path, dir, label, 800, 1200);
}

bool InstallEngine::RunMsiSequence() noexcept {
    const bool is64 = scanNativeX64_;
    const bool ru = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN;
    struct Step {
        std::wstring msiFile;
        std::wstring dir;
        std::wstring label;
    };
    // Only the checked products are touched. An installed product that was
    // left unchecked is kept as it is: Setup never removes products.
    std::vector<Step> steps;
    if (is64 && selection_.platform64)
        steps.push_back({L"Glass-x64.msi", x64Dir_, labels_.platform64});
    if (selection_.platform32)
        steps.push_back({L"Glass-x86.msi", x86Dir_, labels_.platform32});
    if (selection_.secureUxTheme || selection_.secureUxThemePackageOnly)
        steps.push_back({is64 ? L"SecureUxTheme-x64.msi" : L"SecureUxTheme-x86.msi",
                         is64 ? x64Dir_ : x86Dir_, labels_.secureUxTheme});
    // The selected GlassSettings MSI has already run before monitoring.
    if (is64 && selection_.openGlass)
        steps.push_back({L"OpenGlass-x64.msi", openGlassDir_, labels_.openGlass});

    constexpr int kProgressStart = 2200;
    constexpr int kProgressEnd = 9000;
    const int span = steps.empty() ? 0 : (kProgressEnd - kProgressStart) / static_cast<int>(steps.size());
    int base = kProgressStart;
    for (const auto& step : steps) {
        if (IsCancelRequested()) return false;
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
            if (!RecheckSnapshottedProduct(legacy)) return false;
            GlassInstallRollback::NoteModified(rollbackSession_,legacy);
        }
        wchar_t msiPath[MAX_PATH]{};
        if (!PathCombineW(msiPath, tempDir_.c_str(), step.msiFile.c_str())) return false;
        if (!RunOneMsi(msiPath, step.dir, step.label, base, span)) return false;
        base += span;

        bool pathWritten=true;
        if (step.msiFile == L"Glass-x64.msi")
            pathWritten=GlassInstallRollback::PreservePlatformUserPath(rollbackSession_,true) &&
                GlassInstallUtil::WritePlatformInstallPath(true, x64Dir_);
        else if (step.msiFile == L"Glass-x86.msi")
            pathWritten=GlassInstallRollback::PreservePlatformUserPath(rollbackSession_,false) &&
                GlassInstallUtil::WritePlatformInstallPath(false, x86Dir_);
        else if (step.msiFile == L"OpenGlass-x64.msi")
            pathWritten=GlassInstallUtil::WriteOpenGlassInstallPath(x64Dir_, openGlassDir_);
        if (!pathWritten) {
            const DWORD error=GetLastError();
            InstallLog::LineFormat(L"Could not write installation path for %s (error %lu)",step.msiFile.c_str(),error);
            SetFailure(ru ? L"Не удалось сохранить путь установки в реестре."
                          : L"Could not save the installation path in the registry.",error);
            return false;
        }
    }

    // Do not replace an existing customised shortcut without a backup.
    if (selection_.glassSettings && !prior_.shortcutExisted) {
        const std::wstring& gsRoot = is64 ? x64Dir_ : x86Dir_;
        const std::wstring gsDir = GlassInstallUtil::TrimTrailingSlashes(gsRoot) + L"\\GlassSettings";
        (void)GlassInstallUtil::CreateStartMenuShortcut(gsDir + L"\\GlassSettings.exe", gsDir);
    }

    InstallLog::Line(L"InstallEngine: selected MSI products applied");
    return true;
}

namespace {

std::wstring JoinWarningDetail(const std::wstring& hint, const std::wstring& technical) {
    if (technical.empty()) return hint;
    if (hint.empty()) return technical;
    return hint + L" (" + technical + L")";
}

} // namespace

void InstallEngine::AddFinishWarning(const std::wstring& title, const std::wstring& detail) noexcept {
    InstallLog::LineFormat(L"Finish note: %s%s%s", title.c_str(), detail.empty() ? L"" : L" | ", detail.c_str());
    try {
        FinishWarning warning{title, detail};
        AcquireSRWLockExclusive(&statusLock_);
        try { finishWarnings_.push_back(std::move(warning)); } catch (...) {}
        ReleaseSRWLockExclusive(&statusLock_);
    } catch (...) {
        // The note is already in the log; the finish page simply shows fewer lines.
    }
}

std::vector<InstallEngine::FinishWarning> InstallEngine::FinishWarnings() const noexcept {
    std::vector<FinishWarning> copy;
    AcquireSRWLockShared(&statusLock_);
    try { copy = finishWarnings_; } catch (...) { copy.clear(); }
    ReleaseSRWLockShared(&statusLock_);
    return copy;
}

// Per-user defaults of the platform (AttachList/BlockList). Written for the
// user running Setup, before the platform starts and reads them. Existing
// lists are kept; only missing or empty lists are filled, so an update never
// overwrites the user's edits. A failure here is a finish note, not a reason
// to roll back a complete installation.
void InstallEngine::SeedUserDefaults() noexcept {
    if (!GlassUserDefaults::SeedProcessLists(userDefaults_)) {
        InstallLog::LineFormat(L"User defaults: seeding failed: %s", userDefaults_.detail.c_str());
        AddFinishWarning(labels_.warnUserDefaults.empty()
            ? L"The default process lists (AttachList/BlockList) could not be written." : labels_.warnUserDefaults,
            userDefaults_.detail);
        return;
    }
    InstallLog::LineFormat(L"User defaults: AttachList %s (%u entries), BlockList %s (%u entries)",
        userDefaults_.attachListCreated ? L"seeded" : L"kept", userDefaults_.attachWritten,
        userDefaults_.blockListCreated ? L"seeded" : L"kept", userDefaults_.blockWritten);
}

// Phase B. Everything here runs before the first product start, so the
// rollback snapshot is still authoritative and any failure is fatal.
bool InstallEngine::RegisterSelectedProducts() noexcept {
    const bool is64 = scanNativeX64_;
    const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;

    SetStatus(labels_.finalize, 9200);
    SeedUserDefaults();

    auto registerArch = [this, ru](GP::PlatformArch arch, const std::wstring& dir, bool taskExistedBefore) -> bool {
        if (!taskExistedBefore) {
            if (!GlassInstallHost::EnsureGlassAutorunTask(arch, dir, true, true)) {
                InstallLog::LineFormat(L"Autostart: failed to register task for arch %d", static_cast<int>(arch));
                SetFailure(labels_.autorunTaskFailed.empty()
                    ? L"Could not register the platform autostart task." : labels_.autorunTaskFailed, 0);
                return false;
            }
            if (arch==GP::PlatformArch::X64) x64TaskCreatedBySetup_=true;
            else x86TaskCreatedBySetup_=true;
        }
        // Every selected platform is started after the installation, whatever
        // its state was before Setup. The task that is about to be started must
        // still be ours while a rollback can remove it; the check is repeated
        // right before the start.
        if (GP::IsGlassPlatformRunning(arch)) return true;
        std::wstring error;
        if (!GlassInstallRemoval::ValidatePlatformTask(arch,dir,true,false,ru,error)) {
            InstallLog::LineFormat(L"Autostart: refusing changed task for arch %d: %s",static_cast<int>(arch),error.c_str());
            SetFailure(error,0);
            return false;
        }
        return true;
    };

    if (is64 && selection_.platform64 && !registerArch(GP::PlatformArch::X64, x64Dir_, prior_.x64TaskExisted))
        return false;
    if (selection_.platform32 && !registerArch(GP::PlatformArch::X86, x86Dir_, prior_.x86TaskExisted))
        return false;

    if (is64 && selection_.openGlass) {
        if (!openGlassStartup_.valid) {
            SetFailure(L"OpenGlass startup state was not captured; no automatic launch was attempted.",0);
            return false;
        }
        if (!prior_.ogServiceExisted && !prior_.ogLegacyTaskExisted) {
            // Files without orchestration may have been run manually. Do not
            // introduce automatic startup for that existing installation.
            const DWORD startType=openGlassStartup_.filesPresent ? SERVICE_DEMAND_START : SERVICE_AUTO_START;
            if (!GlassInstallHost::EnsureOpenGlassService(openGlassDir_, startType)) {
                SetFailure(labels_.ogServiceFailed.empty()
                    ? L"Could not register the OpenGlassHost service." : labels_.ogServiceFailed,0);
                return false;
            }
            ogServiceCreatedBySetup_=true;
        }
        if (GlassProducts::ShouldStartOpenGlass(openGlassStartup_)) {
            std::wstring pathError;
            if (!GlassInstallRemoval::ValidateOpenGlassPaths(openGlassDir_,&cancelRequested_,ru,pathError)) {
                if (!IsCancelRequested()) SetFailure(pathError,0);
                return false;
            }
        }
    }
    return true;
}

// Phase C. The first StartPlatform call is the commit point of the whole
// installation: the files, tasks and service are final, so nothing in this
// routine may fail the install. Each problem is logged and shown as a note.
void InstallEngine::LaunchSelectedProducts() noexcept {
    const bool is64 = scanNativeX64_;
    const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
#ifdef GLASS_EMBED_MSI
    const GlassMaintenanceCache::Blob cacheBlobs[]={
        {IDR_MSI_X64,L"Glass-x64.msi"},{IDR_MSI_X86,L"Glass-x86.msi"},
        {IDR_MSI_GS_X64,L"GlassSettings-x64.msi"},{IDR_MSI_GS_X86,L"GlassSettings-x86.msi"},
        {IDR_MSI_SUT_X64,L"SecureUxTheme-x64.msi"},{IDR_MSI_SUT_X86,L"SecureUxTheme-x86.msi"},
        {IDR_MSI_OPENGLASS,L"OpenGlass-x64.msi"}};
    if(!GlassMaintenanceCache::Publish(instance_,cacheBlobs,std::size(cacheBlobs),IDR_MAINTENANCE_UNINS,IDR_UNLOCKER_X86))
        AddFinishWarning(ru?L"Оболочка обслуживания сохранена не полностью":L"Maintenance cache publication is incomplete",ru?L"Сохраните исходный установщик и журнал; повторите обслуживание.":L"Keep the original installer and log; retry maintenance.");
    const wchar_t* products[]={GlassInstallMsi::kProductPlatformX64,GlassInstallMsi::kProductPlatformX86,
        GlassInstallMsi::kProductSettingsX64,GlassInstallMsi::kProductSettingsX86,GlassInstallMsi::kProductOpenGlass,
        GlassInstallMsi::kProductSecureUxThemeX64,GlassInstallMsi::kProductSecureUxThemeX86};
    for(const auto* code:products)if(GlassInstallMsi::IsProductInstalled(code)) {
        const bool product32=_wcsicmp(code,GlassInstallMsi::kProductPlatformX86)==0||_wcsicmp(code,GlassInstallMsi::kProductSettingsX86)==0||_wcsicmp(code,GlassInstallMsi::kProductSecureUxThemeX86)==0;
        GlassInstallMsi::PatchArpUninstallString(code,_wcsicmp(code,GlassInstallMsi::kProductOpenGlass)==0?openGlassDir_:product32?x86Dir_:x64Dir_);
    }
#endif

    if (selection_.secureUxTheme) {
        SetStatus(labels_.secureUxTheme.empty()?L"SecureUxTheme":labels_.secureUxTheme,9300);
        GlassInstallUtil::SecureUxThemeActivateRun sutRun;
        std::wstring sutError;
        if (!GlassInstallUtil::InvokeSecureUxThemeActivate(x64Dir_,x86Dir_,&sutRun,&sutError)) {
            const std::wstring hint=labels_.warnSecureUxThemeHint.empty()?sutError:labels_.warnSecureUxThemeHint+L" "+sutError;
            AddFinishWarning(labels_.warnSecureUxTheme.empty()?L"SecureUxTheme could not be enabled.":labels_.warnSecureUxTheme,hint);
        } else if (sutRun.skippedExternalInstall) {
            AddFinishWarning(labels_.warnSecureUxTheme.empty()?L"SecureUxTheme is managed by another installation.":labels_.warnSecureUxTheme,
                labels_.warnSecureUxThemeHint.empty()?L"The external installation was left untouched.":labels_.warnSecureUxThemeHint);
        } else if (sutRun.rebootRequired) {
            AddFinishWarning(labels_.warnSecureUxThemeRestart.empty()?L"Restart Windows to finish enabling SecureUxTheme.":labels_.warnSecureUxThemeRestart,L"");
        }
    }

    // Policy: a selected platform is always started, whether or not it was
    // running (or its task was enabled) when Setup began. StartPlatform
    // re-enables a disabled task itself.
    auto startArch = [this, ru](
        GP::PlatformArch arch,
        const std::wstring& dir,
        bool taskExistedBefore,
        bool taskEnabledBefore,
        bool wasRunningBefore,
        const std::wstring& label,
        int pct,
        const std::wstring& warnStart,
        const std::wstring& warnTask) {
        InstallLog::LineFormat(L"Autostart: arch %d before setup: task=%d enabled=%d running=%d",
            static_cast<int>(arch), taskExistedBefore ? 1 : 0, taskEnabledBefore ? 1 : 0, wasRunningBefore ? 1 : 0);
        SetStatus(label, pct);
        if (!GP::IsGlassPlatformRunning(arch)) {
            // Repeat the full action check immediately before starting a task.
            std::wstring error;
            if (!GlassInstallRemoval::ValidatePlatformTask(arch,dir,true,false,ru,error)) {
                InstallLog::LineFormat(L"Autostart: refusing changed task for arch %d: %s",static_cast<int>(arch),error.c_str());
                AddFinishWarning(warnTask.empty()
                    ? L"The platform autostart task changed during setup; the platform was not started." : warnTask, error);
                return;
            }
            InstallLog::LineFormat(L"Autostart: StartPlatform arch %d", static_cast<int>(arch));
            GP::StartPlatform(arch);
            const ULONGLONG start = GetTickCount64();
            while (!GP::IsGlassPlatformRunning(arch)) {
                if (GetTickCount64() - start >= GP::kDefaultPlatformShutdownWaitMs) {
                    InstallLog::LineFormat(L"Autostart: platform did not report running within %lu ms, arch %d",
                        static_cast<unsigned long>(GP::kDefaultPlatformShutdownWaitMs), static_cast<int>(arch));
                    AddFinishWarning(warnStart.empty()
                        ? L"The platform did not start within the time limit." : warnStart, labels_.warnPlatformStartHint);
                    return;
                }
                Sleep(200);
            }
        }
        InstallLog::LineFormat(L"Autostart: platform running arch %d", static_cast<int>(arch));
    };

    if (is64 && selection_.platform64)
        startArch(GP::PlatformArch::X64, x64Dir_, prior_.x64TaskExisted, prior_.x64TaskEnabled, prior_.x64WasRunning,
            labels_.start64, 9500, labels_.warnPlatform64Start, labels_.warnPlatform64Task);
    if (selection_.platform32)
        startArch(GP::PlatformArch::X86, x86Dir_, prior_.x86TaskExisted, prior_.x86TaskEnabled, prior_.x86WasRunning,
            labels_.start86, 9750, labels_.warnPlatform32Start, labels_.warnPlatform32Task);

    if (is64 && selection_.openGlass) {
        SetStatus(labels_.startOg, 9900);
        const bool shouldStartOg = GlassProducts::ShouldStartOpenGlass(openGlassStartup_);
        InstallLog::LineFormat(L"Autostart: OpenGlass files-at-start=%d running-at-start=%d start=%d",
            openGlassStartup_.filesPresent ? 1 : 0, openGlassStartup_.wasRunning ? 1 : 0, shouldStartOg ? 1 : 0);
        const std::wstring warnOg = labels_.warnOpenGlassStart.empty() ? L"OpenGlass did not start." : labels_.warnOpenGlassStart;
        std::wstring pathError;
        if (shouldStartOg && !GlassInstallRemoval::ValidateOpenGlassPaths(openGlassDir_,nullptr,ru,pathError)) {
            // Re-checked after the commit point: a changed backend is not started, but
            // the installation is complete, so this is a note rather than a failure.
            AddFinishWarning(warnOg, pathError);
        } else if (shouldStartOg) {
            bool startupAccepted=false;
            const bool serviceBackend=prior_.ogServiceExisted || ogServiceCreatedBySetup_;
            if (serviceBackend && prior_.ogServiceExisted && prior_.ogServiceStartType==SERVICE_DISABLED) {
                ogServiceStartTypeMayNeedRestore_=true;
                startupAccepted=GlassInstallHost::StartOpenGlassServicePreservingStartType(prior_.ogServiceStartType);
                if (startupAccepted) ogServiceStartTypeMayNeedRestore_=false;
            } else if (serviceBackend) {
                startupAccepted=GlassInstallHost::StartOpenGlassService();
            } else {
                // The COM runner refuses a disabled or concurrently changed
                // task and never enables it as a side effect.
                startupAccepted=GlassInstallUtil::RunEnabledOpenGlassTask();
            }
            if (!startupAccepted) {
                InstallLog::Line(L"Autostart: OpenGlassHost start request was refused");
                AddFinishWarning(warnOg, JoinWarningDetail(labels_.warnOpenGlassHint,
                    serviceBackend ? L"service start request failed" : L"legacy task could not be run"));
            } else {
                bool running=false, known=true;
                const ULONGLONG start = GetTickCount64();
                do {
                    if (serviceBackend) running=GlassInstallUtil::ImageRunning(L"OpenGlassHost.exe");
                    else known=GlassInstallUtil::QueryOpenGlassRunning(running); // legacy task need not use OpenGlassHost.exe
                    if (running || !known || GetTickCount64()-start>=15000) break;
                    Sleep(250);
                } while (true);
                InstallLog::LineFormat(L"Autostart: OpenGlass running=%d known=%d",running?1:0,known?1:0);
                if (!running || !known)
                    AddFinishWarning(warnOg, JoinWarningDetail(labels_.warnOpenGlassHint,
                        known ? L"OpenGlassHost did not report running within 15 s" : L"running state could not be queried"));
            }
        }
    }

    if (!RestorePriorHosts(true)) {
        AddFinishWarning(labels_.warnPriorHosts.empty()
            ? L"A previously running component could not be restarted." : labels_.warnPriorHosts, labels_.warnPriorHostsHint);
    }
    // Windows 11: the developer certificate (signer of GlassShellSparse.msix) is
    // added to LocalMachine\TrustedPeople only when the user chose so on the
    // certificate page; it is taken from the msix that was just installed and
    // trusted before the menu is registered. Declined: nothing is trusted and
    // the finish page explains the classic menu and where the public copy lies
    // ({dir}\DeveloperCodeSigning.cer + .txt). The uninstaller never removes it.
    const auto expandInstallDir=[this](std::wstring text) {
        const std::wstring dir=GlassInstallUtil::TrimTrailingSlashes(x64Dir_.empty() ? x86Dir_ : x64Dir_);
        for (size_t at=text.find(L"{dir}"); at!=std::wstring::npos; at=text.find(L"{dir}",at+dir.size()))
            text.replace(at,5,dir);
        return text;
    };
    if (selection_.trustSigner) {
        std::wstring trustDetail;
        if (GlassInstallUtil::InvokeShellSignerTrust(x64Dir_,x86Dir_,&trustDetail)) {
            InstallLog::Line(L"Certificate: developer certificate trusted (LocalMachine\\TrustedPeople) by user choice");
        } else {
            InstallLog::LineFormat(L"Certificate: trust failed: %s", trustDetail.c_str());
            AddFinishWarning(labels_.warnCertInstall.empty()
                ? L"The developer certificate could not be installed; the Windows 11 compact menu stays unavailable."
                : labels_.warnCertInstall, JoinWarningDetail(expandInstallDir(labels_.warnCertInstallHint), trustDetail));
        }
    } else {
        InstallLog::Line(L"Certificate: not requested (page not shown or declined); nothing trusted");
    }
    // Register by default on both fresh installs and reinstalls. The helper
    // preserves an existing registration and reports whether it changed one.
    // On Windows 11 a package rejected for this user (no trusted signer) makes
    // the helper register the classic Win32 menu instead (regsvr32 of
    // GlassShellExt.dll); the install is complete and the finish page explains
    // the reduced menu.
    bool shellConflict=false;
    DWORD shellClassicReason=0;
    std::wstring shellDetail;
    if (!GlassInstallUtil::InvokeShellExtRegistration(true,x64Dir_,x86Dir_,&shellRegisteredBySetup_,&shellConflict,&shellDetail,&shellClassicReason)) {
        if (shellConflict) {
            AddFinishWarning(labels_.warnShellConflict.empty()
                ? L"An existing or unverifiable shell registration was preserved without changes." : labels_.warnShellConflict,
                shellDetail);
        } else {
            AddFinishWarning(labels_.shellRegisterFailed.empty()
                ? L"Could not register the Windows shell extension." : labels_.shellRegisterFailed,
                JoinWarningDetail(labels_.warnShellHint, shellDetail));
        }
    } else if (shellClassicReason!=0) {
        constexpr DWORD kReasonSignerNotTrusted=8; // OpMessageId::SparseSignerNotTrusted
        const bool certificate=shellClassicReason==kReasonSignerNotTrusted;
        std::wstring title=certificate ? labels_.warnShellClassicCert : labels_.warnShellClassicOther;
        if (title.empty()) {
            title=certificate
                ? L"The Windows 11 compact menu is unavailable: the developer certificate (CN=Stanislav K.) is not trusted on this computer. The Glass entry was registered the classic way (\"Show more options\")."
                : L"The Windows 11 compact menu is unavailable; the Glass entry was registered the classic way (\"Show more options\").";
        }
        std::wstring hint;
        if (certificate) hint=expandInstallDir(labels_.warnShellClassicHint);
        AddFinishWarning(title, JoinWarningDetail(hint, GlassInstallUtil::DescribeShellClassicFallback(shellClassicReason)));
    }
    SetStatus(labels_.finalize, 9950);
}

bool InstallEngine::DetermineCleanInstall() noexcept {
    const ArchPresence x64 = ClassifyArch(GP::PlatformArch::X64,x64Dir_);
    const ArchPresence x86 = ClassifyArch(GP::PlatformArch::X86,x86Dir_);
    const bool gsInstalled = GlassInstallMsi::IsProductInstalled(GlassInstallMsi::kProductSettingsX64)
        || GlassInstallMsi::IsProductInstalled(GlassInstallMsi::kProductSettingsX86);
    const bool sutInstalled = GlassInstallMsi::IsProductInstalled(GlassInstallMsi::kProductSecureUxThemeX64)
        || GlassInstallMsi::IsProductInstalled(GlassInstallMsi::kProductSecureUxThemeX86);
    const bool openGlass = OpenGlassPresent(openGlassDir_) || prior_.ogLegacyTaskExisted;
    cleanInstall_ = x64 == ArchPresence::Absent && x86 == ArchPresence::Absent
        && !gsInstalled && !sutInstalled && !openGlass && !prior_.x64TaskExisted && !prior_.x86TaskExisted
        && !prior_.ogServiceExisted && !prior_.ogLegacyTaskExisted && !prior_.ogWasRunning;
    // Setup is x86; on x64 Windows the embedded x64 helper is required even
    // when product registration/files are missing or the install looks clean.
    // Failure to determine WOW64 must not silently skip the x64 monitor.
#if defined(_WIN64)
    scanNativeX64_ = true;
#else
    BOOL selfWow = FALSE;
    if (!IsWow64Process(GetCurrentProcess(), &selfWow)) {
        const DWORD error = GetLastError();
        SetFailure(labels_.nativeScanFailed.empty() ? L"Could not determine the system architecture."
                                                    : labels_.nativeScanFailed, error);
        return false;
    }
    scanNativeX64_ = selfWow != FALSE;
#endif
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
        } else if (rollbackSession_.productPathUnverified) {
            const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
            const bool directory=rollbackSession_.productPathCode==L"<installation directory>";
            std::wstring message=directory
                ? (ru ? L"Каталог установки или его содержимое не удалось безопасно проверить: обнаружена ссылка или недоступен путь. Установка остановлена до изменений. Сохраните журнал и обратитесь за помощью; не удаляйте файлы вручную."
                      : L"The installation directory or its contents could not be checked safely: a link or an inaccessible path was found. Setup stopped before making changes. Save the log and ask for help; do not delete files manually.")
                : (ru ? L"Не удалось безопасно проверить ранее зарегистрированный MSI: его файлы находятся вне определённого каталога установки либо неизвестны пути, контекст или связанный продукт. Установка остановлена до изменений. Сохраните журнал и обратитесь за помощью; не удаляйте файлы вручную."
                      : L"A previously registered MSI cannot be verified safely: its files are outside the installation directory, or its paths, context or related products are unknown. Setup stopped before making changes. Save the log and ask for help; do not delete files manually.");
            if (!directory) message+=L"\r\n"+rollbackSession_.productPathCode;
            if (!rollbackSession_.productPathDetail.empty()) message+=L"\r\n"+rollbackSession_.productPathDetail;
            SetFailure(message,ERROR_INVALID_DATA);
        } else {
            SetFailure(labels_.snapshotFailed.empty()
                ? L"Could not save a copy of the current installation. No files were changed."
                : labels_.snapshotFailed, rollbackSession_.copyError);
        }
    }
    SetStatus(labels_.backup, 800);
    return ok;
}

bool InstallEngine::StopHostsForRollback() noexcept {
    const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
    std::wstring error;
    for (const auto arch:{GP::PlatformArch::X64,GP::PlatformArch::X86}) {
        bool running=false;
        if (!QueryPlatformMutex(arch,running)) return false;
        GP::InvalidateGlassAutorunTaskCache();
        const bool exists=GP::GlassAutorunTaskExists(arch);
        const auto& root=arch==GP::PlatformArch::X64?x64Dir_:x86Dir_;
        if (!GlassInstallRemoval::ValidatePlatformTask(arch,root,exists,running,ru,error)) {
            InstallLog::LineFormat(L"Rollback refused to stop changed platform: %s",error.c_str());return false;
        }
    }
    if (scanNativeX64_ && !GlassInstallRemoval::ValidateOpenGlassPaths(openGlassDir_,nullptr,ru,error)) {
        InstallLog::LineFormat(L"Rollback refused to stop changed OpenGlass backend: %s",error.c_str());return false;
    }
    return GlassInstallHost::StopAllForUninstall(x64Dir_,x86Dir_,true,true,scanNativeX64_);
}

bool InstallEngine::RemoveCreatedHostsForRollback() noexcept {
    const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
    std::wstring error;
    GP::InvalidateGlassAutorunTaskCache();
    if ((x64TaskCreatedBySetup_ && !GlassInstallRemoval::ValidatePlatformTask(
            GP::PlatformArch::X64,x64Dir_,true,false,ru,error)) ||
        (x86TaskCreatedBySetup_ && !GlassInstallRemoval::ValidatePlatformTask(
            GP::PlatformArch::X86,x86Dir_,true,false,ru,error)) ||
        (ogServiceCreatedBySetup_ && !GlassInstallRemoval::ValidateOpenGlassPaths(
            openGlassDir_,nullptr,ru,error))) {
        InstallLog::LineFormat(L"Rollback refused to remove changed task or service: %s",error.c_str());
        return false;
    }
    if ((x64TaskCreatedBySetup_ || x86TaskCreatedBySetup_) &&
        !GlassInstallHost::RemovePlatformAutorunTasks(x64TaskCreatedBySetup_,x86TaskCreatedBySetup_)) return false;
    if (ogServiceCreatedBySetup_ && !GlassInstallHost::RemoveOpenGlassServiceAndTasks(false)) return false;
    return true;
}

bool InstallEngine::PerformRollback() noexcept {
    // The stop/uninstall stages have no reliable percentage. Publish marquee
    // before exposing the reverse flag so the UI never uses stale MSI progress.
    SetStatus(labels_.rollback.empty() ? L"…" : labels_.rollback, -1);
    progressReverse_.store(true,std::memory_order_release);
    const bool stopped=StopHostsForRollback();
    bool lifecycleOk=stopped;
    if (stopped) {
        if (shellRegisteredBySetup_ && !GlassInstallUtil::InvokeShellExtRegistration(false,x64Dir_,x86Dir_)) lifecycleOk=false;
        if (lifecycleOk && !RemoveCreatedHostsForRollback()) lifecycleOk=false;
        if (lifecycleOk && !prior_.shortcutExisted) GlassInstallUtil::DeleteStartMenuShortcut();
    }
    const bool filesOk = lifecycleOk && GlassInstallRollback::ExecuteRollback(
        rollbackSession_, [this](int units) {
            SetStatus(labels_.rollback, WizardProgress::RollbackRemaining(units));
        }, nonSettingsChangesAttempted_ ? GlassInstallRollback::RestoreScope::AllProducts
            : (scanNativeX64_ ? GlassInstallRollback::RestoreScope::SettingsX64
                              : GlassInstallRollback::RestoreScope::SettingsX86));
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
    const bool stopped=StopHostsForRollback();
    bool ok = stopped && (!shellRegisteredBySetup_ || GlassInstallUtil::InvokeShellExtRegistration(false,x64Dir_,x86Dir_));
    const size_t productCount=rollbackSession_.installedThisSession.size();
    size_t productsProcessed=0;
    for (auto it = rollbackSession_.installedThisSession.rbegin();
         it != rollbackSession_.installedThisSession.rend(); ++it) {
        if (ok && !GlassInstallRollback::UninstallSessionProductSafely(rollbackSession_,it->c_str())) ok = false;
        ++productsProcessed;
        // Leave room for registry, services and tasks; final success is reported
        // separately after all cleanup and host restoration have finished.
        if (productCount) SetStatus(labels_.rollback,WizardProgress::kMaxUnits-
            static_cast<int>(productsProcessed*8000/productCount));
    }
    if (ok) {
        // A failed MSI can leave an unexpected registration that was never
        // recorded as successfully installed. Do not call a partial cleanup a
        // successful clean rollback or guess which product owns it.
        const wchar_t* known[]={GlassInstallMsi::kProductPlatformX64,GlassInstallMsi::kProductPlatformX86,
            GlassInstallMsi::kProductSettingsX64,GlassInstallMsi::kProductSettingsX86,
            GlassInstallMsi::kProductSecureUxThemeX64,GlassInstallMsi::kProductSecureUxThemeX86,
            GlassInstallMsi::kProductOpenGlass,GlassInstallMsi::kLegacyPlatformX64,
            GlassInstallMsi::kLegacyPlatformX86,GlassInstallMsi::kLegacyOpenGlass};
        for (const auto* code:known) {
            std::wstring detail;
            if (GlassInstallMsi::CheckProductContexts(code,detail)!=GlassInstallMsi::ProductPathCheck::Safe ||
                GlassInstallMsi::IsProductInstalled(code)) {
                InstallLog::LineFormat(L"Clean rollback retained unexpected MSI registration %s (%s)",code,detail.c_str());
                ok=false;break;
            }
        }
    }
    if (ok) {
        // MSI removes its own files. Only empty directories may be removed;
        // leave the remaining state untouched if any MSI was unsafe to remove.
        if (!RemoveCreatedHostsForRollback()) ok=false;
        if (ok) {
            if (!openGlassDir_.empty()) RemoveDirectoryW(openGlassDir_.c_str());
            if (!x64Dir_.empty()) RemoveDirectoryW(x64Dir_.c_str());
            if (!x86Dir_.empty()) RemoveDirectoryW(x86Dir_.c_str());
            if (!prior_.shortcutExisted) GlassInstallUtil::DeleteStartMenuShortcut();
            if (!GlassInstallRollback::RestoreRegistryValues(rollbackSession_)) ok=false;
        }
    } else InstallLog::Line(L"Clean rollback left remaining state untouched; backup retained");

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
    SetStatus(labels_.purge.empty() ? L"…" : labels_.purge, 2000);
    const bool is64 = scanNativeX64_;
    const wchar_t* installedSettings = is64 ? GlassInstallMsi::kProductSettingsX64 : GlassInstallMsi::kProductSettingsX86;
    const wchar_t* codes[] = {
        GlassInstallMsi::kProductSecureUxThemeX64,
        GlassInstallMsi::kProductSecureUxThemeX86,
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
        // This Settings MSI was installed before monitoring. A clean reinstall
        // must not immediately uninstall it again; the other architecture, if
        // present, is still purged normally.
        if (selection_.glassSettings && _wcsicmp(code, installedSettings) == 0) continue;
        if (!GlassInstallMsi::IsProductInstalled(code)) continue;
        if (IsCancelRequested()) return false;
        if (!ProductWasSnapshotted(rollbackSession_,code)) {
            const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
            SetFailure(ru ? L"Состав установленных продуктов изменился после создания копии. Повторите установку."
                          : L"Installed products changed after the snapshot. Run Setup again.", 0);
            return false;
        }
        if (!RecheckSnapshottedProduct(code)) return false;
        InstallLog::LineFormat(L"Purge uninstall %s", code);
        GlassInstallRollback::NoteModified(rollbackSession_,code);
        GlassInstallMsi::ProgressSink sink;
        sink.label=labels_.purge; sink.base=2000; sink.span=0;
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
    if ((is64 && !GlassInstallUtil::ClearPlatformInstallPath(true)) ||
        !GlassInstallUtil::ClearPlatformInstallPath(false) ||
        (is64 && !GlassInstallUtil::ClearOpenGlassInstallPath())) {
        SetFailure(labels_.purgeFailed.empty() ? L"Could not clear installation paths." : labels_.purgeFailed, GetLastError());
        return false;
    }
    purgeCompleted_ = true;
    SetStatus(labels_.purge, 2200);
    InstallLog::Line(L"Purge MSI registrations completed; user files retained");
    return true;
}
