#pragma once
#include <Windows.h>
#include <atomic>
#include <string>

namespace GlassSecureUxPackage {
struct Package {
    std::wstring path, productCode, version;
    bool alreadyInstalled=false;
};
// Runs on InstallEngine's worker before any system changes. Checks next to
// GetModuleFileName(NULL), never CWD. The returned MSI is a verified private
// snapshot, not the user-writable adjacent download.
bool Prepare(const std::wstring& privateDirectory, const std::atomic<bool>& cancel,
    Package& package, std::wstring& error) noexcept;
// Refuses legacy Glass-repacked products. Existing official versions are kept
// without implicit repair, upgrade, MSI download or registration changes.
bool CheckLegacyRepackAbsent(std::wstring& error) noexcept;
}
