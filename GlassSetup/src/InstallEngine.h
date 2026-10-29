#pragma once
#include "InstallSupport/GlassInstallRollback.h"
#include <windows.h>
#include <functional>
#include <string>
#include <GlassPlatformTask.h>

// Распаковка встроенных MSI и установка через MsiInstallProduct с прогрессом по файлам.
class InstallEngine {
public:
    using StatusFn = std::function<void(const std::wstring& line, int percent)>;
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
        std::wstring rollbackFailed;
        std::wstring rollbackRestored;
        std::wstring rollbackRemoved;
        std::wstring nativeScanFailed;
        std::wstring snapshotFailed;
        std::wstring diskFull;
    };

    InstallEngine() noexcept = default;
    ~InstallEngine() noexcept;

    InstallEngine(const InstallEngine&) = delete;
    InstallEngine& operator=(const InstallEngine&) = delete;

    bool Start(HINSTANCE instance, const std::wstring& x64Dir, const std::wstring& x86Dir,
        const std::wstring& openGlassDir, bool installOpenGlass, bool forceClean,
        PhaseLabels labels, StatusFn onStatus) noexcept;
    bool Poll() noexcept;
    bool Succeeded() const noexcept { return succeeded_; }
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
    std::wstring HoldReport() const noexcept;
    bool QuietClose() const noexcept { return quietClose_; }
    void SignalHold(HoldAction action) noexcept;

private:
    bool StopGlassPlatform(GlassShared::GlassPlatformTask::PlatformArch arch,
        const std::wstring& statusLabel, const wchar_t* logTag,
        const std::function<bool(const wchar_t*)>& fail) noexcept;
    bool StopRunningHosts() noexcept;
    bool ReleaseLockedModules() noexcept;
    void DisableAutostart() noexcept;
    bool EnsureTempWorkspace() noexcept;
    bool ExtractPayload(HINSTANCE instance) noexcept;
    bool RunMsiSequence(const std::wstring& x64Dir, const std::wstring& x86Dir,
        const std::wstring& openGlassDir, bool installOpenGlass) noexcept;
    bool RunOneMsi(const std::wstring& msiPath, const std::wstring& installDir,
        const std::wstring& label, int progressBase, int progressSpan) noexcept;
    bool SetOpenGlassHostOwner() noexcept;
    bool RestorePlatformAutostart(bool installOpenGlass) noexcept;
    bool DetermineCleanInstall() noexcept;
    bool CreateRollbackSnapshot() noexcept;
    bool InstallGlassSettingsMsis() noexcept;
    bool PerformRollback() noexcept;
    bool UndoCleanInstall() noexcept;
    bool PurgeRemnants() noexcept;
    void SetFailure(const std::wstring& text, unsigned msiCode) noexcept;
    void EnsureGlassSettingsAfterPlatformUninstall() noexcept;

    StatusFn status_;
    GlassInstallRollback::SessionState rollbackSession_;
    PhaseLabels labels_;
    std::wstring tempDir_;
    std::wstring error_;
    std::wstring failureDetail_;
    std::wstring msiMessage_;
    std::wstring rollbackNote_;
    unsigned msiExitCode_ = 0;
    std::wstring ogHostPath_;
    std::wstring x64Dir_;
    std::wstring x86Dir_;
    std::wstring openGlassDir_;
    std::wstring holdReport_;
    HoldKind holdKind_ = HoldKind::None;
    HoldAction holdAction_ = HoldAction::None;
    HANDLE holdGate_ = nullptr;
    HANDLE errorPageShown_ = nullptr;
    volatile LONG errorPageRequested_ = 0;
    bool quietClose_ = false;
    bool cleanInstall_ = false;
    bool forceClean_ = false;
    bool purgeCompleted_ = false;
    bool installAttempted_ = false;
    bool scanNativeX64_ = false;
    bool installOpenGlass_ = false;
    bool started_ = false;
    bool finished_ = false;
    bool succeeded_ = false;
    HANDLE thread_ = nullptr;
    HINSTANCE instance_ = nullptr;
    mutable SRWLOCK statusLock_ = SRWLOCK_INIT;
    std::wstring statusLine_;
    int progressUnits_ = 0;
    bool progressReverse_ = false;
    bool marquee_ = false;
};
