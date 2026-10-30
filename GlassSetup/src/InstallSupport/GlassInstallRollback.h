#pragma once

#include <windows.h>
#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace GlassInstallRollback {

struct ProductSnapshot {
    std::wstring productCode;
    std::wstring installDir;
    std::wstring cachedMsiPath;
    std::wstring displayVersion;
};

struct RegistryValueSnapshot {
    HKEY hive = nullptr;
    REGSAM view = 0;
    std::wstring key;
    std::wstring name;
    DWORD type = 0;
    std::vector<BYTE> data;
    bool existed = false;
};

struct SessionState {
    std::wstring workspaceDir;
    std::wstring x64Dir;
    std::wstring x86Dir;
    std::wstring openGlassDir;
    std::wstring backupX64;
    std::wstring backupX86;
    std::wstring backupOpenGlass;
    std::vector<ProductSnapshot> oldProducts;
    std::vector<RegistryValueSnapshot> registryValues;
    std::vector<std::wstring> installedThisSession;
    std::vector<std::wstring> modifiedProducts;
    bool hadPriorInstall = false;
    unsigned copyError = 0;
    // Failure to verify an installed MSI's file locations is distinct from a
    // copy error: proceeding could uninstall files outside the fixed root.
    bool productPathUnverified = false;
    std::wstring productPathCode;
    std::wstring productPathDetail;
};

using CopyProgressFn = std::function<void(int percentUnits)>;

bool CreateSnapshot(
    SessionState& state,
    const std::wstring& x64Dir,
    const std::wstring& x86Dir,
    const std::wstring& openGlassDir,
    bool includeOpenGlass,
    CopyProgressFn onProgress,
    const std::atomic_bool* cancelled = nullptr) noexcept;

void NoteModified(SessionState& state, const wchar_t* productCode) noexcept;
void NoteInstalled(SessionState& state, const wchar_t* productCode) noexcept;
// During rollback, recheck the current registered MSI before uninstalling it;
// a concurrently changed/foreign registration is never removed speculatively.
bool UninstallSessionProductSafely(SessionState& state, const wchar_t* productCode) noexcept;

enum class RestoreScope { AllProducts, SettingsX86, SettingsX64 };

bool ExecuteRollback(
    SessionState& state,
    CopyProgressFn onProgress,
    RestoreScope scope = RestoreScope::AllProducts) noexcept;

bool RestoreRegistryValues(const SessionState& state) noexcept;
void CleanupSnapshot(SessionState& state) noexcept;

} // namespace GlassInstallRollback
