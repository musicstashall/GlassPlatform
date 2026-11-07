#pragma once
#include <Windows.h>
#include <atomic>
#include <string>
#include <array>
#include <vector>

namespace GlassSecureUxPackage {
struct Package {
    std::wstring path, productCode, version;
    bool alreadyInstalled=false;
};
// Only one original vendor MSI is supported by this commit-last removal path.
// This is not a Glass rollback product and never authorizes file/registry cleanup.
struct Removal {
    std::wstring productCode, version, baseline;
    std::array<BYTE,32> digest{};
    bool ready=false;
};
bool PrepareRemoval(const std::wstring& workspace, const std::wstring& expectedCode,
    const std::atomic<bool>& cancel, Removal& removal, std::wstring& error) noexcept;
bool RecheckRemoval(const std::wstring& workspace, const std::atomic<bool>& cancel,
    const Removal& removal, std::wstring& error) noexcept;
// Context-validated install presence. Stale ABSENT family references with no
// contexts are not an existing installation; foreign contexts still refuse it.
bool QueryInstallPresence(std::vector<std::wstring>& installed,
    const std::atomic<bool>& cancel, std::wstring& error) noexcept;
// Runs on InstallEngine's worker before any system changes. Checks next to
// GetModuleFileName(NULL)/packages and Current, never CWD. Downloads and the
// returned immutable snapshot stay in the protected ProgramData workspace.
bool Prepare(const std::wstring& privateDirectory, const std::atomic<bool>& cancel,
    Package& package, std::wstring& error) noexcept;
// Refuses legacy Glass-repacked products. Existing official versions are kept
// without implicit repair, upgrade, MSI download or registration changes.
bool CheckLegacyRepackAbsent(std::wstring& error) noexcept;
}
