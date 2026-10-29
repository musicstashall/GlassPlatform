#include "GlassInstallRollback.h"
#include "GlassInstallMsi.h"
#include "GlassInstallLog.h"
#include <GlassPlatformTask.h>
#include <msi.h>
#include <shlwapi.h>
#include <cstdint>
#include <vector>

namespace {

bool CopyFileLogged(const std::wstring& from, const std::wstring& to, unsigned& copyError) noexcept {
    if (CopyFileW(from.c_str(), to.c_str(), FALSE)) return true;
    const DWORD err = GetLastError();
    if (err == ERROR_DISK_FULL || copyError == 0) copyError = err;
    GlassInstallLog::LineFormat(L"CopyFile failed %s -> %s (%lu)", from.c_str(), to.c_str(), err);
    return false;
}

// Содержимое каталога, а не сам каталог: иначе в приёмнике появляется
// вложенная папка с именем источника. Маска SHFileOperation «dir\*» даёт 124
// (DE_INVALIDFILES), поэтому копирование пофайловое.
bool CopyTree(const std::wstring& from, const std::wstring& to, uint64_t& copiedBytes, uint64_t totalBytes,
    const GlassInstallRollback::CopyProgressFn& onProgress, unsigned& copyError) noexcept {
    if (from.empty() || copyError == ERROR_DISK_FULL) return copyError != ERROR_DISK_FULL;
    const DWORD attr = GetFileAttributesW(from.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return true;
    if ((attr & FILE_ATTRIBUTE_DIRECTORY) == 0)
        return CopyFileLogged(from, to, copyError);

    if (!CreateDirectoryW(to.c_str(), nullptr)) {
        const DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) {
            if (err == ERROR_DISK_FULL || copyError == 0) copyError = err;
            GlassInstallLog::LineFormat(L"CreateDirectory failed %s (%lu)", to.c_str(), err);
            return false;
        }
    }

    std::wstring prefix = from;
    if (prefix.back() != L'\\') prefix += L'\\';
    WIN32_FIND_DATAW fd{};
    const HANDLE find = FindFirstFileW((prefix + L'*').c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return true;
    bool ok = true;
    do {
        if (fd.cFileName[0] == L'.' && (!fd.cFileName[1] || (fd.cFileName[1] == L'.' && !fd.cFileName[2])))
            continue;
        const std::wstring src = prefix + fd.cFileName;
        std::wstring dst = to;
        if (dst.empty() || dst.back() != L'\\') dst += L'\\';
        dst += fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!CopyTree(src, dst, copiedBytes, totalBytes, onProgress, copyError)) ok = false;
        } else if (!CopyFileLogged(src, dst, copyError)) {
            ok = false;
        } else {
            copiedBytes += (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            if (onProgress && totalBytes > 0) {
                const int units = static_cast<int>((copiedBytes * 10000) / totalBytes);
                onProgress(units > 10000 ? 10000 : units);
            }
        }
        if (copyError == ERROR_DISK_FULL) break;
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return ok;
}

uint64_t DirBytes(const std::wstring& root) noexcept {
    uint64_t total = 0;
    if (root.empty()) return 0;
    std::wstring pattern = root;
    if (pattern.back() != L'\\') pattern += L'\\';
    pattern += L'*';
    WIN32_FIND_DATAW fd{};
    const HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.cFileName[0] == L'.' && (!fd.cFileName[1] || (fd.cFileName[1] == L'.' && !fd.cFileName[2])))
            continue;
        const std::wstring child = root + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            total += DirBytes(child);
        else
            total += (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return total;
}

void TrySnapshotProduct(GlassInstallRollback::SessionState& state,
    const wchar_t* productCode,
    const std::wstring& installDir) noexcept {
    if (!productCode || installDir.empty()) return;
    const INSTALLSTATE st = MsiQueryProductStateW(productCode);
    if (st != INSTALLSTATE_DEFAULT) return;
    GlassInstallRollback::ProductSnapshot snap;
    snap.productCode = productCode;
    snap.installDir = installDir;
    snap.displayVersion = GlassInstallMsi::QueryDisplayVersion(productCode);
    wchar_t temp[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, temp)) {
        wchar_t dest[MAX_PATH]{};
        const std::wstring name = productCode;
        swprintf_s(dest, L"%sGlassRb-%u-%s.msi", temp, GetTickCount(), name.c_str() + 1);
        if (GlassInstallMsi::CopyLocalPackage(productCode, dest))
            snap.cachedMsiPath = dest;
    }
    state.oldProducts.push_back(std::move(snap));
    state.hadPriorInstall = true;
}

} // namespace

namespace GlassInstallRollback {

bool CreateSnapshot(
    SessionState& state,
    const std::wstring& x64Dir,
    const std::wstring& x86Dir,
    const std::wstring& openGlassDir,
    bool includeOpenGlass,
    CopyProgressFn onProgress) noexcept {
    state.oldProducts.clear();
    state.installedThisSession.clear();
    state.hadPriorInstall = false;
    state.x64Dir = x64Dir;
    state.x86Dir = x86Dir;
    state.openGlassDir = openGlassDir;
    state.copyError = 0;
    wchar_t temp[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temp)) return false;
    wchar_t folder[MAX_PATH]{};
    swprintf_s(folder, L"%sGlassSetup-rollback-%u", temp, GetTickCount());
    if (!CreateDirectoryW(folder, nullptr)) {
        state.copyError = GetLastError();
        GlassInstallLog::LineFormat(L"Rollback workspace failed %s (%lu)", folder, state.copyError);
        return false;
    }
    state.workspaceDir = folder;

    TrySnapshotProduct(state, L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5074}", x64Dir);
    TrySnapshotProduct(state, L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5075}", x86Dir);
    TrySnapshotProduct(state, L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5071}", x64Dir);
    TrySnapshotProduct(state, L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5072}", x86Dir);
    if (includeOpenGlass)
        TrySnapshotProduct(state, L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5073}", openGlassDir);

    uint64_t total = DirBytes(x64Dir) + DirBytes(x86Dir) + (includeOpenGlass ? DirBytes(openGlassDir) : 0);
    uint64_t copied = 0;
    bool copiedOk = true;
    const auto bump = [&](const std::wstring& src, const std::wstring& sub) {
        if (src.empty()) return;
        wchar_t dest[MAX_PATH]{};
        if (!PathCombineW(dest, folder, sub.c_str())) {
            copiedOk = false;
            return;
        }
        CreateDirectoryW(dest, nullptr);
        if (!CopyTree(src, dest, copied, total, onProgress, state.copyError)) copiedOk = false;
    };
    bump(x64Dir, L"x64");
    state.backupX64 = std::wstring(folder) + L"\\x64";
    bump(x86Dir, L"x86");
    state.backupX86 = std::wstring(folder) + L"\\x86";
    if (includeOpenGlass) {
        bump(openGlassDir, L"OpenGlass");
        state.backupOpenGlass = std::wstring(folder) + L"\\OpenGlass";
    }
    GlassInstallLog::LineFormat(L"Rollback snapshot at %s", folder);
    return copiedOk;
}

void NoteInstalled(SessionState& state, const wchar_t* productCode) noexcept {
    if (!productCode || !productCode[0]) return;
    for (const auto& code : state.installedThisSession) {
        if (_wcsicmp(code.c_str(), productCode) == 0) return;
    }
    state.installedThisSession.push_back(productCode);
}

bool ExecuteRollback(SessionState& state, CopyProgressFn onProgress) noexcept {
    for (auto it = state.installedThisSession.rbegin(); it != state.installedThisSession.rend(); ++it)
        GlassInstallMsi::UninstallProduct(it->c_str());

    if (!state.hadPriorInstall) return true;

    uint64_t copied = 0;
    uint64_t total = 1;
    bool ok = true;
    unsigned copyError = 0;
    if (!state.backupX64.empty() && !state.x64Dir.empty()) {
        if (!CopyTree(state.backupX64, state.x64Dir, copied, total, onProgress, copyError)) ok = false;
    }
    if (copyError != ERROR_DISK_FULL && !state.backupX86.empty() && !state.x86Dir.empty()) {
        if (!CopyTree(state.backupX86, state.x86Dir, copied, total, onProgress, copyError)) ok = false;
    }
    if (copyError != ERROR_DISK_FULL && !state.backupOpenGlass.empty() && !state.openGlassDir.empty()) {
        if (!CopyTree(state.backupOpenGlass, state.openGlassDir, copied, total, onProgress, copyError)) ok = false;
    }
    if (copyError != 0) state.copyError = copyError;
    for (const auto& snap : state.oldProducts) {
        if (snap.cachedMsiPath.empty() || snap.installDir.empty()) continue;
        wchar_t patched[MAX_PATH]{};
        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp);
        GetTempFileNameW(temp, L"grb", 0, patched);
        DeleteFileW(patched);
        wcscat_s(patched, L".msi");
        if (!GlassInstallMsi::PrepareRestoreMsiCopy(snap.cachedMsiPath, patched)) {
            ok = false;
            continue;
        }
        const std::wstring props = GlassInstallMsi::BuildInstallPropertyString(snap.installDir, true);
        const UINT code = GlassInstallMsi::InstallProduct(patched, props, nullptr);
        DeleteFileW(patched);
        if (!GlassInstallMsi::MsiSuccess(code)) {
            GlassInstallLog::LineFormat(L"Rollback reinstall failed %s exit %u", snap.productCode.c_str(), code);
            ok = false;
        }
        const std::wstring now = GlassInstallMsi::QueryDisplayVersion(snap.productCode.c_str());
        if (!snap.displayVersion.empty() && _wcsicmp(now.c_str(), snap.displayVersion.c_str()) != 0)
            GlassInstallLog::Line(L"Rollback: DisplayVersion mismatch after restore");
    }
    return ok;
}

} // namespace GlassInstallRollback
