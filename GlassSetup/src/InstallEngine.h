#pragma once
#include "InstallSupport/GlassInstallRollback.h"
#include "InstallSupport/GlassInstallRemoval.h"
#include "InstallSupport/GlassUserDefaults.h"
#include "InstallSupport/GlassSecureUxPackage.h"
#include "InstallSupport/GlassOpenGlassPackage.h"
#include "InstallSupport/GlassOpenGlassFiles.h"
#include "GlassProducts.h"
#include "InstallSupport/GlassProfileMaintenance.h"
#include "GlassProductPlan.h"
#include "GlassProcedurePlan.h"
#include "WizardUnlockState.h"
#include <windows.h>
#include <winsvc.h>
#include <atomic>
#include <functional>
#include <string>
#include <vector>
#include <GlassPlatformTask.h>

// Protected MSI acquisition/install with cooperative cancellation and Glass rollback.
// Original vendor MSI install/removal is a separate, last transaction.
class InstallEngine {
public:
    using StatusFn = std::function<void(const std::wstring& line, int percent)>;

    using ComponentSelection = GlassProducts::Selection;

    struct PhaseLabels {
        std::wstring stop64;
        std::wstring stop86;
        std::wstring stopOg;
        std::wstring unload;
        std::wstring forceUnload;
        std::wstring unlockProcesses;
        std::wstring start64;
        std::wstring start86;
        std::wstring startOg;
        std::wstring extract;
        std::wstring platform64;
        std::wstring platform32;
        std::wstring openGlass;
        std::wstring finalize;
        std::wstring backup;
        std::wstring glassSettings64;
        std::wstring glassSettings86;
        std::wstring secureUxTheme;
        std::wstring warnSecureUxTheme;
        std::wstring warnSecureUxThemeHint;
        std::wstring warnSecureUxThemeRestart;
        std::wstring rollback;
        std::wstring purge;
        std::wstring purgeFailed;
        std::wstring platformDidNotStart;
        std::wstring shellRegisterFailed;
        std::wstring rollbackFailed;
        std::wstring rollbackRestored;
        std::wstring rollbackRemoved;
        std::wstring nativeScanFailed;
        std::wstring snapshotFailed;
        std::wstring diskFull;
        // Pre-commit registration failures (still roll back).
        std::wstring autorunTaskFailed;
        std::wstring ogServiceFailed;
        // Post-commit notes shown on the finish page (never roll back).
        std::wstring warnPlatform64Start;
        std::wstring warnPlatform32Start;
        std::wstring warnPlatformStartHint;
        std::wstring warnPlatform64Task;
        std::wstring warnPlatform32Task;
        std::wstring warnOpenGlassStart;
        std::wstring warnOpenGlassHint;
        std::wstring warnPriorHosts;
        std::wstring warnPriorHostsHint;
        std::wstring warnShellConflict;
        std::wstring warnShellHint;
        // Win11 classic fallback (install complete, compact menu unavailable):
        // title when the msix signer certificate is not trusted / other reason,
        // and the hint with "{dir}" replaced by the install folder.
        std::wstring warnShellClassicCert;
        std::wstring warnShellClassicOther;
        std::wstring warnShellClassicHint;
        // The user accepted the certificate page but the certificate could not
        // be added to LocalMachine\TrustedPeople (title + hint).
        std::wstring warnCertInstall;
        std::wstring warnCertInstallHint;
        std::wstring warnUserDefaults;
    };

    // A finishing step that failed after the commit point (first platform
    // start). The installation itself is complete and is never rolled back
    // because of these; the finish page lists them instead.
    struct FinishWarning {
        std::wstring title;   // one localized sentence
        std::wstring detail;  // optional hint and/or technical reason
    };

    InstallEngine() noexcept = default;
    ~InstallEngine() noexcept;

    InstallEngine(const InstallEngine&) = delete;
    InstallEngine& operator=(const InstallEngine&) = delete;

    bool Start(
        HINSTANCE instance,
        const std::wstring& x64Dir,
        const std::wstring& x86Dir,
        const std::wstring& openGlassDir,
        ComponentSelection selection,
        GlassProducts::OpenGlassStartupState openGlassStartup,
        bool forceClean,
        PhaseLabels labels,
        StatusFn onStatus,
        const std::wstring& secureUxRemovalCode = {}) noexcept;

    bool Poll() noexcept;
    bool Succeeded() const noexcept { return succeeded_; }
    std::vector<FinishWarning> FinishWarnings() const noexcept;
    bool WasCancelled() const noexcept { return userCancelled_.load(std::memory_order_acquire); }
    bool RollbackSucceeded() const noexcept { return rollbackSucceeded_.load(std::memory_order_acquire); }
    bool IsCancelRequested() const noexcept { return cancelRequested_.load(std::memory_order_acquire); }
    bool IsRollingBack() const noexcept { return rollingBack_.load(std::memory_order_acquire); }
    // Explicitly selected startup products only; retained-host restoration is finalization.
    bool HasHostLaunchWork() const noexcept { return hostLaunchExpected_.load(std::memory_order_acquire); }
    bool IsLaunchingPhase() const noexcept { return launchingPhase_.load(std::memory_order_acquire); }
    bool CanRequestCancel() const noexcept {
        return started_ && !finished_
            && !cancelRequested_.load(std::memory_order_acquire)
            && !rollingBack_.load(std::memory_order_acquire)
            && !failureFinalizing_.load(std::memory_order_acquire)
            && !launchingPhase_.load(std::memory_order_acquire);
    }

    // Hold the commit/launch boundary while the in-window confirmation is open.
    bool PauseForCancelDecision() noexcept;
    void ResumeAfterCancelDecision() noexcept;
    void RequestCancel() noexcept;

    std::wstring ErrorText() const noexcept;
    std::wstring FailureDetail() const noexcept;
    std::wstring MsiMessage() const noexcept;
    std::wstring RollbackNote() const noexcept;
    unsigned MsiExitCode() const noexcept;
    bool TakeErrorPageRequest() noexcept;
    void NotifyErrorPageShown() noexcept;
    void RememberMsiMessage(const wchar_t* text) noexcept;
    std::wstring StatusLine() const noexcept;
    int ProgressUnits() const noexcept { return progressUnits_; }
    bool IsProgressReverse() const noexcept { return progressReverse_; }
    bool IsMarquee() const noexcept;
    void SetStatus(const std::wstring& line, int percent) noexcept;

    enum class HoldKind { None, Modules, Processes, MissingHelper };
    enum class HoldAction { None, Retry, Unlock, ContinueRisk, Cancel };

    HoldKind CurrentHold() const noexcept;
    WizardUnlock::Phase ActiveUnlock() const noexcept { return unlockActivity_.Current(); }
    std::wstring HoldReport() const noexcept;
    bool QuietClose() const noexcept { return quietClose_; }
    void SignalHold(HoldAction action) noexcept;

private:
    struct PriorHostState {
        bool x64TaskExisted = false;
        bool x64TaskEnabled = true;
        bool x64WasRunning = false;
        bool x86TaskExisted = false;
        bool x86TaskEnabled = true;
        bool x86WasRunning = false;
        bool ogServiceExisted = false;
        bool ogLegacyTaskExisted = false;
        bool ogLegacyTaskEnabled = true;
        bool shortcutExisted = false;
        DWORD ogServiceStartType = SERVICE_AUTO_START;
        bool ogWasRunning = false;
    };

    static DWORD WINAPI WorkerThread(LPVOID context) noexcept;
    bool CapturePriorHostState() noexcept;
    // Synchronize failure handling with the user's open cancel confirmation.
    bool ChooseCancellationOrFailure(bool failed) noexcept;
    bool RestorePriorHosts(bool keptOnly = false) noexcept;
    bool StopGlassPlatform(GlassShared::GlassPlatformTask::PlatformArch arch,
        const std::wstring& statusLabel, const wchar_t* logTag,
        const std::function<bool(const wchar_t*)>& fail) noexcept;
    bool StopRunningHosts() noexcept;
    bool ReleaseLockedModules() noexcept;
    bool EnsureTempWorkspace() noexcept;
    bool ExtractPayload(HINSTANCE instance) noexcept;
    bool InstallGlassSettingsEarly() noexcept;
    bool RunOfficialOpenGlassOnly() noexcept;
    bool BuildExecutionPlan() noexcept;
    GlassProductPlan::Plan executionPlan_{};
    unsigned procedureMask_=0x2Fu;
    bool auxiliaryOnly_=false;
    bool executionPlanReady_=false;
    bool openGlassResidualRemoval_=false;
    bool RunMsiSequence() noexcept;
    bool InstallOriginalSecureUxTheme() noexcept;
    GlassSecureUxPackage::Package secureUxPackage_;
    GlassOpenGlassPackage::Package openGlassPackage_;
    std::shared_ptr<GlassOpenGlassFiles::Transaction> openGlassFiles_;
    GlassSecureUxPackage::Removal secureUxRemoval_;
    GlassProfileMaintenance::Inventory secureUxRemovalProfiles_;
    bool secureUxLoadedRemoval_=false;
    std::wstring secureUxRemovalCode_;
    bool PrepareSecureUxRemoval() noexcept;
    bool RemoveOriginalSecureUxTheme() noexcept;
    bool RecheckSnapshottedProduct(const wchar_t* productCode) noexcept;
    bool RecheckRelatedProducts(const wchar_t* productCode) noexcept;
    bool StopHostsForRollback() noexcept;
    bool RemoveCreatedHostsForRollback() noexcept;
    bool PrepareManagedRemoval() noexcept;
    bool PrepareFullRemovalCleanup() noexcept;
    void FinishFullRemovalCleanup() noexcept;
    GlassProfileMaintenance::Inventory fullRemovalProfiles_;
    bool RemoveManagedProduct(GlassProductPlan::Product product) noexcept;
    bool RunOneMsi(const std::wstring& msiPath, const std::wstring& installDir,
        const std::wstring& label, int progressBase, int progressSpan) noexcept;
    // Phase B: autostart tasks / OpenGlass service and their validation.
    // Runs before anything is started, so a failure still rolls back.
    bool RegisterSelectedProducts() noexcept;
    // Phase C: after worker commit (including the final vendor transaction),
    // start selected products, restore prior hosts, register shell extension.
    // Every failure here becomes a FinishWarning, never a rollback.
    void LaunchSelectedProducts() noexcept;
    void SeedUserDefaults() noexcept;
    void AddFinishWarning(const std::wstring& title, const std::wstring& detail) noexcept;
    bool DetermineCleanInstall() noexcept;
    bool CreateRollbackSnapshot() noexcept;
    bool PerformRollback() noexcept;
    bool UndoCleanInstall() noexcept;
    bool PurgeRemnants() noexcept;
    void SetFailure(const std::wstring& text, unsigned msiCode) noexcept;

    StatusFn status_;
    GlassInstallRollback::SessionState rollbackSession_;
    GlassInstallRemoval::Session removalSession_;
    bool shellRemovedForMaintenance_=false;
    ComponentSelection selection_{};
    GlassProducts::OpenGlassStartupState openGlassStartup_{};
    GlassUserDefaults::SeedReport userDefaults_;
    PriorHostState prior_{};
    PhaseLabels labels_;
    std::wstring tempDir_;
    std::wstring error_;
    std::wstring failureDetail_;
    std::wstring msiMessage_;
    std::wstring rollbackNote_;
    unsigned msiExitCode_ = 0;
    std::wstring x64Dir_;
    std::wstring x86Dir_;
    std::wstring openGlassDir_;
    std::wstring holdReport_;
    HoldKind holdKind_ = HoldKind::None;
    WizardUnlock::Activity unlockActivity_;
    std::atomic<HoldAction> holdAction_{HoldAction::None};
    HANDLE holdGate_ = nullptr;
    HANDLE decisionGate_ = nullptr;
    HANDLE errorPageShown_ = nullptr;
    volatile LONG errorPageRequested_ = 0;
    std::atomic<bool> cancelRequested_{false};
    std::atomic<bool> userCancelled_{false};
    std::atomic<bool> rollingBack_{false};
    std::atomic<bool> failureFinalizing_{false};
    std::atomic<bool> hostLaunchExpected_{false}; // UI only; launchingPhase_ still closes cancellation.
    std::atomic<bool> launchingPhase_{false};
    std::atomic_bool rollbackSucceeded_{true};
    bool preserveSnapshot_ = false;
    bool shellRegisteredBySetup_ = false;
    bool x64TaskCreatedBySetup_ = false, x86TaskCreatedBySetup_ = false;
    bool ogServiceCreatedBySetup_ = false;
    bool ogServiceStartTypeMayNeedRestore_ = false;
    std::atomic_bool quietClose_{false};
    bool cleanInstall_ = false;
    bool forceClean_ = false;
    bool purgeCompleted_ = false;
    bool hostsStopped_ = false;
    bool x64StopAttempted_ = false, x86StopAttempted_ = false, ogStopSignalled_ = false;
    bool installAttempted_ = false;
    unsigned earlySettingsAttemptMask_ = 0;
    // Before this boundary only the early GlassSettings MSI can have changed.
    bool nonSettingsChangesAttempted_ = false;
    bool scanNativeX64_ = false;
    bool started_ = false;
    std::atomic_bool finished_{false};
    std::atomic_bool succeeded_{false};
    HANDLE thread_ = nullptr;
    HINSTANCE instance_ = nullptr;
    mutable SRWLOCK statusLock_ = SRWLOCK_INIT;
    std::wstring statusLine_;
    std::vector<FinishWarning> finishWarnings_; // guarded by statusLock_
    std::atomic_int progressUnits_{0};
    std::atomic_bool progressReverse_{false};
    bool marquee_ = false;
};
