#pragma once
#include "InstallSupport/GlassInstallRollback.h"
#include "InstallSupport/GlassInstallRemoval.h"
#include "GlassProducts.h"
#include "WizardUnlockState.h"
#include <windows.h>
#include <winsvc.h>
#include <atomic>
#include <functional>
#include <string>
#include <GlassPlatformTask.h>

// Распаковка встроенных MSI и установка через MsiInstallProduct с кооперативной отменой и откатом.
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
        std::wstring removing;
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
        StatusFn onStatus) noexcept;

    bool Poll() noexcept;
    bool Succeeded() const noexcept { return succeeded_; }
    bool WasCancelled() const noexcept { return userCancelled_.load(std::memory_order_acquire); }
    bool RollbackSucceeded() const noexcept { return rollbackSucceeded_.load(std::memory_order_acquire); }
    bool IsCancelRequested() const noexcept { return cancelRequested_.load(std::memory_order_acquire); }
    bool IsRollingBack() const noexcept { return rollingBack_.load(std::memory_order_acquire); }
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

    enum class HoldKind { None, Modules, Processes };
    enum class HoldAction { None, Retry, Unlock, Cancel };

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
    bool RestorePriorHosts(bool selectedOnly = false) noexcept;
    bool PrepareProductRemoval() noexcept;
    bool StopGlassPlatform(GlassShared::GlassPlatformTask::PlatformArch arch,
        const std::wstring& statusLabel, const wchar_t* logTag,
        const std::function<bool(const wchar_t*)>& fail) noexcept;
    bool StopRunningHosts() noexcept;
    bool ReleaseLockedModules() noexcept;
    bool EnsureTempWorkspace() noexcept;
    bool ExtractPayload(HINSTANCE instance) noexcept;
    bool InstallGlassSettingsEarly() noexcept;
    bool RunMsiSequence() noexcept;
    bool RunOneMsi(const std::wstring& msiPath, const std::wstring& installDir,
        const std::wstring& label, int progressBase, int progressSpan) noexcept;
    bool LaunchAndVerifySelectedProducts() noexcept;
    bool DetermineCleanInstall() noexcept;
    bool CreateRollbackSnapshot() noexcept;
    bool PerformRollback() noexcept;
    bool UndoCleanInstall() noexcept;
    bool PurgeRemnants() noexcept;
    void SetFailure(const std::wstring& text, unsigned msiCode) noexcept;

    StatusFn status_;
    GlassInstallRollback::SessionState rollbackSession_;
    GlassInstallRemoval::Session removalSession_;
    ComponentSelection selection_{};
    GlassProducts::OpenGlassStartupState openGlassStartup_{};
    ComponentSelection remove_ {false, false, false, false};
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
    bool x64StopSignalled_ = false, x86StopSignalled_ = false, ogStopSignalled_ = false;
    bool installAttempted_ = false;
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
    std::atomic_int progressUnits_{0};
    std::atomic_bool progressReverse_{false};
    bool marquee_ = false;
};
