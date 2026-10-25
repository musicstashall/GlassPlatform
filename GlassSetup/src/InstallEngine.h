#pragma once
#include <windows.h>
#include <functional>
#include <string>

// Распаковка встроенных MSI и установка через MsiInstallProduct с прогрессом по файлам.
class InstallEngine {
public:
    using StatusFn = std::function<void(const std::wstring& line, int percent)>;
    struct PhaseLabels {
        std::wstring extract;
        std::wstring platform64;
        std::wstring platform32;
        std::wstring openGlass;
        std::wstring finalize;
    };

    InstallEngine() noexcept = default;
    ~InstallEngine() noexcept;

    InstallEngine(const InstallEngine&) = delete;
    InstallEngine& operator=(const InstallEngine&) = delete;

    bool Start(HINSTANCE instance, const std::wstring& x64Dir, const std::wstring& x86Dir,
        const std::wstring& openGlassDir, bool installOpenGlass, PhaseLabels labels,
        StatusFn onStatus) noexcept;
    bool Poll() noexcept;
    bool Succeeded() const noexcept { return succeeded_; }
    std::wstring ErrorText() const noexcept { return error_; }
    std::wstring StatusLine() const noexcept;
    int ProgressUnits() const noexcept { return progressUnits_; }
    void SetStatus(const std::wstring& line, int percent) noexcept;

private:
    bool ExtractPayload(HINSTANCE instance) noexcept;
    bool RunMsiSequence(const std::wstring& x64Dir, const std::wstring& x86Dir,
        const std::wstring& openGlassDir, bool installOpenGlass) noexcept;
    bool RunOneMsi(const std::wstring& msiPath, const std::wstring& installDir,
        const std::wstring& label, int progressBase, int progressSpan) noexcept;
    bool SetOpenGlassHostOwner() noexcept;

    StatusFn status_;
    PhaseLabels labels_;
    std::wstring tempDir_;
    std::wstring error_;
    std::wstring ogHostPath_;
    bool installOpenGlass_ = false;
    bool started_ = false;
    bool finished_ = false;
    bool succeeded_ = false;
    HANDLE thread_ = nullptr;
    mutable SRWLOCK statusLock_ = SRWLOCK_INIT;
    std::wstring statusLine_;
    int progressUnits_ = 0;
};
