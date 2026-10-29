#pragma once

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

struct SessionState {
    std::wstring workspaceDir;
    std::wstring x64Dir;
    std::wstring x86Dir;
    std::wstring openGlassDir;
    std::wstring backupX64;
    std::wstring backupX86;
    std::wstring backupOpenGlass;
    std::vector<ProductSnapshot> oldProducts;
    std::vector<std::wstring> installedThisSession;
    bool hadPriorInstall = false;
    unsigned copyError = 0;
};

using CopyProgressFn = std::function<void(int percentUnits)>;

bool CreateSnapshot(
    SessionState& state,
    const std::wstring& x64Dir,
    const std::wstring& x86Dir,
    const std::wstring& openGlassDir,
    bool includeOpenGlass,
    CopyProgressFn onProgress) noexcept;

void NoteInstalled(SessionState& state, const wchar_t* productCode) noexcept;

bool ExecuteRollback(
    SessionState& state,
    CopyProgressFn onProgress) noexcept;

} // namespace GlassInstallRollback
