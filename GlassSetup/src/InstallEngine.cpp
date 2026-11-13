#include "InstallSupport/GlassMaintenanceLaunch.h"
#include "InstallSupport/GlassOpenGlassRelease.h"
#include "InstallSupport/GlassOpenGlassInstall.h"
#include "InstallSupport/GlassOpenGlassService.h"
#include "InstallSupport/GlassMaintenanceCache.h"
#include "InstallSupport/GlassPackageAcquisition.h"
#include "InstallEngine.h"
#include "InstallSupport/GlassMaintenanceLifetime.h"
#include "GlassMaintenanceExecutor.h"
#include "InstallSupport/GlassProductInspect.h"
#include "InstallSupport/GlassPackageRules.h"
#include "InstallSupport/GlassRegisteredMsi.h"
#include <SecureUxThemeInventory.h>
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
#include <utility>
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
    if(!preserveOfficialResourcesSnapshot_)GlassInstallRollback::CleanupSnapshot(officialResourcesRollback_);
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
    if(GlassProcedurePlan::ModuleScan(procedureMask_)) {
        GP::InvalidateGlassAutorunTaskCache();
        prior_.x64TaskExisted = GlassProcedurePlan::Platform64(procedureMask_) && GP::GlassAutorunTaskExists(GP::PlatformArch::X64);
        prior_.x64TaskEnabled = GlassProcedurePlan::Platform64(procedureMask_) && GP::QueryGlassAutorunUiState(GP::PlatformArch::X64) != GP::AutorunUiState::Disabled;
        prior_.x86TaskExisted = GlassProcedurePlan::Platform32(procedureMask_) && GP::GlassAutorunTaskExists(GP::PlatformArch::X86);
        prior_.x86TaskEnabled = GlassProcedurePlan::Platform32(procedureMask_) && GP::QueryGlassAutorunUiState(GP::PlatformArch::X86) != GP::AutorunUiState::Disabled;
        if ((GlassProcedurePlan::Platform64(procedureMask_) && !QueryPlatformMutex(GP::PlatformArch::X64, prior_.x64WasRunning)) ||
            (GlassProcedurePlan::Platform32(procedureMask_) && !QueryPlatformMutex(GP::PlatformArch::X86, prior_.x86WasRunning))) {
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
        if ((GlassProcedurePlan::Platform64(procedureMask_) && !checkTask(GP::PlatformArch::X64,prior_.x64TaskExisted,prior_.x64WasRunning,x64Dir_)) ||
            (GlassProcedurePlan::Platform32(procedureMask_) && !checkTask(GP::PlatformArch::X86,prior_.x86TaskExisted,prior_.x86WasRunning,x86Dir_))) return false;
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

    }
    if(GlassProcedurePlan::OpenGlass(procedureMask_)) {
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
        InstallLog::LineFormat(L"Rollback host restore: arch=%s stopAttempted=%d",GP::GlassAutorunTaskName(arch),stopAttempted?1:0);
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
    const auto restorePlatforms=[&]() {
    if (!keptOnly || (!selection_.platform64 && !selection_.Removes(0)))
        if (!restoreArch(GP::PlatformArch::X64,x64Dir_,prior_.x64TaskExisted,prior_.x64TaskEnabled,prior_.x64WasRunning,x64StopAttempted_)) ok=false;
    if (!keptOnly || (!selection_.platform32 && !selection_.Removes(1)))
        if (!restoreArch(GP::PlatformArch::X86,x86Dir_,prior_.x86TaskExisted,prior_.x86TaskEnabled,prior_.x86WasRunning,x86StopAttempted_)) ok=false;
    };
    if (keptOnly && (selection_.openGlass||selection_.Removes(3))) {restorePlatforms();return ok;}
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
    if(ok&&prior_.ogWasRunning&&ogStopSignalled_)ok=EnsureOpenGlassBeforePlatform();
    if(ok)restorePlatforms(); // OG restoration must settle first.
    return ok;
}

// Transitional standalone vendor lane. No Glass snapshot, MSI journal, host
// stop/start, service/task registration, receipt or ARP rewriting is involved.
bool InstallEngine::RunOfficialOpenGlassOnly() noexcept {
    const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
    try {
        const bool removing=GlassProducts::StandaloneOpenGlassRemoval(selection_,forceClean_,GlassInstallUtil::IsOs64Bit());
        if(!removing&&!GlassProducts::StandaloneOpenGlassApply(selection_,forceClean_,GlassInstallUtil::IsOs64Bit())) {
            SetFailure(ru?L"Этот план OpenGlass пока не поддерживается: сочетание с удалением других продуктов, SecureUxTheme, очисткой или чистой переустановкой недоступно. Продукты не изменены.":
                L"This OpenGlass plan is not yet supported: combining with removal of other products, SecureUxTheme, cleanup or clean reinstall is unavailable. Products were not changed.",ERROR_NOT_SUPPORTED);
            return false;
        }
        return PrepareOfficialOpenGlass(removing)&&CommitOfficialOpenGlass(removing);
    }catch(...) {SetFailure(L"OpenGlass operation failed",ERROR_INSTALL_FAILURE);return false;}
}

bool InstallEngine::PrepareOfficialOpenGlass(bool removing) noexcept {
    const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
    try {
        if(MsiQueryProductStateW(GlassInstallMsi::kProductOpenGlass)!=INSTALLSTATE_UNKNOWN||
           MsiQueryProductStateW(GlassInstallMsi::kLegacyOpenGlass)!=INSTALLSTATE_UNKNOWN||
           GlassOpenGlassFiles::HasReceipt(openGlassDir_)) {
            SetFailure(ru?L"Обнаружена старая тестовая установка OpenGlass. Удалите её прежним деинсталлятором перед проверкой нового маршрута; миграции не выполняются.":
                L"A retired test OpenGlass installation was found. Remove it with its previous uninstaller before testing the new route; no migration is performed.",ERROR_INSTALL_PACKAGE_REJECTED);return false;
        }
        std::wstring serviceError;DWORD serviceCode=ERROR_SUCCESS;
        if(!GlassOpenGlassService::Check(false,serviceError,serviceCode)) {
            SetFailure(serviceError,serviceCode);return false;
        }
        auto& before=officialOpenGlassBefore_;std::wstring error;
        if(!GlassOpenGlassInstall::Query(before,error)){SetFailure(error,ERROR_INSTALL_PACKAGE_REJECTED);return false;}
        if(removing&&!before.installed) {
            SetFailure(ru?L"Штатная регистрация OpenGlass не найдена. Удаление не выполнялось.":
                L"Official OpenGlass registration is missing. No removal was performed.",ERROR_INSTALL_PACKAGE_REJECTED);return false;
        }
        if(removing&&!GlassOpenGlassPackage::ValidateUninstaller(before,error)) {
            SetFailure((ru?L"Штатный деинсталлятор OpenGlass не прошёл проверку доверия. Продукты не изменены. ":
                L"The official OpenGlass uninstaller failed trust verification. Products were not changed. ")+error,ERROR_INSTALL_FAILURE);
            return false;
        }
        if(removing&&!GlassInstallRemoval::ValidateOpenGlassPaths(before.directory,nullptr,ru,error)) {
            SetFailure((ru?L"Проверка службы OpenGlass отклонена до изменений продуктов. ":
                L"OpenGlass service validation failed before product changes. ")+error,ERROR_INSTALL_FAILURE);return false;
        }
        auto& target=officialOpenGlassTarget_;
        if(before.installed) {
            target=before.directory;
            if(!GlassMaintenanceCache::TrustedDirectory(target)||GlassOpenGlassFiles::HasReceipt(target)) {
                SetFailure(L"Official OpenGlass directory is untrusted or has a retired managed receipt",ERROR_INSTALL_PACKAGE_REJECTED);return false;
            }
        } else {
            std::wstring native,other;
            if(!GlassInstallUtil::ResolveFixedPlatformRoots(native,other)) {
                SetFailure(L"Cannot resolve native Program Files for OpenGlass",GetLastError());return false;
            }
            target=native+L"\\OpenGlass";
            // Resources/configuration may survive an official uninstall. An
            // existing safe directory is not product registration or a reason
            // to block Inno; never delete it or adopt its files as managed OG.
            const DWORD attributes=GetFileAttributesW(target.c_str());
            const DWORD pathError=attributes==INVALID_FILE_ATTRIBUTES?GetLastError():ERROR_SUCCESS;
            if(attributes!=INVALID_FILE_ATTRIBUTES) {
                if(!(attributes&FILE_ATTRIBUTE_DIRECTORY)||(attributes&FILE_ATTRIBUTE_REPARSE_POINT)||
                   !GlassInstallUtil::IsSafeInstallPath(target)||!GlassMaintenanceCache::TrustedDirectory(target)) {
                    SetFailure(ru?L"Существующий каталог OpenGlass небезопасен или недоступен. Продукты не изменены.":
                        L"The existing OpenGlass directory is unsafe or inaccessible. Products were not changed.",ERROR_INSTALL_PACKAGE_REJECTED);return false;
                }
                if(GlassOpenGlassFiles::HasReceipt(target)) {
                    SetFailure(ru?L"В каталоге OpenGlass найдена старая тестовая квитанция. Удалите тестовую установку прежним деинсталлятором; миграции не выполняются.":
                        L"A retired test receipt was found in the OpenGlass directory. Remove the test installation with its previous uninstaller; no migration is performed.",ERROR_INSTALL_PACKAGE_REJECTED);return false;
                }
            } else if(pathError!=ERROR_FILE_NOT_FOUND&&pathError!=ERROR_PATH_NOT_FOUND) {
                SetFailure(ru?L"Не удалось проверить каталог OpenGlass. Продукты не изменены.":
                    L"Could not inspect the OpenGlass directory. Products were not changed.",pathError);return false;
            }
        }
        if(!removing) {
            if(!EnsureTempWorkspace()){SetFailure(L"Cannot create protected OpenGlass workspace",GetLastError());return false;}
            SetStatus(ru?L"OpenGlass: подготовка официального установщика":L"OpenGlass: preparing official installer",0);
            if(!GlassOpenGlassPackage::PrepareInstallerForCurrentSystem(tempDir_,cancelRequested_,openGlassPackage_,error)) {
                if(IsCancelRequested())userCancelled_.store(true,std::memory_order_release);
                else SetFailure(error,ERROR_INSTALL_PACKAGE_REJECTED);
                return false;
            }
            if(before.installed&&GlassOpenGlassRelease::PayloadUpdateAvailable(openGlassPackage_.version,before.version)) {
                SetFailure(L"OpenGlass downgrade refused",ERROR_INSTALL_PACKAGE_REJECTED);return false;
            }
        }
        return true;
    }catch(...) {SetFailure(L"OpenGlass preparation failed; no vendor execution",ERROR_INSTALL_FAILURE);return false;}
}

bool InstallEngine::CommitOfficialOpenGlass(bool removing) noexcept {
    const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
    bool attempted=false;std::wstring error;
    try {
        // Use the same lock/gate as RequestCancel: the vendor transaction is
        // non-cancellable once this boundary has been crossed.
        for(;;) {
            if(decisionGate_)WaitForSingleObject(decisionGate_,INFINITE);
            AcquireSRWLockExclusive(&statusLock_);
            if(cancelRequested_.load(std::memory_order_acquire)) {
                ReleaseSRWLockExclusive(&statusLock_);userCancelled_.store(true,std::memory_order_release);return false;
            }
            const bool ready=!decisionGate_||WaitForSingleObject(decisionGate_,0)==WAIT_OBJECT_0;
            if(ready)launchingPhase_.store(true,std::memory_order_release);
            ReleaseSRWLockExclusive(&statusLock_);
            if(ready)break;
        }
        GlassOpenGlassInstall::Record current;
        if(!GlassOpenGlassInstall::Query(current,error)||!GlassOpenGlassInstall::Same(officialOpenGlassBefore_,current)) {
            SetFailure(error.empty()?L"OpenGlass registration changed before commit":error,ERROR_INSTALL_PACKAGE_REJECTED);return false;
        }
        SetStatus(removing?(ru?L"OpenGlass: штатное удаление":L"OpenGlass: official uninstall"):
            (ru?L"OpenGlass: официальный установщик":L"OpenGlass: official installer"),9000);
        DWORD code=ERROR_INSTALL_FAILURE;attempted=true;
        const bool ran=removing?GlassOpenGlassPackage::RunUninstaller(current,code,error,selection_.cleanupOpenGlassConfig):
            GlassOpenGlassPackage::RunInstaller(openGlassPackage_,officialOpenGlassTarget_,code,error);
        GlassOpenGlassInstall::Record after;std::wstring inventoryError;
        const bool inventoryKnown=GlassOpenGlassInstall::Query(after,inventoryError);
        const bool pathMatches=after.installed&&GlassInstallUtil::SameInstallPath(after.directory,officialOpenGlassTarget_);
        const bool versionMatches=after.installed&&GlassOpenGlassInstall::SameVersion(after.version,openGlassPackage_.version);
        const bool confirmed=inventoryKnown&&(removing?!after.installed:(pathMatches&&versionMatches));
        InstallLog::LineFormat(L"OpenGlass post-check: ran=%d exit=%lu removing=%d inventoryKnown=%d installed=%d pathMatches=%d versionMatches=%d key=%s expectedDir=%s actualDir=%s expectedVersion=%s actualVersion=%s detail=%s",
            ran?1:0,code,removing?1:0,inventoryKnown?1:0,after.installed?1:0,pathMatches?1:0,versionMatches?1:0,
            after.keyPath.c_str(),officialOpenGlassTarget_.c_str(),after.directory.c_str(),openGlassPackage_.version.c_str(),after.version.c_str(),inventoryError.c_str());
        if(!ran||(code!=ERROR_SUCCESS&&code!=ERROR_SUCCESS_REBOOT_REQUIRED)||!confirmed) {
            rollbackSucceeded_.store(false,std::memory_order_release);
            SetFailure((ru?L"Операция OpenGlass не подтверждена. Его состояние могло измениться; upstream-установка не откатывалась. ":
                L"OpenGlass operation was not confirmed. Its state may have changed; the upstream installation was not rolled back. ")+error+L" "+inventoryError,
                (code==ERROR_SUCCESS||code==ERROR_SUCCESS_REBOOT_REQUIRED)?ERROR_INSTALL_FAILURE:code);return false;
        }
        if(code==ERROR_SUCCESS_REBOOT_REQUIRED)AddFinishWarning(ru?L"OpenGlass: требуется перезагрузка":L"OpenGlass: restart required",
            ru?L"Официальный установщик OpenGlass сообщил о необходимости перезагрузки (код 3010). Даже если OpenGlass уже работает, для завершения отложенных изменений перезагрузите Windows.":
                L"The official OpenGlass installer requested a restart (code 3010). Even if OpenGlass is running, restart Windows to complete pending changes.");
        InstallLog::Line(L"OpenGlass official transaction confirmed; no Glass rollback claimed");
        if(!GlassInstallMsi::RefreshManagedArp(x64Dir_,x86Dir_,openGlassDir_))
            AddFinishWarning(ru?L"Не все записи приложений обновлены":L"Some installed-app entries were not updated",
                ru?L"Не удалось обновить ссылки обслуживания. Подробности в журнале.":L"Maintenance links could not all be updated. See the log.");
        SetStatus(labels_.finalize,selection_.openGlassResources?9600:10000);return true;
    }catch(...) {
        if(attempted)rollbackSucceeded_.store(false,std::memory_order_release);
        SetFailure(ru?L"Операция OpenGlass завершилась ошибкой; автоматический откат upstream не выполнялся.":
            L"OpenGlass operation failed; no automatic upstream rollback was performed.",ERROR_INSTALL_FAILURE);return false;
    }
}

// A separate reversible graphics transaction surrounds the irreversible OG commit.
// Its baseline is captured before ANY managed/vendor mutation, never afterward.
bool InstallEngine::OfficialResourcesPhase(bool preparing) noexcept {
    const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
    try {
        std::wstring root;
        if(!GlassInstallUtil::ResolveOpenGlassResourcesRoot(root)) {
            SetFailure(L"OpenGlass Resources root unavailable",GetLastError());return false;
        }
        if(preparing) {
            GlassPackageCatalog::Catalog catalog;std::wstring diagnostic,error;
            if(!EnsureTempWorkspace()){SetFailure(L"Cannot create protected Resources workspace",GetLastError());return false;}
            (void)GlassPackageAcquisition::FetchCatalog(cancelRequested_,catalog,diagnostic);
            if(!GlassPackageAcquisition::Prepare(GlassPackageCatalog::Product::OpenGlassResources,
                catalog,tempDir_,cancelRequested_,error)) {
                SetFailure(error,ERROR_INSTALL_PACKAGE_REJECTED);return false;
            }
            if(!GlassInstallMsi::SupportsResourcesBeforeOpenGlass(tempDir_+L"\\OpenGlassResources-x64.msi")) {
                InstallLog::Line(L"Resources pre-OG protocol missing in acquired MSI; refusing before managed mutation");
                SetFailure(ru?L"Пакет Resources устарел: он не поддерживает установку перед OpenGlass. Обновите Resources MSI и каталог релиза, затем перезапустите Setup. Продукты не изменены.":
                    L"The Resources package is outdated: it cannot be installed before OpenGlass. Update the Resources MSI and release catalog, then restart Setup. Products were not changed.",ERROR_INSTALL_PACKAGE_REJECTED);
                return false;
            }
            if(!GlassInstallRollback::CreateSnapshot(officialResourcesRollback_,x64Dir_,x86Dir_,openGlassDir_,false,
                {},&cancelRequested_,64u)) {
                SetFailure(ru?L"Не удалось сохранить исходное состояние OpenGlass Resources. Продукты не изменены.":
                    L"Could not snapshot OpenGlass Resources. Products were not changed.",officialResourcesRollback_.copyError);return false;
            }
        }
        const auto savedSelection=selection_;
        const auto savedPlan=executionPlan_;
        const bool savedReady=executionPlanReady_,savedScan=scanNativeX64_,savedPending=preparingOfficialResources_;
        const auto savedMask=procedureMask_;
        std::swap(rollbackSession_,officialResourcesRollback_);
        const auto restore=[&]() noexcept {
            std::swap(rollbackSession_,officialResourcesRollback_);
            selection_=savedSelection;executionPlan_=savedPlan;executionPlanReady_=savedReady;
            scanNativeX64_=savedScan;procedureMask_=savedMask;preparingOfficialResources_=savedPending;
        };
        struct Restore {const decltype(restore)& fn;~Restore(){fn();}} scope{restore};
        selection_={};selection_.platform64=selection_.platform32=selection_.glassSettings=selection_.openGlass=false;
        selection_.independentSettings=true;selection_.openGlassResources=true;
        scanNativeX64_=true;procedureMask_=64u;preparingOfficialResources_=true;
        if(!BuildExecutionPlan())return false;
        if(preparing)return true;
        // Cancellation closed at official OG commit. Only the graphics MSI and
        // its own checked baseline participate in rollback from this point.
        const auto path=tempDir_+L"\\OpenGlassResources-x64.msi";
        if(RunOneMsi(path,root,L"OpenGlass Resources",9600,350)) {
            // Keep the original baseline until the vendor confirms OG success.
            return true;
        }
        const auto error=ErrorText();
        const auto code=GlassInstallMsi::kProductOpenGlassResourcesX64;
        bool modified=false;
        for(const auto& item:rollbackSession_.modifiedProducts)modified=modified||_wcsicmp(item.c_str(),code)==0;
        if(GlassInstallMsi::IsProductInstalled(code)&&(modified||rollbackSession_.oldProducts.empty()))
            GlassInstallRollback::NoteInstalled(rollbackSession_,code); // Include failed MSI's possible registration.
        const bool restored=GlassInstallRollback::ExecuteRollback(rollbackSession_,{});
        preserveOfficialResourcesSnapshot_=!restored;
        SetFailure(error+(restored?(ru?L" Исходное состояние Resources восстановлено.":L" Resources baseline restored."):
            (ru?L" Откат Resources не завершён. Копия: ":L" Resources rollback incomplete. Backup: ")+rollbackSession_.workspaceDir),ERROR_INSTALL_FAILURE);
        return false;
    }catch(...) {
        preserveOfficialResourcesSnapshot_=true;
        SetFailure(ru?L"Операция Resources прервана; сохраните журнал и копию для восстановления.":
            L"Resources operation interrupted; retain the log and recovery snapshot.",ERROR_INSTALL_FAILURE);return false;
    }
}

bool InstallEngine::FinishOfficialResources(bool commit) noexcept {
    if(commit) {
        GlassInstallRollback::CleanupSnapshot(officialResourcesRollback_);
        const auto saved=selection_;
        selection_={};selection_.platform64=selection_.platform32=selection_.glassSettings=selection_.openGlass=false;
        selection_.independentSettings=true;selection_.openGlassResources=true;
        LaunchSelectedProducts(); // Publish only installed, confirmed package baselines.
        selection_=saved;
        return true;
    }
    const bool restored=GlassInstallRollback::ExecuteRollback(officialResourcesRollback_,{});
    preserveOfficialResourcesSnapshot_=!restored;
    if(restored)GlassInstallRollback::CleanupSnapshot(officialResourcesRollback_);
    return restored;
}

DWORD WINAPI InstallEngine::WorkerThread(LPVOID p) noexcept {
    auto* self=static_cast<InstallEngine*>(p);
    const bool mixedRemoval=GlassProducts::MixedOpenGlassRemoval(self->selection_,self->forceClean_,GlassInstallUtil::IsOs64Bit());
    self->mixedOpenGlass_=mixedRemoval||GlassProducts::MixedOpenGlassApply(self->selection_,self->forceClean_,GlassInstallUtil::IsOs64Bit());
    InstallLog::LineFormat(L"OpenGlass dispatcher revision=2: route=%s rows=0x%X ogApply=%d ogRemove=%d SecureUxTheme=%d forceClean=%d",
        self->mixedOpenGlass_?(mixedRemoval?L"mixed-remove":L"mixed-apply"):(self->selection_.openGlass||self->selection_.Removes(3)?L"standalone-or-unsupported":L"managed-glass"),
        self->procedureMask_,self->selection_.openGlass?1:0,self->selection_.Removes(3)?1:0,
        self->selection_.secureUxTheme?1:0,self->forceClean_?1:0);
    if(self->mixedOpenGlass_) {
        // Preparation failure changes no products. Keep the staged EXE until
        // after managed commit; the Glass worker must never own the OG row.
        const bool resources=self->selection_.openGlassResources;
        if(self->PrepareOfficialOpenGlass(mixedRemoval)&&(!resources||self->OfficialResourcesPhase(true))&&
           (!self->selection_.cleanupUserData||self->PrepareFullRemovalCleanup())) {
            const bool originalApply=self->selection_.openGlass;
            const auto originalRemoveMask=self->selection_.removeMask;
            const bool originalCleanup=self->selection_.cleanupUserData;
            self->selection_.cleanupUserData=false; // Deferred until official OG removal is confirmed.
            self->selection_.openGlass=false;
            self->selection_.openGlassResources=false;
            self->selection_.removeMask&=~8u;
            self->procedureMask_&=~8u;
            if(resources)self->procedureMask_&=~64u;
            const bool managed=GlassProducts::HasPackageApply(self->selection_)||
                self->selection_.removeMask||self->selection_.secureUxThemeRemove;
            if(managed)RunGlassWorker(p);
            else self->succeeded_=true;
            self->selection_.openGlass=originalApply;
            self->selection_.openGlassResources=resources;
            self->selection_.removeMask=originalRemoveMask;
            self->selection_.cleanupUserData=originalCleanup;
            if(self->succeeded_&&resources) {
                self->succeeded_=self->OfficialResourcesPhase(false);
                if(!self->succeeded_) {
                    const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
                    self->rollbackSucceeded_.store(false,std::memory_order_release);
                    self->SetFailure((ru?L"Resources не установлены; официальный установщик OpenGlass не запускался. Предыдущие изменения Glass сохранены. ":
                        L"Resources installation failed; the official OpenGlass installer was not started. Previous Glass changes were retained. ")+self->ErrorText(),ERROR_INSTALL_FAILURE);
                }
            }
            if(self->succeeded_) {
                self->succeeded_=self->CommitOfficialOpenGlass(mixedRemoval);
                if(!self->succeeded_&&resources) {
                    const bool restored=self->FinishOfficialResources(false);
                    const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
                    self->SetFailure(self->ErrorText()+(restored?
                        (ru?L" Исходное состояние Resources восстановлено.":L" Resources baseline restored."):
                        (ru?L" Восстановление Resources не завершено; сохраните журнал и резервную копию.":L" Resources recovery incomplete; retain the log and snapshot.")),ERROR_INSTALL_FAILURE);
                }
                if(!self->succeeded_) {
                    self->preserveSnapshot_=!self->rollbackSession_.workspaceDir.empty();
                    InstallLog::LineFormat(L"Mixed operation partial; retaining managed snapshot: %s",self->rollbackSession_.workspaceDir.c_str());
                    const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
                    self->rollbackSucceeded_.store(false,std::memory_order_release);
                    self->SetFailure((managed?(ru?L"План выполнен частично: изменения Glass сохранены, финальная операция OpenGlass не подтверждена. Общий откат не выполнялся. ":
                        L"Plan partially completed: Glass changes were committed; the final OpenGlass operation was not confirmed. No overall rollback was performed. "):
                        (ru?L"Операция OpenGlass не подтверждена. ":L"OpenGlass operation was not confirmed. "))+self->ErrorText()+
                        (self->preserveSnapshot_?L"\nBackup: "+self->rollbackSession_.workspaceDir:L""),ERROR_INSTALL_FAILURE);
                }
                if(self->succeeded_&&managed) {
                    // Official installer starts OG itself; never launch Platform
                    // in the managed subtransaction before that vendor commit.
                    const bool applyOg=self->selection_.openGlass;
                    self->selection_.openGlass=false;
                    self->LaunchSelectedProducts();
                    self->selection_.openGlass=applyOg;
                }
                if(self->succeeded_)GlassInstallRollback::CleanupSnapshot(self->rollbackSession_);
                if(self->succeeded_&&mixedRemoval)self->FinishFullRemovalCleanup();
                if(self->succeeded_&&resources) {
                    self->FinishOfficialResources(true);
                    self->SetStatus(self->labels_.finalize,10000);
                }
            }
        }
    } else if(self->selection_.openGlass||self->selection_.Removes(3)) {
        self->succeeded_=self->RunOfficialOpenGlassOnly();
    } else RunGlassWorker(p);
    if(self->mixedOpenGlass_&&!self->succeeded_&&!self->rollbackSession_.workspaceDir.empty()) {
        self->preserveSnapshot_=true;
        InstallLog::LineFormat(L"Incomplete mixed plan: recovery snapshot retained at %s",self->rollbackSession_.workspaceDir.c_str());
    }
    if(!self->tempDir_.empty()) {
        if(RemoveDirectoryTree(self->tempDir_))self->tempDir_.clear();
        else InstallLog::LineFormat(L"Staging cleanup incomplete; destructor will retry: %s",self->tempDir_.c_str());
    }
    self->finished_.store(true,std::memory_order_release);
    self->launchingPhase_.store(false,std::memory_order_release);
    return 0;
}

DWORD WINAPI InstallEngine::RunGlassWorker(LPVOID p) noexcept {
            auto* self = static_cast<InstallEngine*>(p);
            const bool vendorOnly=GlassProducts::OnlySecureUxThemeRemoval(self->selection_);
            bool priorStateOk=vendorOnly || self->CapturePriorHostState();
            if(vendorOnly || self->auxiliaryOnly_)self->scanNativeX64_=GlassInstallUtil::IsOs64Bit();
            else if (priorStateOk) priorStateOk=self->DetermineCleanInstall();
            if (self->forceClean_)
                InstallLog::Line(L"Clean reinstall: remnants will be removed before install");

            bool ok = priorStateOk && !self->IsCancelRequested() && self->ExtractPayload(self->instance_);
            bool snapshotFailed = !priorStateOk && !self->IsCancelRequested();

            // Снимок до стопа хостов: если на диске нет места или нажата Отмена, система не затрагивается.
            if (ok && !vendorOnly) {
                // Snapshot even on a clean/reinstall path: pre-existing user files must survive cancellation.
                ok = self->CreateRollbackSnapshot();
                snapshotFailed = !ok && !self->IsCancelRequested();
            }

            if (ok && !self->IsCancelRequested()) ok = self->PrepareSecureUxRemoval();
            if (ok && !self->IsCancelRequested()) ok = self->PrepareFullRemovalCleanup();
            if (ok && !self->IsCancelRequested()) ok = self->BuildExecutionPlan();
            if(ok && !self->IsCancelRequested())ok=self->PrepareManagedRemoval();

            if (ok && !vendorOnly && !self->IsCancelRequested()) {
                if(GlassProcedurePlan::Settings(self->procedureMask_)) {
                    ok=GlassInstallUtil::CloseSelectedSettingsProcesses(self->x64Dir_,self->x86Dir_,self->procedureMask_&36u,&self->cancelRequested_);
                    if(!ok&&!self->IsCancelRequested())self->SetFailure(PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN?L"Не удалось закрыть выбранную версию GlassSettings. Закройте её и повторите операцию.":L"Could not close the selected GlassSettings version. Close it and retry.",GetLastError());
                }
                if (ok && !self->IsCancelRequested()) ok = self->StopRunningHosts();
            }

            // The chosen Settings product is installed after the snapshot and
            // platform-stop/mutex wait, but before the two Native monitors.
            // If it is not chosen, only trusted installed helpers are used; no temporary
            // Settings MSI is registered. Record changes before starting MSI so
            // cancellation or a scan failure takes the normal rollback path.
            if (ok && !vendorOnly && !self->IsCancelRequested()) ok = self->InstallGlassSettingsEarly();
            // Clean install: on the machine there is neither a platform nor tasks,
            // services or products that could have loaded Glass modules: nothing to
            // scan, nothing to release. The stage is skipped (as requested): the scan
            // used to open Process32/Module32 and, on a mixed x86/x64 machine, the
            // native monitor, for nothing.
            // If the picture is not clean, the stage still runs in full.
            if (ok && !vendorOnly && !self->IsCancelRequested()) {
                if (self->cleanInstall_ || !GlassProcedurePlan::ModuleScan(self->procedureMask_)) {
                    InstallLog::Line(L"Unlocker: no planned platform module mutation, or clean install - scan skipped");
                } else {
                    ok = self->ReleaseLockedModules();
                }
            }
            if (ok && !vendorOnly && !self->IsCancelRequested() && self->forceClean_) {
                self->installAttempted_ = true;
                self->nonSettingsChangesAttempted_ = true;
                ok = self->PurgeRemnants();
                if (ok) self->cleanInstall_ = true;
            }
            // SUT must be removed while the installed Native helper still
            // exists, before managed removal can delete Settings. All baseline,
            // profile, module-release and managed removal preflights ran above.
            if(ok && self->selection_.secureUxThemeRemove && self->selection_.removeMask)
                ok=self->CommitEarlySecureUxRemoval();
            if (ok && !vendorOnly && !self->IsCancelRequested()) {
                self->installAttempted_ = true;
                self->nonSettingsChangesAttempted_ = true;
                ok = self->RunMsiSequence();
            }

            // Финальная стадия запуска компонентов: commit всегда, Page::Launching >= 5 секунд только при запуске.
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
                    self->hostLaunchExpected_.store(GlassProducts::NeedsHostLaunch(self->selection_,self->openGlassStartup_),std::memory_order_release);
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
                if(!vendorOnly)ok = self->RegisterSelectedProducts();
                // Apply/standalone vendor work is commit-last. Mixed removal
                // already committed SUT before deleting its Native helper; the
                // adapter's committed flag prevents a second vendor execution.
                if(ok && self->selection_.removeMask) {
                    std::wstring detail;
                    ok=GlassInstallRemoval::Commit(self->removalSession_,PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN,detail);
                    if(!ok)self->SetFailure(detail,0);
                }
                if (ok) ok = self->InstallOriginalSecureUxTheme();
                if (ok) {
                    // Commit point. From here on the products are started and the
                    // installation is final: no step below may fail the install
                    // or trigger a rollback; problems become finish-page notes.
                    InstallLog::Line(L"Launch: commit point reached, rollback is no longer possible");
                    if(self->openGlassFiles_)self->openGlassFiles_->Commit();
                    self->rollbackSession_.restoreOpenGlassFiles={};
                    if(!vendorOnly && !self->auxiliaryOnly_ && !self->mixedOpenGlass_)self->LaunchSelectedProducts();
                    constexpr ULONGLONG kMinLaunchMs = 5000;
                    for (;self->HasHostLaunchWork();) {
                        const ULONGLONG elapsed = GetTickCount64() - launchStart;
                        if (elapsed >= kMinLaunchMs)
                            break;
                        const ULONGLONG remain = kMinLaunchMs - elapsed;
                        Sleep(remain > 150 ? 150 : static_cast<DWORD>(remain));
                    }
                    self->FinishFullRemovalCleanup();
                    if(!self->mixedOpenGlass_)self->SetStatus(self->labels_.finalize, 10000);
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
            if (restored && (!self->mixedOpenGlass_ || !ok)) GlassInstallRollback::CleanupSnapshot(self->rollbackSession_);
            else if(restored)InstallLog::Line(L"Mixed transaction: managed snapshot retained until official OG result");
            else {
                self->preserveSnapshot_=true;
                const std::wstring backup=self->rollbackSession_.workspaceDir;
                AcquireSRWLockExclusive(&self->statusLock_);
                self->rollbackNote_=self->labels_.rollbackFailed;
                if (!backup.empty()) self->rollbackNote_+=L"\r\nBackup: "+backup;
                ReleaseSRWLockExclusive(&self->statusLock_);
                InstallLog::LineFormat(L"Rollback failed; retaining backup: %s",backup.c_str());
            }
            if(!ok && self->secureUxRemovalCommitted_) {
                const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
                self->SetFailure((ru?L"План выполнен частично: SecureUxTheme удалён и не восстанавливался. Восстановление изменений Glass выполнялось отдельно; общего отката нет. ":
                    L"Plan partially completed: SecureUxTheme was removed and was not restored. Glass recovery was handled separately; no overall rollback occurred. ")+self->ErrorText(),ERROR_INSTALL_FAILURE);
                self->rollbackSucceeded_.store(false,std::memory_order_release);
            }
            // The local Native module has been released before rollback and no
            // helper process is needed beyond this point.

            self->succeeded_ = ok && !self->userCancelled_.load(std::memory_order_acquire);
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
    StatusFn onStatus,const std::wstring& secureUxRemovalCode) noexcept {
    if (started_) return false;
    started_ = true;
    instance_ = instance;
    labels_ = std::move(labels);
    status_ = std::move(onStatus);
    selection_ = selection;
    secureUxRemovalCode_=secureUxRemovalCode;
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
    hostLaunchExpected_.store(false, std::memory_order_relaxed);

    selection_=GlassProducts::NormalizeInstall(selection_,GlassInstallUtil::IsOs64Bit());
    if((selection_.openGlassResources||selection_.Removes(6))&&forceClean_) {
        const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
        SetFailure(ru?L"OpenGlass Resources нельзя совмещать с принудительной очисткой.":
            L"OpenGlass Resources cannot be combined with forced cleanup.",ERROR_INSTALL_PACKAGE_REJECTED);
        return false;
    }
    procedureMask_=GlassProcedurePlan::MutationMask(selection_,GlassInstallUtil::IsOs64Bit());
    auxiliaryOnly_=procedureMask_==8u && selection_.Removes(3) && !forceClean_ &&
        !GlassOpenGlassFiles::HasReceipt(openGlassDir_) &&
        MsiQueryProductStateW(GlassInstallMsi::kProductOpenGlass)==INSTALLSTATE_UNKNOWN &&
        MsiQueryProductStateW(GlassInstallMsi::kLegacyOpenGlass)==INSTALLSTATE_UNKNOWN;
    InstallLog::LineFormat(L"Procedure map: rows=0x%X auxiliaryOnly=%d",procedureMask_,auxiliaryOnly_?1:0);

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
    if(IsCancelRequested())return false;
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
    if(IsCancelRequested())return false; // Validation can race with cancellation; do not signal a new stop.
    InstallLog::LineFormat(L"%s: beginning host stop",logTag);
    bool attempted=false;
    taskError.clear();
    const bool stopped=GlassInstallHost::StopGlassPlatform(
        arch,fixedRoot,&cancelRequested_,&attempted,&taskError);
    if (arch==GP::PlatformArch::X64) x64StopAttempted_=attempted;
    else x86StopAttempted_=attempted;
    if (IsCancelRequested()) {InstallLog::LineFormat(L"%s: cancellation during host stop; attempted=%d stopped=%d; restoring prior state",logTag,attempted?1:0,stopped?1:0);return false;}
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

    if(IsCancelRequested())return false;
    hostsStopped_ = true;
    // Recheck only affected hosts, even on a seemingly clean install.
    // A newly appeared or unknown affected host fails closed.
    if (GlassProcedurePlan::Platform64(procedureMask_) && !StopGlassPlatform(GP::PlatformArch::X64, labels_.stop64, L"x64", fail)) return false;
    if (GlassProcedurePlan::Platform32(procedureMask_) && !StopGlassPlatform(GP::PlatformArch::X86, labels_.stop86, L"x86", fail)) return false;
    if (IsCancelRequested()) return false;

    if(!GlassProcedurePlan::OpenGlass(procedureMask_))return true;
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
    WizardUnlock::EndOnExit finishUnlock(unlockActivity_);
#ifdef GLASS_SETUP_NATIVE_MONITOR
    struct MonitorScope { ~MonitorScope(){GlassInstallWatch::EndLocalNativeMonitor();} } monitor;
#endif
    for(;;) {
        if(IsCancelRequested())return false;
        bool failed=false;
#ifdef GLASS_SETUP_NATIVE_MONITOR
        failed=GlassProcedurePlan::Platform32(procedureMask_) && !GlassInstallWatch::BeginLocalNativeMonitor();
#endif
        std::vector<GlassInstallUnlock::ModuleHolder> holders;
        if(!failed) {
            const bool clear=GlassInstallWatch::WaitUntilClearOrTimeout(30000,x64Dir_,x86Dir_,scanNativeX64_&&GlassProcedurePlan::Platform64(procedureMask_),holders,failed,&cancelRequested_,GlassProcedurePlan::Platform32(procedureMask_));
            if(IsCancelRequested())return false;
            if(!failed && clear && holders.empty()) {
                InstallLog::Line(L"Module scan verified: all Glass modules unloaded");return true;
            }
        }
        AcquireSRWLockExclusive(&statusLock_);
        unlockActivity_.End();
        if(holdGate_)ResetEvent(holdGate_);
        holdKind_=failed?HoldKind::MissingHelper:HoldKind::Modules;
        holdReport_=failed?L"":GlassInstallUnlock::FormatHolders(holders);
        holdAction_=HoldAction::None;
        ReleaseSRWLockExclusive(&statusLock_);
        InstallLog::Line(failed?L"Module scan unavailable: explicit risk consent required":L"Modules remain loaded: Retry / Continue at risk / Cancel");
        if(IsCancelRequested())return false;
        WaitForSingleObject(holdGate_,INFINITE);
        AcquireSRWLockShared(&statusLock_);const auto action=holdAction_.load(std::memory_order_acquire);ReleaseSRWLockShared(&statusLock_);
        if(action==HoldAction::Cancel || IsCancelRequested()){quietClose_=true;return false;}
        if(action==HoldAction::ContinueRisk) {
            const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
            AddFinishWarning(ru?L"Продолжено без подтверждённой выгрузки модулей":L"Continued without verified module unloading",
                ru?L"Вы явно приняли риск. Проверки безопасности MSI не отключены; может потребоваться перезагрузка.":
                   L"You explicitly accepted the risk. MSI security checks remain enabled; a restart may be required.");
            InstallLog::Line(L"Explicit module-monitor risk accepted; state remains unknown/loaded, not unloaded");return true;
        }
#ifdef GLASS_SETUP_NATIVE_MONITOR
        GlassInstallWatch::EndLocalNativeMonitor();
#endif
        // Retry only observes again. Never inject/unload/terminate a holder as
        // a hidden side effect of retrying an unavailable or nonempty scan.
    }
}

bool InstallEngine::EnsureTempWorkspace() noexcept {
    if (!tempDir_.empty()) return true;
    if (!GlassPackageAcquisition::CreateWorkspace(tempDir_)) return false;
    InstallLog::LineFormat(L"Protected package workspace: %s", tempDir_.c_str());
    return true;
}

bool InstallEngine::ExtractPayload(HINSTANCE instance) noexcept {
    (void)instance; // Legacy method name; no MSI resource extraction remains here.
    if (!EnsureTempWorkspace()) {
        SetFailure(L"Cannot create the protected ProgramData package workspace.", GetLastError());
        return false;
    }
    if(GlassProducts::OnlySecureUxThemeRemoval(selection_))return true; // Private vendor baseline only; no release IO.
    if(GlassProducts::OnlyManagedRemoval(selection_))return true; // Installed snapshot only; no release IO.
    SetStatus(labels_.extract, 0);
    GlassPackageCatalog::Catalog catalog;
    std::wstring diagnostic;
    if (!GlassPackageAcquisition::FetchCatalog(cancelRequested_,catalog,diagnostic)) {
        if (IsCancelRequested()) return false;
        InstallLog::LineFormat(L"Release catalog unavailable; checking independent local/cache sources: %s",diagnostic.c_str());
    }
    using Product=GlassPackageCatalog::Product;
    const bool os64=scanNativeX64_;
    const struct {Product product;bool selected;} packages[]={
        {Product::Platform64,os64&&selection_.platform64},
        {Product::Platform32,selection_.platform32},
        {Product::Settings64,selection_.Settings64(os64)},
        {Product::Settings32,selection_.Settings32(os64)},
        {Product::OpenGlass,os64&&selection_.openGlass},
        {Product::OpenGlassResources,os64&&selection_.openGlassResources}
    };
    int total=0,done=0;
    for(const auto& p:packages)if(p.selected)++total;
    for(const auto& p:packages) {
        if(!p.selected)continue;
        if(IsCancelRequested())return false;
        std::wstring error;
        const bool prepared=p.product==Product::OpenGlass?
            GlassOpenGlassPackage::PrepareForCurrentSystem(tempDir_,cancelRequested_,openGlassPackage_,error):
            GlassPackageAcquisition::Prepare(p.product,catalog,tempDir_,cancelRequested_,error);
        if(!prepared) {
            if(!IsCancelRequested())SetFailure(error,0);
            return false;
        }
        SetStatus(labels_.extract,(++done*400)/(total?total:1));
    }
    if (selection_.secureUxTheme) {
        const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
        SetStatus(ru?L"SecureUxTheme: проверка локального MSI / загрузка":
                     L"SecureUxTheme: checking local MSI / downloading",400);
        std::wstring error;
        if (!GlassSecureUxPackage::Prepare(tempDir_,cancelRequested_,secureUxPackage_,error)) {
            if (!IsCancelRequested()) SetFailure(error,0);
            return false;
        }
    }
    return !IsCancelRequested();
}

bool InstallEngine::RecheckSnapshottedProduct(const wchar_t* productCode) noexcept {
    if (!productCode || !*productCode || IsCancelRequested()) return false;
    std::wstring detail;
    for (const auto& snap:rollbackSession_.oldProducts) {
        if (_wcsicmp(snap.productCode.c_str(),productCode)!=0) continue;
        if(GlassRegisteredMsi::Recheck(productCode,snap.installDir,true,snap.cachedMsiPath,
            rollbackSession_.workspaceDir,detail,&cancelRequested_))return true;
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
        {GlassInstallMsi::kProductOpenGlassResourcesX64,nullptr,L"{A5571574-00E7-43AD-81DF-B65D1AE07699}"},
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
    if(preparingOfficialResources_&&selection_.openGlassResources&&!openGlassPackage_.version.empty()&&
       _wcsicmp(productCode.c_str(),GlassInstallMsi::kProductOpenGlassResourcesX64)==0)
        props+=L" GLASS_OG_INSTALL_PENDING=1";
    const wchar_t* msiName=PathFindFileNameW(msiPath.c_str());
    const bool secureUxThemeMsi=msiName &&
        (_wcsicmp(msiName,L"SecureUxTheme-x64.msi")==0 || _wcsicmp(msiName,L"SecureUxTheme-x86.msi")==0);
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
    if(exitCode==ERROR_SUCCESS_REBOOT_REQUIRED||exitCode==ERROR_SUCCESS_REBOOT_INITIATED)
        (void)GlassMaintenanceReboot::Pinned(true);
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
        InstallLog::Line(L"GlassSettings not selected: monitoring requires trusted installed Native helpers");
        return true;
    }
    bool settingsRunning = false;
    const bool settingsKnown = GlassInstallUtil::QuerySelectedSettingsRunning(x64Dir_,x86Dir_,procedureMask_&36u,settingsRunning);
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
    namespace P=GlassProductPlan;
    if(!executionPlanReady_ || !executionPlan_.valid)return false;
    int base=800;
    const auto result=GlassMaintenanceExecutor::RunPhase(executionPlan_,GlassMaintenanceExecutor::Phase::Helpers,
        [&](P::Product product,P::Action)->unsigned {
            if(IsCancelRequested())return ERROR_INSTALL_USEREXIT;
            const bool x64=product==P::Product::Settings64;
            if(!x64 && product!=P::Product::Settings32)return ERROR_INVALID_PARAMETER;
            earlySettingsAttemptMask_|=x64?1u:2u;
            installAttempted_=true;
            const auto path=tempDir_+(x64?L"\\GlassSettings-x64.msi":L"\\GlassSettings-x86.msi");
            const bool ok=RunOneMsi(path,x64?x64Dir_:x86Dir_,x64?labels_.glassSettings64:labels_.glassSettings86,base,600);
            base+=600;
            return ok?ERROR_SUCCESS:IsCancelRequested()?ERROR_INSTALL_USEREXIT:ERROR_INSTALL_FAILURE;
        });
    if(!result.ok) {
        InstallLog::LineFormat(L"Helper phase failed: error=%u attempted=0x%X completed=0x%X",result.error,result.attemptedMask,result.completedMask);
        return false;
    }
    const bool os64 = GlassInstallUtil::IsOs64Bit();
    const std::wstring& root = (os64 && selection_.settings64) ? x64Dir_ : x86Dir_;
    const std::wstring exe = GlassInstallUtil::TrimTrailingSlashes(root) + L"\\GlassSettings\\GlassSettings.exe";
    if (!GlassInstallUtil::RegisterSettingsBackupAssociation(exe))
        InstallLog::Line(L"Settings backup file association was not registered");
    return true;
}

bool InstallEngine::PrepareSecureUxRemoval() noexcept {
    if(!selection_.secureUxThemeRemove)return true;
    if(IsCancelRequested())return false;
    std::wstring error;
    auto state=GlassInstallUtil::SecureUxThemeState::Conflict;
    secureUxLoadedRemoval_=false;
    const bool manual=selection_.secureUxRemovalWithoutSettings;
    const bool nativeUpdate=GlassInstallUtil::IsOs64Bit()?selection_.Settings64(true):selection_.Settings32(false);
    if(manual || nativeUpdate) {
        // The selected Settings MSI is rolled back normally if later checks fail.
        // Never ask the OLD Native to authorize a dependency on its replacement.
        if(selection_.secureUxTheme || (manual&&!GlassInstallUtil::GlassSettingsAbsentForSecureUxRemoval())) {
            SetFailure(L"SecureUxTheme manual-removal consent no longer matches Settings presence.",ERROR_INVALID_STATE);return false;
        }
        if(GlassSecureUxPackage::PrepareRemoval(tempDir_,secureUxRemovalCode_,cancelRequested_,secureUxRemoval_,error))return true;
        SetFailure(error,ERROR_INVALID_STATE);return false;
    }
    const bool known=GlassInstallUtil::QuerySecureUxThemeState(x64Dir_,x86Dir_,&state,&error) && GlassProducts::SecureUxKnownRemoval(state);
    if(!selection_.secureUxTheme&&known) {
        secureUxLoadedRemoval_=!GlassProducts::SecureUxUnloadedRemoval(state);
        try {
            if(secureUxLoadedRemoval_) {
                if(secureUxRemovalProfiles_.PreflightLoadedRemoval(error)) {
                    if(!error.empty())InstallLog::LineFormat(L"SecureUxTheme profile preparation: %s",error.c_str());
                    if(GlassSecureUxPackage::PrepareRemoval(tempDir_,secureUxRemovalCode_,cancelRequested_,secureUxRemoval_,error))return true;
                }
            }else if(GlassSecureUxPackage::PrepareRemoval(tempDir_,secureUxRemovalCode_,cancelRequested_,secureUxRemoval_,error))return true;
        }catch(...){error=L"Cannot prepare the user/profile safety baseline.";}
    }
    InstallLog::LineFormat(L"Vendor removal preflight refused: %s",error.c_str());
    if(!IsCancelRequested())SetFailure(PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN?
        L"Удаление SecureUxTheme остановлено до изменений. Проверка MSI или профилей не пройдена. "+error:
        L"SecureUxTheme removal stopped before changes. MSI/profile preflight failed. "+error,ERROR_INVALID_STATE);
    return false;
}

bool InstallEngine::RemoveOriginalSecureUxTheme() noexcept {
    // Cancellation is already closed at either vendor boundary. In a mixed
    // removal this runs BEFORE Settings deletion; otherwise it is commit-last.
    // Vendor MSI owns its transaction/permanent files, never Glass rollback.
    namespace P=GlassProductPlan;
    if(!executionPlanReady_ || !executionPlan_.valid ||
       executionPlan_.rows[P::Index(P::Product::SecureUxTheme)].action!=P::Action::Remove) {
        SetFailure(PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN?
            L"План выполнения не разрешает удаление SecureUxTheme.":
            L"SecureUxTheme removal is not authorized by the execution plan.",ERROR_INSTALL_PACKAGE_REJECTED);return false;
    }
    std::wstring error;auto state=GlassInstallUtil::SecureUxThemeState::Conflict;
    const bool manual=selection_.secureUxRemovalWithoutSettings;
    const bool nativeUpdate=GlassInstallUtil::IsOs64Bit()?selection_.Settings64(true):selection_.Settings32(false);
    bool runtimeValid=false;
    if(manual) {
        runtimeValid=GlassInstallUtil::GlassSettingsAbsentForSecureUxRemoval();
        secureUxLoadedRemoval_=false;
    }else {
        runtimeValid=GlassInstallUtil::QuerySecureUxThemeState(x64Dir_,x86Dir_,&state,&error)&&GlassProducts::SecureUxKnownRemoval(state);
        if(runtimeValid&&nativeUpdate) {
            secureUxLoadedRemoval_=!GlassProducts::SecureUxUnloadedRemoval(state);
            if(secureUxLoadedRemoval_)runtimeValid=secureUxRemovalProfiles_.PreflightLoadedRemoval(error);
        }
        runtimeValid=runtimeValid&&(GlassProducts::SecureUxUnloadedRemoval(state)||secureUxLoadedRemoval_);
    }
    if(!selection_.secureUxThemeRemove || selection_.secureUxTheme || !runtimeValid ||
       !GlassSecureUxPackage::RecheckRemoval(tempDir_,cancelRequested_,secureUxRemoval_,error)) {
        InstallLog::LineFormat(L"Vendor removal revalidation refused: %s",error.c_str());
        SetFailure(PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN?
            L"MSI, активность SecureUxTheme или состав GlassSettings изменились после проверки; удаление отменено.":
            L"SecureUxTheme MSI/runtime or GlassSettings presence changed after preflight; removal was refused.",ERROR_INVALID_STATE);
        return false;
    }
    if(secureUxLoadedRemoval_) {
        const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
        bool prepared=false;
        try {
            GlassInstallUtil::SecureUxThemeDeactivateRun run{};
            prepared=secureUxRemovalProfiles_.RecheckLoadedRemoval(error) &&
                GlassInstallUtil::PrepareSecureUxRemovalVisualStyle(x64Dir_,x86Dir_,&run,&error) && run.ran &&
                !run.themeNotRestored && !run.skippedExternalInstall &&
                secureUxRemovalProfiles_.CommitLoadedRemovalStyle(error) &&
                secureUxRemovalProfiles_.RecheckLoadedRemoval(error) &&
                GlassInstallUtil::QuerySecureUxThemeState(x64Dir_,x86Dir_,&state,&error) && GlassProducts::SecureUxKnownRemoval(state) &&
                GlassSecureUxPackage::RecheckRemoval(tempDir_,cancelRequested_,secureUxRemoval_,error) &&
                secureUxRemovalProfiles_.RecheckLoadedRemoval(error);
        }catch(...){error=L"Style preparation could not complete.";}
        if(!prepared) {
            InstallLog::LineFormat(L"Loaded SecureUxTheme removal refused after style preparation: %s",error.c_str());
            SetFailure((ru?L"MSI SecureUxTheme не удалён. При подготовке визуальный стиль мог быть сброшен; автоматического возврата custom style нет.\n":
                L"SecureUxTheme MSI was not removed. The visual style may already be reset; no automatic custom-style restore is attempted.\n")+error,ERROR_INVALID_STATE);
            return false;
        }
        InstallLog::Line(L"Loaded SecureUxTheme removal: all profile selectors/current-session style and original MSI revalidated; invoking vendor MSI");
    }
    GlassInstallMsi::ProgressSink sink{};
    sink.label=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN?L"Удаление SecureUxTheme":L"Removing SecureUxTheme";
    sink.base=9200;sink.span=300;
    sink.onProgress=[this](const std::wstring& label,int progress){SetStatus(label,progress);};
    // No cancellation pointer, INSTALLDIR, Glass ARP rewrite or manual cleanup.
    const auto log=InstallLog::NextMsiVerboseLogPath(L"SecureUxTheme-original-remove");
    if(!log.empty())MsiEnableLogW(INSTALLLOGMODE_VERBOSE,log.c_str(),INSTALLLOGATTRIBUTES_FLUSHEACHLINE);
    UINT result=ERROR_SUCCESS;
    const bool removed=GlassInstallMsi::UninstallProduct(secureUxRemoval_.productCode.c_str(),&sink,&result);
    InstallLog::LineFormat(L"Original SecureUxTheme removal returned %u (ProductCode %s)",result,secureUxRemoval_.productCode.c_str());
    const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
    if(!removed){SetFailure(secureUxLoadedRemoval_?
        (ru?L"Не удалось удалить оригинальный MSI SecureUxTheme. При подготовке применён штатный визуальный стиль; custom style автоматически не восстанавливается.":L"Could not remove the original SecureUxTheme MSI. The stock style was applied during preparation; the custom style is not restored automatically."):
        (ru?L"Не удалось удалить оригинальный MSI SecureUxTheme.":L"Could not remove the original SecureUxTheme MSI."),result);return false;}
    if(manual)AddFinishWarning(ru?L"SecureUxTheme: ручная подготовка темы":L"SecureUxTheme: manual theme preparation",
        ru?L"Удаление выполнено без GlassSettings. Штатная тема и состояние загруженных компонентов автоматически не проверялись; ответственность за подготовку темы и последствия принята пользователем. Перезагрузите Windows.":
           L"Removal completed without GlassSettings. Stock theme and loaded component state were not verified automatically; the user accepted responsibility for theme preparation and consequences. Restart Windows.");
    if(secureUxLoadedRemoval_)for(const auto& skipped:secureUxRemovalProfiles_.SkippedLoadedProfiles()) {
        const std::wstring user=(skipped.second.empty()?L"":skipped.second+L" ")+L"["+skipped.first+L"]";
        AddFinishWarning(ru?L"SecureUxTheme: профиль пропущен":L"SecureUxTheme: profile skipped",
            user+(ru?L" — ключ HKEY_USERS отсутствует. Настройки темы этого пользователя не изменены; NTUSER.DAT не подключался.":
                     L" — HKEY_USERS key is absent. This user's theme settings were not changed; NTUSER.DAT was not loaded."));
    }
    if(!GlassInstallMsi::RefreshManagedArp(x64Dir_,x86Dir_,openGlassDir_))
        AddFinishWarning(ru?L"Записи обслуживания обновлены не полностью":L"Maintenance entries were not fully refreshed",
            ru?L"SecureUxTheme удалён. Не все ограничения удаления оставшихся продуктов обновлены; подробности в журнале.":
               L"SecureUxTheme was removed. Some remaining removal policies could not be refreshed; see the log.");
    bool restart=manual || secureUxLoadedRemoval_ || result==ERROR_SUCCESS_REBOOT_REQUIRED || result==ERROR_SUCCESS_REBOOT_INITIATED;
    auto after=GlassInstallUtil::SecureUxThemeState::Conflict;
    if(!manual && GlassInstallUtil::QuerySecureUxThemeState(x64Dir_,x86Dir_,&after,&error) && result!=ERROR_SUCCESS_REBOOT_INITIATED) {
        const bool runtimeRestart=secureUxLoadedRemoval_ || GlassProducts::SecureUxRestartAfter(false,after,restart);
        bool pending=true;
        if(runtimeRestart || !restart || (GlassInstallUtil::QuerySecureUxPendingFileOperations(pending)&&!pending))restart=runtimeRestart;
    }
    InstallLog::LineFormat(L"SecureUxTheme after removal: state=%u; restart=%d; MSI=%u",static_cast<unsigned>(after),restart?1:0,result);
    if(restart)(void)GlassMaintenanceReboot::Pinned(true);
    if(restart)
        AddFinishWarning(ru?L"SecureUxTheme: требуется перезагрузка":L"SecureUxTheme: restart required",
            ru?L"Для завершения изменения состояния SecureUxTheme требуется перезагрузка. Постоянные файлы автора сохраняются.":
               L"A restart is required to complete the SecureUxTheme state change. Author-owned permanent files are retained.");
    return true;
}

bool InstallEngine::CommitEarlySecureUxRemoval() noexcept {
    // Same cancel/commit gate as the normal final vendor phase. No accepted
    // cancellation may arrive after an irreversible vendor removal.
    for(;;) {
        if(decisionGate_)WaitForSingleObject(decisionGate_,INFINITE);
        AcquireSRWLockExclusive(&statusLock_);
        if(cancelRequested_.load(std::memory_order_acquire)) {
            ReleaseSRWLockExclusive(&statusLock_);return false;
        }
        const bool ready=!decisionGate_||WaitForSingleObject(decisionGate_,0)==WAIT_OBJECT_0;
        if(ready)launchingPhase_.store(true,std::memory_order_release);
        ReleaseSRWLockExclusive(&statusLock_);
        if(ready)break;
    }
    secureUxRemovalCommitted_=RemoveOriginalSecureUxTheme();
    return secureUxRemovalCommitted_;
}

bool InstallEngine::InstallOriginalSecureUxTheme() noexcept {
    if(secureUxRemovalCommitted_)return true;
    if(selection_.secureUxThemeRemove)return RemoveOriginalSecureUxTheme();
    if (!selection_.secureUxTheme) return true;
    const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
    namespace P=GlassProductPlan;
    if(!executionPlanReady_ || !executionPlan_.valid ||
       executionPlan_.rows[P::Index(P::Product::SecureUxTheme)].action!=P::Action::Install) {
        SetFailure(ru?L"План выполнения не разрешает установку SecureUxTheme.":L"SecureUxTheme installation is not authorized by the execution plan.",ERROR_INSTALL_PACKAGE_REJECTED);return false;
    }
    std::vector<std::wstring> officialProducts;
    std::wstring presenceError;
    if(!GlassSecureUxPackage::QueryInstallPresence(officialProducts,cancelRequested_,presenceError)) {
        InstallLog::LineFormat(L"SecureUxTheme install presence refused: %s",presenceError.c_str());
        SetFailure(L"SecureUxTheme MSI inventory or ownership could not be verified.",ERROR_INSTALL_PACKAGE_REJECTED);return false;
    }
    if (!officialProducts.empty()) {
        SetFailure(ru?L"MSI SecureUxTheme появился после проверки плана; повторите установку.":L"SecureUxTheme MSI appeared after plan validation; retry installation.",ERROR_RETRY);
        return false;
    }
    if (secureUxPackage_.alreadyInstalled) {
        SetFailure(L"SecureUxTheme MSI registration changed during setup; retry installation.",ERROR_RETRY);
        return false;
    }
    std::wstring error;
    // Recheck immediately before MSI to avoid implicitly repairing/upgrading a
    // product installed by another process while Glass was being installed.
    std::wstring currentCode,currentVersion;
    if (secureUxPackage_.path.empty() || !GlassSecureUxPackage::CheckLegacyRepackAbsent(error) ||
        !GlassMaintenanceCache::Identity(secureUxPackage_.path,currentCode,currentVersion) ||
        _wcsicmp(currentCode.c_str(),secureUxPackage_.productCode.c_str())!=0 ||
        currentVersion!=secureUxPackage_.version) {
        SetFailure(error.empty()?(ru?L"Оригинальный MSI SecureUxTheme не подготовлен или его идентификатор/версия изменились.":L"Official SecureUxTheme MSI was not prepared or its identity changed."):error,ERROR_INSTALL_PACKAGE_REJECTED);
        return false;
    }
    GlassInstallMsi::ProgressSink sink{};
    sink.label=labels_.secureUxTheme.empty()?L"SecureUxTheme":labels_.secureUxTheme;
    sink.base=9200; sink.span=300;
    sink.onProgress=[this](const std::wstring& label,int progress){SetStatus(label,progress);};
    // launchingPhase_ has closed UI cancellation. No INSTALLDIR, custom
    // properties, ARP rewriting or Glass product/root validation is applied.
    SetStatus(sink.label,9200);
    const auto log=InstallLog::NextMsiVerboseLogPath(L"SecureUxTheme-original");
    if (!log.empty()) MsiEnableLogW(INSTALLLOGMODE_VERBOSE,log.c_str(),INSTALLLOGATTRIBUTES_FLUSHEACHLINE);
    const UINT result=GlassInstallMsi::InstallProduct(secureUxPackage_.path,L"REBOOT=ReallySuppress",&sink);
    InstallLog::LineFormat(L"Original SecureUxTheme MSI returned %u (ProductCode %s)",result,secureUxPackage_.productCode.c_str());
    if (!GlassInstallMsi::MsiSuccess(result)) {
        SetFailure(ru?L"Не удалось установить оригинальный MSI SecureUxTheme.":
                      L"Could not install the original SecureUxTheme MSI.",result);
        return false;
    }
    // Do not put the vendor ProductCode into Glass rollback's fixed-root list.
    // There are no failing/cancellable steps after this vendor transaction.
    bool restart=result==ERROR_SUCCESS_REBOOT_REQUIRED || result==ERROR_SUCCESS_REBOOT_INITIATED;
    auto after=GlassInstallUtil::SecureUxThemeState::Conflict;
    if(GlassInstallUtil::QuerySecureUxThemeState(x64Dir_,x86Dir_,&after,&error) && result!=ERROR_SUCCESS_REBOOT_INITIATED) {
        const bool runtimeRestart=GlassProducts::SecureUxRestartAfter(true,after,restart);
        bool pending=true;
        if(runtimeRestart || !restart || (GlassInstallUtil::QuerySecureUxPendingFileOperations(pending)&&!pending))restart=runtimeRestart;
    }
    InstallLog::LineFormat(L"SecureUxTheme after install: state=%u; restart=%d; MSI=%u",static_cast<unsigned>(after),restart?1:0,result);
    if(restart)(void)GlassMaintenanceReboot::Pinned(true);
    if (restart)
        AddFinishWarning(ru?L"SecureUxTheme: требуется перезагрузка Windows":
                            L"SecureUxTheme: Windows restart required",
            ru?L"MSI установлен. Автоматическая перезагрузка подавлена; перезагрузите Windows самостоятельно.":
               L"The MSI is installed. Automatic restart was suppressed; restart Windows yourself.");
    return true;
}

// Build from freshly inspected products, prepared media and rollback baselines,
// before any host stop or helper MSI. UI checkbox state alone cannot authorize it.
bool InstallEngine::BuildExecutionPlan() noexcept {
    namespace P=GlassProductPlan;
    executionPlanReady_=false;
    openGlassResidualRemoval_=false;
    const auto refuse=[&](const wchar_t* detail)->bool {
        InstallLog::LineFormat(L"Execution plan preflight refused: %s",detail);
        if(!IsCancelRequested()) {
            const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
            SetFailure(ru?L"План установки нельзя безопасно выполнить. Продукты не изменены; откройте обслуживание повторно.":
                L"The installation plan cannot be executed safely. Products were not changed; reopen maintenance.",ERROR_INSTALL_PACKAGE_REJECTED);
        }
        return false;
    };
    try {
        if(IsCancelRequested())return false;
        if(selection_.removeMask && forceClean_)return refuse(L"Clean purge cannot accompany managed removal");
        if(selection_.removeMask & ~GlassProducts::Selection::kManagedRemovalMask)return refuse(L"Invalid managed removal mask");
        if(selection_.removeMask && selection_.secureUxTheme)
            return refuse(L"Vendor transaction and managed removal require separate operations");
        GlassProductInspect::Inputs input;
        input.os64=scanNativeX64_;input.cancelled=&cancelRequested_;input.inspectSources=false;
        const auto report=GlassProductInspect::Run(input);
        if(IsCancelRequested())return false;
        if(!report.ran)return refuse(L"Product inspection did not complete");
        struct Probe {
            P::Product product;const wchar_t* code;const wchar_t* legacy;
            const wchar_t* file;bool selected;const GlassProductInspect::ProductReport* report;
        };
        const Probe probes[]={
            {P::Product::Platform64,GlassInstallMsi::kProductPlatformX64,GlassInstallMsi::kLegacyPlatformX64,L"Glass-x64.msi",selection_.platform64,&report.x64},
            {P::Product::Platform32,GlassInstallMsi::kProductPlatformX86,GlassInstallMsi::kLegacyPlatformX86,L"Glass-x86.msi",selection_.platform32,&report.x86},
            {P::Product::Settings64,GlassInstallMsi::kProductSettingsX64,nullptr,L"GlassSettings-x64.msi",selection_.Settings64(scanNativeX64_),&report.settings64},
            {P::Product::Settings32,GlassInstallMsi::kProductSettingsX86,nullptr,L"GlassSettings-x86.msi",selection_.Settings32(scanNativeX64_),&report.settings32},
            {P::Product::OpenGlassResources,GlassInstallMsi::kProductOpenGlassResourcesX64,nullptr,L"OpenGlassResources-x64.msi",selection_.openGlassResources,&report.openGlassResources},
            {P::Product::OpenGlass,GlassInstallMsi::kProductOpenGlass,GlassInstallMsi::kLegacyOpenGlass,L"OpenGlass-x64.msi",selection_.openGlass,&report.og}
        };
        std::array<P::Facts,P::kProductCount> facts{};
        P::Request request;request.os64=scanNativeX64_;
        for(const auto& probe:probes) {
            if(IsCancelRequested())return false;
            const auto i=P::Index(probe.product);auto& fact=facts[i];
            fact.supported=P::SupportedOnOs(probe.product,scanNativeX64_);
            const bool removing=selection_.Removes(static_cast<unsigned>(i));
            if(removing&&probe.selected)return refuse(L"Contradictory apply/remove intents");
            request.selected[i]=probe.selected;
            if(removing)request.actions[i]=P::Action::Remove;
            if(probe.product==P::Product::OpenGlass && (probe.selected||(removing&&GlassOpenGlassFiles::HasReceipt(openGlassDir_)))) {
                // No transition from our retired MSI family, and no overwrite of
                // an independent upstream install. Ownership is a fixed-file receipt.
                if(MsiQueryProductStateW(probe.code)!=INSTALLSTATE_UNKNOWN ||
                   MsiQueryProductStateW(probe.legacy)!=INSTALLSTATE_UNKNOWN)
                    return refuse(L"Retired OpenGlass MSI is still registered");
                const auto installed=GlassOpenGlassFiles::Inspect(openGlassDir_);
                if(removing) {
                    if(!installed.present)return refuse(L"OpenGlass file receipt disappeared");
                    fact.presence=P::Presence::Installed;fact.condition=P::Condition::Healthy;
                    fact.ownership=P::Ownership::Managed;continue;
                }
                if(openGlassPackage_.dll.empty()||openGlassPackage_.host.empty()||openGlassPackage_.gui.empty())
                    return refuse(L"Upstream OpenGlass payload was not prepared");
                if(installed.present&&GlassOpenGlassRelease::PayloadUpdateAvailable(openGlassPackage_.version,installed.version))
                    return refuse(L"OpenGlass payload downgrade refused");
                fact.presence=installed.present?P::Presence::Installed:P::Presence::Absent;
                fact.condition=installed.present&&!installed.integrity?P::Condition::Damaged:P::Condition::Healthy;
                fact.version=installed.present&&GlassOpenGlassRelease::PayloadUpdateAvailable(installed.version,openGlassPackage_.version)?P::Version::Older:P::Version::Same;
                fact.source=P::Source::Verified;fact.ownership=P::Ownership::Managed;
                continue;
            }
            bool present=false,presenceKnown=true;
            std::array<bool,2> wasInstalled{};std::size_t codeIndex=0;
            for(const auto* code:{probe.code,probe.legacy}) {
                if(!code)continue;
                const auto state=MsiQueryProductStateW(code);
                if(state!=INSTALLSTATE_DEFAULT && state!=INSTALLSTATE_UNKNOWN)presenceKnown=false;
                wasInstalled[codeIndex++]=state==INSTALLSTATE_DEFAULT;
                present=present||state==INSTALLSTATE_DEFAULT;
            }
            fact.presence=!presenceKnown?P::Presence::Unknown:present?P::Presence::Installed:P::Presence::Absent;
            // Kept products do not need installation media or mutation authority.
            if(!probe.selected&&!removing)continue;
            if(!presenceKnown)return refuse(L"Ambiguous or foreign MSI registration");
            if(present!=probe.report->installed)return refuse(L"MSI presence changed during inspection");
            fact.condition=!present?P::Condition::Healthy:!probe.report->integrityKnown?P::Condition::Unverifiable:
                probe.report->damaged?P::Condition::Damaged:P::Condition::Healthy;
            if(removing) {
                const bool residual=probe.product==P::Product::OpenGlass && !present &&
                    probe.report->residual && probe.report->residualSafe;
                if(!present&&!residual)return refuse(L"Removal target disappeared");
                for(const auto* code:{probe.code,probe.legacy}) {
                    if(!code)continue;
                    std::wstring detail;
                    if(GlassInstallMsi::CheckProductContexts(code,detail,&cancelRequested_)!=GlassInstallMsi::ProductPathCheck::Safe ||
                       (MsiQueryProductStateW(code)==INSTALLSTATE_DEFAULT && !RecheckSnapshottedProduct(code)))return refuse(L"Removal baseline or context changed");
                }
                if(!RecheckRelatedProducts(probe.code))return false;
                if(residual) {
                    // This authorizes ONLY auxiliary cleanup. No MSI presence or
                    // rollback package is fabricated for the missing product.
                    fact.presence=P::Presence::Installed;
                    openGlassResidualRemoval_=true;
                }
                fact.condition=P::Condition::Healthy; // Snapshot authorizes MSI identity/paths, not file integrity.
                fact.ownership=P::Ownership::Managed;
                continue;
            }
            std::wstring targetCode,targetVersion;
            if(!GlassMaintenanceCache::Identity(tempDir_+L"\\"+probe.file,targetCode,targetVersion) ||
               _wcsicmp(targetCode.c_str(),probe.code)!=0)return refuse(L"Prepared MSI identity does not match its row");
            GlassPackageRules::Version target{};
            if(!GlassPackageRules::ParseVersion(std::string(targetVersion.begin(),targetVersion.end()),target))
                return refuse(L"Invalid prepared MSI version");
            fact.version=P::Version::Same;codeIndex=0;
            for(const auto* code:{probe.code,probe.legacy}) {
                if(!code)continue;
                std::wstring detail;
                if(GlassInstallMsi::CheckProductContexts(code,detail,&cancelRequested_)!=GlassInstallMsi::ProductPathCheck::Safe)
                    return refuse(L"MSI context is not safe");
                const auto currentState=MsiQueryProductStateW(code);
                if((currentState!=INSTALLSTATE_DEFAULT && currentState!=INSTALLSTATE_UNKNOWN) ||
                   (currentState==INSTALLSTATE_DEFAULT)!=wasInstalled[codeIndex++])
                    return refuse(L"MSI presence changed during preflight");
                if(currentState!=INSTALLSTATE_DEFAULT)continue;
                if(!RecheckSnapshottedProduct(code))return false;
                bool baseline=false;
                for(const auto& old:rollbackSession_.oldProducts) {
                    if(_wcsicmp(old.productCode.c_str(),code)!=0)continue;
                    std::wstring oldCode,oldVersion;GlassPackageRules::Version installed{};
                    if(!GlassMaintenanceCache::Identity(old.cachedMsiPath,oldCode,oldVersion) ||
                       _wcsicmp(oldCode.c_str(),code)!=0 ||
                       !GlassPackageRules::ParseVersion(std::string(oldVersion.begin(),oldVersion.end()),installed))
                        return refuse(L"Installed MSI baseline identity is invalid");
                    const int comparison=GlassPackageRules::Compare(installed,target);
                    if(comparison>0)fact.version=P::Version::Newer;
                    else if(comparison<0 && fact.version!=P::Version::Newer)fact.version=P::Version::Older;
                    baseline=true;break;
                }
                if(!baseline)return refuse(L"Installed product has no rollback baseline");
            }
            if(!RecheckRelatedProducts(probe.code))return false;
            // Acquisition verified this private staged MSI; context/root/cache
            // guards above authorize only these fixed Glass product families.
            fact.source=P::Source::Verified;fact.ownership=P::Ownership::Managed;
        }
        const unsigned settingsRisk=P::SettingsRemovalRiskMask(selection_.Removes(2),selection_.Removes(5),
            facts[0].presence==P::Presence::Installed,facts[1].presence==P::Presence::Installed,
            selection_.Removes(0),selection_.Removes(1));
        if(settingsRisk & ~selection_.confirmedSettingsRemovalRisk)return refuse(L"Settings removal needs architecture-specific risk consent");
        // Represent the vendor row in the same discovered plan, but keep its
        // transaction commit-last: no Glass INSTALLDIR, rollback or file cleanup.
        const auto sutIndex=P::Index(P::Product::SecureUxTheme);
        auto& sut=facts[sutIndex];sut.supported=true;
        sut.presence=report.secureUxTheme.installed?P::Presence::Installed:P::Presence::Absent;
        if(selection_.secureUxTheme && selection_.secureUxThemeRemove)
            return refuse(L"Contradictory SecureUxTheme intents");
        if(selection_.secureUxThemeRemove) {
            std::wstring error;auto state=GlassInstallUtil::SecureUxThemeState::Conflict;
            const bool manual=selection_.secureUxRemovalWithoutSettings;
            const bool nativeUpdate=scanNativeX64_?selection_.Settings64(true):selection_.Settings32(false);
            const bool runtimeValid=manual?GlassInstallUtil::GlassSettingsAbsentForSecureUxRemoval():nativeUpdate?true:
                (GlassInstallUtil::QuerySecureUxThemeState(x64Dir_,x86Dir_,&state,&error)&&GlassProducts::SecureUxKnownRemoval(state)&&
                 (GlassProducts::SecureUxUnloadedRemoval(state)||secureUxLoadedRemoval_)&&
                 (!secureUxLoadedRemoval_||secureUxRemovalProfiles_.RecheckLoadedRemoval(error)));
            if(!report.secureUxTheme.installed || report.officialSecureUxThemeCode.empty() ||
               _wcsicmp(report.officialSecureUxThemeCode.c_str(),secureUxRemovalCode_.c_str())!=0 ||
               !secureUxRemoval_.ready || !runtimeValid ||
               !GlassSecureUxPackage::RecheckRemoval(tempDir_,cancelRequested_,secureUxRemoval_,error))
                return refuse(L"SecureUxTheme removal baseline, consent or profile/runtime state changed");
            // Authority is for this exact vendor MSI, not ownership of its files.
            sut.ownership=P::Ownership::Managed;sut.condition=P::Condition::Healthy;
            // Manual/deferred is NOT a claim that the hook is inactive. Active
            // keeps the plan's style-reset requirement conservative; real checks
            // occur in the vendor adapter, or are explicitly waived by manual consent.
            sut.activity=(!manual&&!nativeUpdate&&GlassProducts::SecureUxUnloadedRemoval(state))?P::Activity::Inactive:P::Activity::Active;
            request.actions[sutIndex]=P::Action::Remove;
        }else if(selection_.secureUxTheme) {
            std::vector<std::wstring> installed;std::wstring error;
            if(!GlassSecureUxPackage::QueryInstallPresence(installed,cancelRequested_,error) ||
               !GlassSecureUxPackage::CheckLegacyRepackAbsent(error) ||
               !installed.empty() || report.secureUxTheme.installed || secureUxPackage_.alreadyInstalled)
                return refuse(L"SecureUxTheme installation presence or ownership changed");
            std::wstring code,version;GlassPackageRules::Version parsed{};
            if(secureUxPackage_.path.empty() ||
               !GlassMaintenanceCache::Identity(secureUxPackage_.path,code,version) ||
               _wcsicmp(code.c_str(),secureUxPackage_.productCode.c_str())!=0 ||
               version!=secureUxPackage_.version ||
               !GlassPackageRules::ParseVersion(std::string(version.begin(),version.end()),parsed))
                return refuse(L"Prepared SecureUxTheme MSI identity changed");
            sut.ownership=P::Ownership::Managed;sut.condition=P::Condition::Healthy;
            sut.source=P::Source::Verified;sut.version=P::Version::Same;
            request.actions[sutIndex]=P::Action::Install;
        }
        if(IsCancelRequested())return false;
        if(selection_.openGlassResources&&!preparingOfficialResources_) {
            GlassOpenGlassInstall::Record upstream;std::wstring detail;
            if(!GlassOpenGlassInstall::Query(upstream,detail) || !upstream.installed ||
               !GlassInstallUtil::IsSafeInstallPath(upstream.directory+L"\\OpenGlass.dll"))
                return refuse(L"OpenGlass Resources needs a verified existing upstream OpenGlass install");
            const auto attrs=GetFileAttributesW((upstream.directory+L"\\OpenGlass.dll").c_str());
            if(attrs==INVALID_FILE_ATTRIBUTES || (attrs&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))
                return refuse(L"OpenGlass Resources prerequisite DLL unavailable");
            facts[P::Index(P::Product::OpenGlass)].presence=P::Presence::Installed;
        }
        if(selection_.openGlassResources&&preparingOfficialResources_) {
            if(openGlassPackage_.version.empty())return refuse(L"Official OG prerequisite was not prepared");
            facts[P::Index(P::Product::OpenGlass)].presence=P::Presence::Installed;
        }
        executionPlan_=P::Build(facts,request);
        if(!executionPlan_.valid) {
            for(std::size_t i=0;i<P::kProductCount;++i)if(executionPlan_.rows[i].error!=P::Error::None)
                InstallLog::LineFormat(L"Execution plan row=%u error=%u",static_cast<unsigned>(i),static_cast<unsigned>(executionPlan_.rows[i].error));
            return refuse(L"Product policy rejected the discovered facts");
        }
        executionPlanReady_=true;
        return true;
    }catch(...){return refuse(L"Cannot allocate execution plan");}
}

bool InstallEngine::PrepareFullRemovalCleanup() noexcept {
    if(!selection_.cleanupUserData)return true;
    const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
    try {
        unsigned installed=0;
        const wchar_t* codes[]={GlassInstallMsi::kProductPlatformX64,GlassInstallMsi::kProductPlatformX86,
            GlassInstallMsi::kProductSettingsX64,GlassInstallMsi::kProductOpenGlass,nullptr,GlassInstallMsi::kProductSettingsX86,GlassInstallMsi::kProductOpenGlassResourcesX64};
        for(unsigned i=0;i<std::size(codes);++i)if(codes[i]&&GlassInstallMsi::IsProductInstalled(codes[i]))installed|=1u<<i;
        if(GlassInstallMsi::IsProductInstalled(GlassInstallMsi::kLegacyPlatformX64))installed|=1;
        if(GlassInstallMsi::IsProductInstalled(GlassInstallMsi::kLegacyPlatformX86))installed|=2;
        if(GlassInstallMsi::IsProductInstalled(GlassInstallMsi::kLegacyOpenGlass))installed|=8;
        GlassInstallUtil::InstallRoots cleanupRoots{};
        if(!GlassInstallUtil::ResolveInstallRoots(cleanupRoots))return false;
        if(GlassOpenGlassFiles::HasReceipt(cleanupRoots.openGlass)||GlassOpenGlassInstall::HasProduct())installed|=8;
        std::vector<std::wstring> vendor;std::wstring detail;
        (void)vendor;
        if(!GlassProducts::PlatformCleanupSelected(selection_,installed)||
           !fullRemovalProfiles_.PreflightRegistryCleanup(detail)) {
            SetFailure(std::wstring(ru?L"Предварительная проверка очистки отклонена: ":L"Registry cleanup preflight refused: ")+detail,ERROR_ACCESS_DENIED);return false;
        }
        return true;
    }catch(...){SetFailure(ru?L"Проверка доступности очистки не выполнена":L"Full-removal cleanup preflight unavailable",ERROR_NOT_ENOUGH_MEMORY);return false;}
}
void InstallEngine::FinishFullRemovalCleanup() noexcept {
    if(!selection_.cleanupUserData)return;
    const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
    try {
        std::wstring detail;
        const bool absent=MaintenanceProductAbsent(GlassInstallMsi::kProductPlatformX64)&&
            MaintenanceProductAbsent(GlassInstallMsi::kProductPlatformX86)&&
            MaintenanceProductAbsent(GlassInstallMsi::kLegacyPlatformX64)&&
            MaintenanceProductAbsent(GlassInstallMsi::kLegacyPlatformX86);
        const bool ok=absent&&fullRemovalProfiles_.CleanupRegistry(detail);
        if(!ok)AddFinishWarning(ru?L"Пользовательские данные очищены не полностью":L"User data cleanup incomplete",
            absent?detail:(ru?L"Продукты ещё зарегистрированы; очистка не выполнялась.":L"Products remain registered; cleanup was not performed."));
        else InstallLog::Line(L"Platform registry cleanup completed");
    }catch(...){AddFinishWarning(ru?L"Данные очищены не полностью":L"User data cleanup incomplete",ru?L"Ошибка очистки после удаления; продукты не восстанавливались.":L"Cleanup failed after product removal; products were not restored.");}
}

bool InstallEngine::PrepareManagedRemoval() noexcept {
    if(!selection_.removeMask)return true;
    std::wstring detail;
    const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
    const auto nativeArch=scanNativeX64_?GP::PlatformArch::X64:GP::PlatformArch::X86;
    const bool removesNative=selection_.Removes(scanNativeX64_?0u:1u);
    const bool preparesNative=scanNativeX64_?selection_.Settings64(true):selection_.Settings32(false);
    if(removesNative&&!preparesNative) {
        const auto helper=GlassInstallUtil::NativeDllForArch(nativeArch,x64Dir_,x86Dir_);
        if(helper.empty() || !GlassInstallUtil::PeMatches(helper,scanNativeX64_?IMAGE_FILE_MACHINE_AMD64:IMAGE_FILE_MACHINE_I386,GlassInstallUtil::kInstallerShellExport)) {
            SetFailure(ru?L"Для безопасного удаления Platform нужен совместимый GlassSettings той же разрядности. Добавьте его установку или восстановление в план, затем повторите операцию.":
                L"Safe Platform removal requires compatible GlassSettings of the same architecture. Add its installation or repair to the plan and retry.",ERROR_INSTALL_PACKAGE_REJECTED);
            return false;
        }
    }
    const bool survives64=selection_.Settings64(scanNativeX64_) ||
        (GlassInstallMsi::IsProductInstalled(GlassInstallMsi::kProductSettingsX64)&&!selection_.Removes(2));
    const bool survives32=selection_.Settings32(scanNativeX64_) ||
        (GlassInstallMsi::IsProductInstalled(GlassInstallMsi::kProductSettingsX86)&&!selection_.Removes(5));
    const bool removingSettings=selection_.Removes(2)||selection_.Removes(5);
    const bool removeShortcut=removingSettings&&!survives64&&!survives32;
    if(!GlassInstallRemoval::Prepare(removalSession_,selection_.Removes(1),selection_.Removes(3),removeShortcut,
        x64Dir_,x86Dir_,openGlassDir_,rollbackSession_.workspaceDir,&cancelRequested_,ru,detail,selection_.Removes(0))) {
        if(!IsCancelRequested())SetFailure(detail,ERROR_INSTALL_PACKAGE_REJECTED);
        return false;
    }
    if(removingSettings&&!removeShortcut &&
       !GlassInstallRemoval::PrepareSettingsShortcutRetarget(removalSession_,survives64?x64Dir_:x86Dir_,
           x64Dir_,x86Dir_,rollbackSession_.workspaceDir,ru,detail)) {
        SetFailure(detail.empty()?(ru?L"Не удалось безопасно подготовить ярлык оставшегося Settings.":
            L"Could not safely prepare the retained Settings shortcut."):detail,ERROR_INSTALL_PACKAGE_REJECTED);
        return false;
    }
    return true;
}

bool InstallEngine::RemoveManagedProduct(GlassProductPlan::Product product) noexcept {
    namespace P=GlassProductPlan;
    const unsigned row=static_cast<unsigned>(P::Index(product));
    if(!executionPlanReady_ || !selection_.Removes(row) ||
       executionPlan_.rows[row].action!=P::Action::Remove || IsCancelRequested())return false;
    if(product==P::Product::OpenGlass&&GlassOpenGlassFiles::HasReceipt(openGlassDir_)) {
        try {
        if(MsiQueryProductStateW(GlassInstallMsi::kProductOpenGlass)!=INSTALLSTATE_UNKNOWN ||
           MsiQueryProductStateW(GlassInstallMsi::kLegacyOpenGlass)!=INSTALLSTATE_UNKNOWN) {
            SetFailure(L"OpenGlass MSI registration changed",ERROR_INSTALL_PACKAGE_REJECTED);return false;
        }
        openGlassFiles_=std::make_shared<GlassOpenGlassFiles::Transaction>();
        const auto transaction=openGlassFiles_;
        rollbackSession_.restoreOpenGlassFiles=[transaction]{return transaction->Rollback();};
        std::wstring error;
        if(transaction->Begin(openGlassDir_,error)&&transaction->Remove(error))return true;
        SetFailure(error,ERROR_INSTALL_FAILURE);return false;
        }catch(...){SetFailure(L"OpenGlass removal allocation failed",ERROR_NOT_ENOUGH_MEMORY);return false;}
    }
    const wchar_t* current=nullptr;const wchar_t* legacy=nullptr;const wchar_t* title=nullptr;
    switch(product) {
    case P::Product::Platform64:current=GlassInstallMsi::kProductPlatformX64;legacy=GlassInstallMsi::kLegacyPlatformX64;title=L"Platform x64";break;
    case P::Product::Platform32:current=GlassInstallMsi::kProductPlatformX86;legacy=GlassInstallMsi::kLegacyPlatformX86;title=L"Platform x86";break;
    case P::Product::Settings64:current=GlassInstallMsi::kProductSettingsX64;title=L"GlassSettings x64";break;
    case P::Product::OpenGlassResources:current=GlassInstallMsi::kProductOpenGlassResourcesX64;title=L"OpenGlass Resources";break;
    case P::Product::Settings32:current=GlassInstallMsi::kProductSettingsX86;title=L"GlassSettings x86";break;
    case P::Product::OpenGlass:current=GlassInstallMsi::kProductOpenGlass;legacy=GlassInstallMsi::kLegacyOpenGlass;title=L"OpenGlass";break;
    default:return false;
    }
    if(!RecheckRelatedProducts(current))return false;
    bool hasBaseline=false;
    for(const auto* code:{current,legacy}) {
        if(!code)continue;
        bool expected=false;
        for(const auto& old:rollbackSession_.oldProducts)
            if(_wcsicmp(old.productCode.c_str(),code)==0){expected=true;break;}
        hasBaseline=hasBaseline||expected;
        std::wstring detail;
        if(GlassInstallMsi::IsProductInstalled(code)!=expected ||
           GlassInstallMsi::CheckProductContexts(code,detail,&cancelRequested_)!=GlassInstallMsi::ProductPathCheck::Safe ||
           (expected&&!RecheckSnapshottedProduct(code))) {
            if(!IsCancelRequested())SetFailure(L"Removal registration changed after snapshot",ERROR_INSTALL_PACKAGE_REJECTED);
            return false;
        }
    }
    if(!hasBaseline) {
        if(product==P::Product::OpenGlass && openGlassResidualRemoval_) {
            // Family/context/presence were rechecked above. Prepare captured
            // service configuration and task XML before stopping hosts; Commit
            // rechecks those exact objects and deletes the service last.
            std::wstring detail;
            if(GlassInstallRemoval::ValidateOpenGlassPaths(openGlassDir_,&cancelRequested_,
                PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN,detail))return true;
            if(!IsCancelRequested())SetFailure(detail,ERROR_INSTALL_PACKAGE_REJECTED);
            return false;
        }
        SetFailure(L"No installed removal baseline",ERROR_INSTALL_PACKAGE_REJECTED);return false;
    }
    // The native helper is still installed here: all Settings removals are last.
    // Do not unregister a foreign root or bypass an unavailable helper.
    if(row==(scanNativeX64_?0u:1u)) {
        bool conflict=false;std::wstring detail;
        if(!GlassInstallUtil::InvokeShellExtRegistration(false,x64Dir_,x86Dir_,&shellRemovedForMaintenance_,&conflict,&detail)) {
            SetFailure(detail,ERROR_INSTALL_FAILURE);return false;
        }
    }
    GlassInstallMsi::ProgressSink sink{};
    const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
    sink.label=std::wstring(ru?L"Удаление ":L"Removing ")+title;
    sink.base=2200;sink.span=6700;sink.cancelRequested=&cancelRequested_;
    sink.onError=[this](const std::wstring& text){RememberMsiMessage(text.c_str());};
    sink.onProgress=[this](const std::wstring& text,int value){SetStatus(text,value);};
    for(const auto* code:{current,legacy}) {
        if(!code)continue;
        if(IsCancelRequested())return false;
        if(!GlassInstallMsi::IsProductInstalled(code))continue;
        if(!RecheckSnapshottedProduct(code))return false;
        GlassInstallRollback::NoteModified(rollbackSession_,code); // Before MSI, including its failure path.
        const auto log=InstallLog::NextMsiVerboseLogPath(sink.label.c_str());
        if(!log.empty())MsiEnableLogW(INSTALLLOGMODE_VERBOSE|INSTALLLOGMODE_ERROR|INSTALLLOGMODE_WARNING,log.c_str(),INSTALLLOGATTRIBUTES_FLUSHEACHLINE);
        UINT result=ERROR_SUCCESS;
        const bool removed=GlassInstallMsi::UninstallProduct(code,&sink,&result);
        InstallLog::LineFormat(L"Managed removal %s exit=%u",code,result);
        if(!removed) {
            if(result==ERROR_INSTALL_USEREXIT)cancelRequested_.store(true,std::memory_order_release);
            if(!IsCancelRequested())SetFailure(sink.label,result);
            return false;
        }
        std::wstring contextDetail;
        if(GlassInstallMsi::IsProductInstalled(code) ||
           GlassInstallMsi::CheckProductContexts(code,contextDetail,&cancelRequested_)!=GlassInstallMsi::ProductPathCheck::Safe) {
            SetFailure(L"MSI removal did not safely remove the selected registration",ERROR_INSTALL_FAILURE);return false;
        }
        if(result==ERROR_SUCCESS_REBOOT_REQUIRED || result==ERROR_SUCCESS_REBOOT_INITIATED)
            (void)GlassMaintenanceReboot::Pinned(true);
        if(result==ERROR_SUCCESS_REBOOT_REQUIRED || result==ERROR_SUCCESS_REBOOT_INITIATED)
            AddFinishWarning(ru?L"Для завершения удаления нужна перезагрузка":L"Restart required to complete removal",title);
    }
    return true;
}

bool InstallEngine::RunMsiSequence() noexcept {
    const bool is64 = scanNativeX64_;
    const bool ru = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN;
    struct Step {
        GlassProductPlan::Product product;
        std::wstring msiFile;
        std::wstring dir;
        std::wstring label;
    };
    // Only the checked products are touched. An installed product that was
    // left unchecked is kept as it is; vendor removal has its separate final phase.
    std::vector<Step> steps;
    namespace P=GlassProductPlan;
    if(!executionPlanReady_ || !executionPlan_.valid)return false;
    // Original SUT uses the private vendor adapter, never the Glass MSI phase.
    // Mixed removal already committed it before any Settings removal.
    auto plan=executionPlan_;
    plan.rows[P::Index(P::Product::SecureUxTheme)].action=P::Action::Keep;
    for(auto product:GlassMaintenanceExecutor::kInstallOrder) {
        if(!P::IsMutation(plan.rows[P::Index(product)].action))continue;
        if(product==P::Product::Platform64)steps.push_back({P::Product::Platform64,L"Glass-x64.msi",x64Dir_,labels_.platform64});
        if(product==P::Product::Platform32)steps.push_back({P::Product::Platform32,L"Glass-x86.msi",x86Dir_,labels_.platform32});
        if(product==P::Product::OpenGlassResources) {
            std::wstring root;
            if(!GlassInstallUtil::ResolveOpenGlassResourcesRoot(root)){SetFailure(L"OpenGlass Resources root unavailable",GetLastError());return false;}
            steps.push_back({P::Product::OpenGlassResources,L"OpenGlassResources-x64.msi",root,L"OpenGlass Resources"});
        }

    }

    constexpr int kProgressStart = 2200;
    constexpr int kProgressEnd = 9000;
    const int span = steps.empty() ? 0 : (kProgressEnd - kProgressStart) / static_cast<int>(steps.size());
    int base = kProgressStart;
    const auto applyStep=[&](const Step& step)->bool {
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
        return true;
    };
    const auto scheduled=GlassMaintenanceExecutor::RunPhase(plan,GlassMaintenanceExecutor::Phase::Products,
        [&](P::Product product,P::Action action)->unsigned {
            if(product==P::Product::OpenGlass && action!=P::Action::Remove) {
                try {
                if(MsiQueryProductStateW(GlassInstallMsi::kProductOpenGlass)!=INSTALLSTATE_UNKNOWN ||
                   MsiQueryProductStateW(GlassInstallMsi::kLegacyOpenGlass)!=INSTALLSTATE_UNKNOWN) {
                    SetFailure(L"OpenGlass MSI registration changed",ERROR_INSTALL_PACKAGE_REJECTED);return ERROR_INSTALL_FAILURE;
                }
                openGlassFiles_=std::make_shared<GlassOpenGlassFiles::Transaction>();
                const auto transaction=openGlassFiles_;
                rollbackSession_.restoreOpenGlassFiles=[transaction]{return transaction->Rollback();};
                std::wstring error;
                const bool applied=transaction->Begin(openGlassDir_,error)&&
                    transaction->Apply(openGlassPackage_.version,openGlassPackage_.milComp,
                        {openGlassPackage_.dll,openGlassPackage_.host,openGlassPackage_.gui},error)&&
                    GlassInstallUtil::WriteOpenGlassInstallPath(x64Dir_,openGlassDir_);
                if(!applied){SetFailure(error.empty()?L"Could not apply upstream OpenGlass files":error,ERROR_INSTALL_FAILURE);return ERROR_INSTALL_FAILURE;}
                return ERROR_SUCCESS;
                }catch(...){SetFailure(L"OpenGlass file transaction allocation failed",ERROR_NOT_ENOUGH_MEMORY);return ERROR_INSTALL_FAILURE;}
            }
            if(action==P::Action::Remove)return RemoveManagedProduct(product)?ERROR_SUCCESS:
                IsCancelRequested()?ERROR_INSTALL_USEREXIT:ERROR_INSTALL_FAILURE;
            for(const auto& step:steps)if(step.product==product)
                return applyStep(step)?ERROR_SUCCESS:
                    IsCancelRequested()?ERROR_INSTALL_USEREXIT:ERROR_INSTALL_FAILURE;
            return ERROR_INVALID_PARAMETER;
        });
    if(!scheduled.ok) {
        InstallLog::LineFormat(L"MSI phase failed: error=%u attempted=0x%X completed=0x%X",
            scheduled.error,scheduled.attemptedMask,scheduled.completedMask);
        return false; // The worker retains the failed row's snapshot/journal for rollback.
    }

    // Do not replace an existing customised shortcut without a backup.
    if (selection_.glassSettings && !prior_.shortcutExisted) {
        const std::wstring& gsRoot = selection_.Settings64(is64) ? x64Dir_ : x86Dir_;
        const std::wstring gsDir = GlassInstallUtil::TrimTrailingSlashes(gsRoot) + L"\\GlassSettings";
        (void)GlassInstallUtil::CreateStartMenuShortcut(gsDir + L"\\GlassSettings.exe", gsDir);
    }

    InstallLog::Line(L"InstallEngine: planned product actions completed (MSI or verified auxiliary removal)");
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
    if(selection_.platform64||selection_.platform32)SeedUserDefaults();

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

// Phase C. The worker has already crossed the commit boundary (including
// the final original vendor MSI, when selected). Nothing in this routine may
// fail the install. Each problem is logged and shown as a finish note.
bool InstallEngine::EnsureOpenGlassBeforePlatform() noexcept {
    try {
        GlassOpenGlassInstall::Record record;std::wstring error;
        if(!GlassOpenGlassInstall::Query(record,error))return false;
        if(!record.installed)return true; // No service/directory-only product inference.
        if(!GlassInstallRemoval::ValidateOpenGlassPaths(record.directory,nullptr,
            PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN,error)) {
            InstallLog::LineFormat(L"Startup order: OpenGlass path validation refused: %s",error.c_str());return false;
        }
        bool exists=false;DWORD startType=SERVICE_DEMAND_START;
        if(!GlassInstallHost::QueryOpenGlassServiceStartType(exists,startType)||!exists)return false;
        if(startType==SERVICE_DISABLED) {
            InstallLog::Line(L"Startup order: OpenGlass service is disabled; no configuration change or Platform launch");return false;
        }
        InstallLog::Line(L"Startup order: starting installed OpenGlass before Platform");
        if(!GlassInstallHost::StartOpenGlassServicePreservingStartType(startType))return false;
        const SC_HANDLE scm=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT);
        if(!scm)return false;
        const SC_HANDLE service=OpenServiceW(scm,L"OpenGlassHost",SERVICE_QUERY_STATUS);
        CloseServiceHandle(scm);
        if(!service)return false;
        struct CloseService {SC_HANDLE value;~CloseService(){CloseServiceHandle(value);}} close{service};
        const ULONGLONG start=GetTickCount64();
        do {
            SERVICE_STATUS_PROCESS status{};DWORD needed=0;
            if(!QueryServiceStatusEx(service,SC_STATUS_PROCESS_INFO,reinterpret_cast<BYTE*>(&status),sizeof(status),&needed))return false;
            if(status.dwCurrentState==SERVICE_RUNNING) {
                InstallLog::Line(L"Startup order: OpenGlass SERVICE_RUNNING confirmed; Platform may start");return true;
            }
            if(status.dwCurrentState!=SERVICE_START_PENDING)return false;
            Sleep(200);
        }while(GetTickCount64()-start<15000);
        InstallLog::Line(L"Startup order: OpenGlass startup timed out; Platform remains stopped");
    }catch(...){}
    return false;
}

void InstallEngine::LaunchSelectedProducts() noexcept {
    const bool is64 = scanNativeX64_;
    const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
    if(GlassProducts::HasPackageApply(selection_)) {
        // Publish the actual acquired sources, not stale embedded packages. Keep
        // sources for unmodified installed products in Current across partial updates.
        const wchar_t* names[]={L"Glass-x64.msi",L"Glass-x86.msi",L"GlassSettings-x64.msi",
            L"GlassSettings-x86.msi",is64?L"SecureUxTheme_x64.msi":L"SecureUxTheme_x86.msi",L"OpenGlassResources-x64.msi"};
        std::vector<std::wstring> sources;
        sources.reserve(std::size(names));
        for(const auto* name:names) {
            const auto acquired=tempDir_+L"\\"+name;
            sources.push_back(GetFileAttributesW(acquired.c_str())!=INVALID_FILE_ATTRIBUTES?
                acquired:GlassMaintenanceCache::CurrentFile(name));
        }
        std::vector<GlassMaintenanceCache::Blob> cacheBlobs;
        constexpr unsigned sourceRows[]={0,1,2,5,4,6};
        for(std::size_t i=0;i<std::size(names);++i)if(!sources[i].empty() && !selection_.Removes(sourceRows[i]) &&
            !(mixedOpenGlass_&&!selection_.openGlassResources&&!officialResourcesRollback_.workspaceDir.empty()&&sourceRows[i]==6))
            cacheBlobs.push_back({0,names[i],sources[i].c_str(),sources[i]==tempDir_+L"\\"+names[i]});
        std::wstring cacheFailure;
        std::vector<std::wstring> needsOriginal;
        if(!GlassMaintenanceCache::Publish(instance_,cacheBlobs.data(),cacheBlobs.size(),IDR_MAINTENANCE_UNINS,&cacheFailure,&needsOriginal))
            AddFinishWarning(ru?L"Источники обслуживания сохранены не полностью":L"Maintenance sources were not fully saved",
                (ru?L"Не удалось полностью сохранить источники обслуживания продуктов. Это не ошибка обновления оболочки или запуска компонентов. Сохраните исходный установщик и журнал.\n":
                    L"Product maintenance sources were not fully saved. This is not a controller update or component startup failure. Keep the original installer and log.\n")+cacheFailure);
        if(!needsOriginal.empty()) {
            std::wstring detail=ru?
                L"Для восстановления следующих продуктов нужен проверенный исходный MSI с полным содержимым поставки. Существующие кандидаты сохранены, но не зарегистрированы как доверенные источники. Это не ошибка сохранения оболочки и не отмена выполненной операции:":
                L"Repair of the following products needs a verified original MSI and its complete source payload. Existing candidates were retained but were not registered as trusted sources. This is not a controller publication failure or a rollback of the completed operation:";
            for(const auto& source:needsOriginal)detail+=L"\n"+source;
            AddFinishWarning(ru?L"Не для всех продуктов доступен источник восстановления":L"Repair sources are unavailable for some products",detail);
        }
    } else {
        InstallLog::Line(L"Removal-only: maintenance cache left unchanged; no controller rotation or publication");
    }
    if(!GlassInstallMsi::RefreshManagedArp(x64Dir_,x86Dir_,openGlassDir_))
        AddFinishWarning(ru?L"Записи обслуживания обновлены не полностью":L"Maintenance entries were not fully refreshed",
            ru?L"Не удалось обновить команды или ограничения удаления оставшихся продуктов. Подробности в журнале.":
               L"Commands or removal policy for remaining products could not be refreshed. See the log.");

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

    if (is64 && selection_.openGlass) {
        SetStatus(labels_.startOg, 9400);
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

    const bool platformLaunch=selection_.platform64||selection_.platform32||
        (hostsStopped_&&((prior_.x64WasRunning&&!selection_.Removes(0))||(prior_.x86WasRunning&&!selection_.Removes(1))));
    const bool ogReady=!platformLaunch||EnsureOpenGlassBeforePlatform();
    if(ogReady) {
    if (is64 && selection_.platform64)
        startArch(GP::PlatformArch::X64, x64Dir_, prior_.x64TaskExisted, prior_.x64TaskEnabled, prior_.x64WasRunning,
            labels_.start64, 9500, labels_.warnPlatform64Start, labels_.warnPlatform64Task);
    if (selection_.platform32)
        startArch(GP::PlatformArch::X86, x86Dir_, prior_.x86TaskExisted, prior_.x86TaskEnabled, prior_.x86WasRunning,
            labels_.start86, 9750, labels_.warnPlatform32Start, labels_.warnPlatform32Task);

    } else {
        AddFinishWarning(ru?L"Платформа не запущена":L"Platform was not started",
            ru?L"Не удалось подтвердить запуск OpenGlass. Запустите OpenGlass, затем платформу.":
                L"OpenGlass startup was not confirmed. Start OpenGlass before the platform.");
    }

    if (ogReady && !RestorePriorHosts(true)) {
        AddFinishWarning(labels_.warnPriorHosts.empty()
            ? L"A previously running component could not be restarted." : labels_.warnPriorHosts, labels_.warnPriorHostsHint);
    }
    // Removal must not recreate the native menu or require an already removed helper.
    if(!(is64?selection_.platform64:selection_.platform32))return;
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
        InstallLog::Line(L"Certificate: no additional trust requested (already trusted / page not applicable / declined); existing certificate stores unchanged");
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
    // Setup is x86; on x64 Windows a trusted installed x64 helper is required even
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
    if(auxiliaryOnly_) {
        const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
        SetStatus(ru?L"Сохранение состояния службы и задач OpenGlass":L"Saving OpenGlass service and task state",450);
        if(GlassInstallUtil::CreateSecureTempDirectory(L"GlassRollback",rollbackSession_.workspaceDir))return true;
        SetFailure(L"Could not create the auxiliary rollback workspace",GetLastError());return false;
    }
    SetStatus(labels_.backup.empty() ? L"…" : labels_.backup, 450);
    const bool ok = GlassInstallRollback::CreateSnapshot(
        rollbackSession_, x64Dir_, x86Dir_, openGlassDir_, selection_.openGlass||selection_.Removes(3),
        [this](int units) {
            SetStatus(labels_.backup, 450 + (units * 350) / 10000);
        }, &cancelRequested_, procedureMask_);
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
        if(arch==GP::PlatformArch::X64?!GlassProcedurePlan::Platform64(procedureMask_):!GlassProcedurePlan::Platform32(procedureMask_))continue;
        bool running=false;
        if (!QueryPlatformMutex(arch,running)) return false;
        GP::InvalidateGlassAutorunTaskCache();
        const bool exists=GP::GlassAutorunTaskExists(arch);
        const auto& root=arch==GP::PlatformArch::X64?x64Dir_:x86Dir_;
        if (!GlassInstallRemoval::ValidatePlatformTask(arch,root,exists,running,ru,error)) {
            InstallLog::LineFormat(L"Rollback refused to stop changed platform: %s",error.c_str());return false;
        }
    }
    if (GlassProcedurePlan::OpenGlass(procedureMask_) && !GlassInstallRemoval::ValidateOpenGlassPaths(openGlassDir_,nullptr,ru,error)) {
        InstallLog::LineFormat(L"Rollback refused to stop changed OpenGlass backend: %s",error.c_str());return false;
    }
    return GlassInstallHost::StopAllForUninstall(x64Dir_,x86Dir_,GlassProcedurePlan::Platform64(procedureMask_),GlassProcedurePlan::Platform32(procedureMask_),GlassProcedurePlan::OpenGlass(procedureMask_),nullptr,nullptr,nullptr,nullptr,nullptr,procedureMask_&36u);
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
    if(auxiliaryOnly_) {
        std::wstring detail;
        const bool ok=GlassInstallRemoval::Restore(removalSession_,PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN,detail);
        if(!ok)InstallLog::LineFormat(L"Auxiliary restore failed: %s",detail.c_str());
        return ok;
    }
    // The stop/uninstall stages have no reliable percentage. Publish marquee
    // before exposing the reverse flag so the UI never uses stale MSI progress.
    SetStatus(labels_.rollback.empty() ? L"…" : labels_.rollback, -1);
    progressReverse_.store(true,std::memory_order_release);
    const bool stopped=StopHostsForRollback();
    bool lifecycleOk=stopped;
    if (stopped) {
        if (shellRegisteredBySetup_ && !GlassInstallUtil::InvokeShellExtRegistration(false,x64Dir_,x86Dir_)) lifecycleOk=false;
        if (lifecycleOk && !RemoveCreatedHostsForRollback()) lifecycleOk=false;
        if (lifecycleOk && selection_.glassSettings && !prior_.shortcutExisted) GlassInstallUtil::DeleteStartMenuShortcut();
    }
    const bool filesOk = lifecycleOk && GlassInstallRollback::ExecuteRollback(
        rollbackSession_, [this](int units) {
            SetStatus(labels_.rollback, WizardProgress::RollbackRemaining(units));
        }, nonSettingsChangesAttempted_ ? GlassInstallRollback::RestoreScope::AllProducts
            : (earlySettingsAttemptMask_==3
                ? GlassInstallRollback::RestoreScope::SettingsBoth
                : (earlySettingsAttemptMask_&1)?GlassInstallRollback::RestoreScope::SettingsX64
                    :GlassInstallRollback::RestoreScope::SettingsX86));
    std::wstring removalDetail;
    const bool removalOk=filesOk && GlassInstallRemoval::Restore(removalSession_,PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN,removalDetail);
    bool shellOk=true;
    if(filesOk && shellRemovedForMaintenance_)
        shellOk=GlassInstallUtil::InvokeShellExtRegistration(true,x64Dir_,x86Dir_);
    const bool ok=lifecycleOk && filesOk && removalOk && shellOk;
    if(!removalOk)InstallLog::LineFormat(L"Removal rollback auxiliaries failed: %s",removalDetail.c_str());
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
    if(auxiliaryOnly_) {
        std::wstring detail;
        const bool ok=GlassInstallRemoval::Restore(removalSession_,PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN,detail);
        if(!ok)InstallLog::LineFormat(L"Auxiliary restore failed: %s",detail.c_str());
        return ok;
    }
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
        const wchar_t* known[]={GlassInstallMsi::kProductOpenGlassResourcesX64,GlassInstallMsi::kProductPlatformX64,GlassInstallMsi::kProductPlatformX86,
            GlassInstallMsi::kProductSettingsX64,GlassInstallMsi::kProductSettingsX86,
            GlassInstallMsi::kProductSecureUxThemeX64,GlassInstallMsi::kProductSecureUxThemeX86,
            GlassInstallMsi::kProductOpenGlass,GlassInstallMsi::kLegacyPlatformX64,
            GlassInstallMsi::kLegacyPlatformX86,GlassInstallMsi::kLegacyOpenGlass};
        constexpr unsigned rows[]={64,1,2,4,32,16,16,8,1,2,8};
        for (unsigned index=0;index<std::size(known);++index) {
            const auto* code=known[index];
            if(!(procedureMask_&rows[index]))continue;
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
            if (GlassProcedurePlan::OpenGlass(procedureMask_) && !openGlassDir_.empty()) RemoveDirectoryW(openGlassDir_.c_str());
            if (GlassProcedurePlan::Platform64(procedureMask_) && !x64Dir_.empty()) RemoveDirectoryW(x64Dir_.c_str());
            if (GlassProcedurePlan::Platform32(procedureMask_) && !x86Dir_.empty()) RemoveDirectoryW(x86Dir_.c_str());
            if (selection_.glassSettings && !prior_.shortcutExisted) GlassInstallUtil::DeleteStartMenuShortcut();
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
    const struct {const wchar_t* code;bool selected;} products[] = {
        {GlassInstallMsi::kProductPlatformX64,is64&&selection_.platform64},
        {GlassInstallMsi::kProductPlatformX86,selection_.platform32},
        {GlassInstallMsi::kProductOpenGlass,is64&&selection_.openGlass},
        {L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5061}",is64&&selection_.platform64},
        {L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5062}",selection_.platform32},
        {L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5063}",is64&&selection_.openGlass},
    };
    for (const auto& product : products) {
        if(!product.selected)continue;
        const auto* code=product.code;
        // Settings already ran in the helper phase. Unselected architectures
        // and SecureUxTheme are retained, including a clean reinstall request.
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
    if ((is64 && selection_.platform64 && !GlassInstallUtil::ClearPlatformInstallPath(true)) ||
        (selection_.platform32 && !GlassInstallUtil::ClearPlatformInstallPath(false)) ||
        (is64 && selection_.openGlass && !GlassInstallUtil::ClearOpenGlassInstallPath())) {
        SetFailure(labels_.purgeFailed.empty() ? L"Could not clear installation paths." : labels_.purgeFailed, GetLastError());
        return false;
    }
    purgeCompleted_ = true;
    SetStatus(labels_.purge, 2200);
    InstallLog::Line(L"Purge MSI registrations completed; user files retained");
    return true;
}
