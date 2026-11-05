#pragma once
#include <Windows.h>
#include <atomic>
#include <string>

namespace GlassSecureUxPackage {
struct Package {
    std::wstring path, productCode, version;
};
// Runs on InstallEngine's worker before any system changes. Checks next to
// GetModuleFileName(NULL), never CWD. The returned MSI is a verified private
// snapshot, not the user-writable adjacent download.
bool Prepare(const std::wstring& privateDirectory, const std::atomic<bool>& cancel,
    Package& package, std::wstring& error) noexcept;
// Refuses legacy Glass-repacked products and any installed official version.
// An existing official MSI must not be upgraded, repaired or removed implicitly.
bool CheckNoInstalledProduct(std::wstring& error) noexcept;
}
