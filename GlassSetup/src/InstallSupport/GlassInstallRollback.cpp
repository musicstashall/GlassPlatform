#include "GlassInstallRollback.h"
#include "GlassInstallMsi.h"
#include "GlassInstallUtil.h"
#include "GlassInstallLog.h"
#include <GlassPlatformTask.h>
#include <GlassShellMenuConstants.h>
#include <msi.h>
#include <shlwapi.h>
#include <cstdint>
#include <vector>

namespace {

bool IsNestedSubPath(const std::wstring& parent, const std::wstring& child) noexcept {
    const std::wstring p = GlassInstallUtil::TrimTrailingSlashes(parent);
    const std::wstring c = GlassInstallUtil::TrimTrailingSlashes(child);
    if (p.empty() || c.empty() || c.size() <= p.size())
        return false;
    if (_wcsnicmp(p.c_str(), c.c_str(), p.size()) != 0)
        return false;
    return c[p.size()] == L'\\' || c[p.size()] == L'/';
}

bool SamePathInsensitive(const std::wstring& a, const std::wstring& b) noexcept {
    const std::wstring na = GlassInstallUtil::TrimTrailingSlashes(a);
    const std::wstring nb = GlassInstallUtil::TrimTrailingSlashes(b);
    return !na.empty() && _wcsicmp(na.c_str(), nb.c_str()) == 0;
}

DWORD CALLBACK CopyProgress(LARGE_INTEGER, LARGE_INTEGER, LARGE_INTEGER, LARGE_INTEGER,
    DWORD, DWORD, HANDLE, HANDLE, LPVOID context) {
    const auto* cancelled=static_cast<const std::atomic_bool*>(context);
    return cancelled && cancelled->load(std::memory_order_acquire) ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
}

bool CopyFileLogged(const std::wstring& from, const std::wstring& to, unsigned& copyError,
    const std::atomic_bool* cancelled) noexcept {
    if (cancelled && cancelled->load(std::memory_order_acquire)) { copyError=ERROR_CANCELLED; return false; }
    if (CopyFileExW(from.c_str(), to.c_str(), CopyProgress, const_cast<std::atomic_bool*>(cancelled), nullptr, 0)) return true;
    const DWORD err = GetLastError();
    if (err == ERROR_DISK_FULL || copyError == 0) copyError = err;
    GlassInstallLog::LineFormat(L"CopyFile failed %s -> %s (%lu)", from.c_str(), to.c_str(), err);
    return false;
}

bool CopyTree(
    const std::wstring& from,
    const std::wstring& to,
    const std::wstring& skipSubdir,
    uint64_t& copiedBytes,
    uint64_t totalBytes,
    const GlassInstallRollback::CopyProgressFn& onProgress,
    unsigned& copyError,
    const std::atomic_bool* cancelled = nullptr) noexcept {
    if (cancelled && cancelled->load(std::memory_order_acquire)) { copyError=ERROR_CANCELLED; return false; }
    if (from.empty() || copyError == ERROR_DISK_FULL || copyError == ERROR_CANCELLED) return copyError == 0;
    const DWORD attr = GetFileAttributesW(from.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        copyError=GetLastError();
        if (!copyError) copyError=ERROR_GEN_FAILURE;
        GlassInstallLog::LineFormat(L"Snapshot source unavailable: %s (%lu)",from.c_str(),copyError);
        return false;
    }
    if (attr & FILE_ATTRIBUTE_REPARSE_POINT) { copyError=ERROR_NOT_SUPPORTED; return false; }
    if ((attr & FILE_ATTRIBUTE_DIRECTORY) == 0)
        return CopyFileLogged(from, to, copyError, cancelled);

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
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD error=GetLastError();
        if (error==ERROR_FILE_NOT_FOUND) return true; // Empty directory.
        copyError=error?error:ERROR_GEN_FAILURE;
        GlassInstallLog::LineFormat(L"Snapshot enumeration failed: %s (%lu)",from.c_str(),copyError);
        return false;
    }
    bool ok = true;
    do {
        if (fd.cFileName[0] == L'.' && (!fd.cFileName[1] || (fd.cFileName[1] == L'.' && !fd.cFileName[2])))
            continue;
        const std::wstring src = prefix + fd.cFileName;
        if (!skipSubdir.empty() && SamePathInsensitive(src, skipSubdir))
            continue;
        std::wstring dst = to;
        if (dst.empty() || dst.back() != L'\\') dst += L'\\';
        dst += fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            copyError=ERROR_NOT_SUPPORTED;
            GlassInstallLog::LineFormat(L"Snapshot refused reparse point: %s",src.c_str());
            ok=false; break;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!CopyTree(src, dst, skipSubdir, copiedBytes, totalBytes, onProgress, copyError, cancelled)) ok = false;
        } else if (!CopyFileLogged(src, dst, copyError, cancelled)) {
            ok = false;
        } else {
            copiedBytes += (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            if (onProgress && totalBytes > 0) {
                const int units = static_cast<int>((copiedBytes * 10000) / totalBytes);
                onProgress(units > 10000 ? 10000 : units);
            }
        }
        if (cancelled && cancelled->load(std::memory_order_acquire)) {
            copyError=ERROR_CANCELLED;
            ok=false;
            break;
        }
        if (copyError != 0) break;
    } while (FindNextFileW(find, &fd));
    if (ok && copyError==0) {
        const DWORD error=GetLastError();
        if (error!=ERROR_NO_MORE_FILES) {
            copyError=error?error:ERROR_GEN_FAILURE;
            GlassInstallLog::LineFormat(L"Snapshot enumeration interrupted: %s (%lu)",from.c_str(),copyError);
            ok=false;
        }
    }
    FindClose(find);
    return ok;
}

uint64_t DirBytes(const std::wstring& root, const std::wstring& skipSubdir = {},
    const std::atomic_bool* cancelled = nullptr) noexcept {
    if (cancelled && cancelled->load(std::memory_order_acquire)) return 0;
    uint64_t total = 0;
    if (root.empty()) return 0;
    std::wstring pattern = root;
    if (pattern.back() != L'\\') pattern += L'\\';
    pattern += L'*';
    WIN32_FIND_DATAW fd{};
    const HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return 0;
    do {
        if (cancelled && cancelled->load(std::memory_order_acquire)) break;
        if (fd.cFileName[0] == L'.' && (!fd.cFileName[1] || (fd.cFileName[1] == L'.' && !fd.cFileName[2])))
            continue;
        const std::wstring child = GlassInstallUtil::TrimTrailingSlashes(root) + L"\\" + fd.cFileName;
        if (!skipSubdir.empty() && SamePathInsensitive(child, skipSubdir))
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            total += DirBytes(child, skipSubdir, cancelled);
        else
            total += (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return total;
}

void DeleteDirectoryRecursive(const std::wstring& root) noexcept {
    if (root.empty()) return;
    const DWORD attr = GetFileAttributesW(root.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return;
    if ((attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        SetFileAttributesW(root.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(root.c_str());
        return;
    }
    std::wstring prefix = root;
    if (prefix.back() != L'\\') prefix += L'\\';
    WIN32_FIND_DATAW fd{};
    const HANDLE find = FindFirstFileW((prefix + L'*').c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == L'.' && (!fd.cFileName[1] || (fd.cFileName[1] == L'.' && !fd.cFileName[2])))
                continue;
            const std::wstring child = prefix + fd.cFileName;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                DeleteDirectoryRecursive(child);
            } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                RemoveDirectoryW(child.c_str());
            } else {
                SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(child.c_str());
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    SetFileAttributesW(root.c_str(), FILE_ATTRIBUTE_NORMAL);
    RemoveDirectoryW(root.c_str());
}

bool CaptureRegistryValue(GlassInstallRollback::SessionState& state, HKEY hive, REGSAM view,
    const wchar_t* path, const wchar_t* name) noexcept {
    GlassInstallRollback::RegistryValueSnapshot item;
    item.hive=hive; item.view=view; item.key=path; item.name=name;
    HKEY key=nullptr;
    const LSTATUS open=RegOpenKeyExW(hive,path,0,KEY_QUERY_VALUE|view,&key);
    if (open==ERROR_FILE_NOT_FOUND || open==ERROR_PATH_NOT_FOUND) {
        state.registryValues.push_back(std::move(item)); return true;
    }
    if (open!=ERROR_SUCCESS) { state.copyError=open; return false; }
    DWORD size=0;
    LSTATUS status=RegQueryValueExW(key,name,nullptr,&item.type,nullptr,&size);
    if (status==ERROR_SUCCESS) {
        if (size>1024*1024) { RegCloseKey(key); state.copyError=ERROR_MORE_DATA; return false; }
        item.data.resize(size);
        status=RegQueryValueExW(key,name,nullptr,&item.type,item.data.empty()?nullptr:item.data.data(),&size);
        // A concurrently changed value can report a much larger required size.
        if (status==ERROR_SUCCESS) {
            item.data.resize(size);
            item.existed=true;
        }
    }
    RegCloseKey(key);
    if (status!=ERROR_SUCCESS && status!=ERROR_FILE_NOT_FOUND) { state.copyError=status; return false; }
    state.registryValues.push_back(std::move(item)); return true;
}

bool CaptureRegistryValues(GlassInstallRollback::SessionState& state) noexcept {
    state.registryValues.clear();
    const wchar_t* platform=L"Software\\GlassPlatform";
    for (const wchar_t* value:{L"InstallPath",L"InstallPathX64",L"InstallPathX86"}) {
        if (!CaptureRegistryValue(state,HKEY_CURRENT_USER,0,platform,value)) return false;
        if (!CaptureRegistryValue(state,HKEY_LOCAL_MACHINE,KEY_WOW64_32KEY,platform,value)) return false;
        if (GlassInstallUtil::IsOs64Bit() && !CaptureRegistryValue(state,HKEY_LOCAL_MACHINE,KEY_WOW64_64KEY,platform,value)) return false;
    }
    if (!CaptureRegistryValue(state,HKEY_CURRENT_USER,0,L"Software\\Wow6432Node\\GlassPlatform",L"InstallPath")) return false;
    for (const wchar_t* value:{L"InstallPath",L"OpenGlassInstallPath"}) {
        if (!CaptureRegistryValue(state,HKEY_CURRENT_USER,0,L"Software\\StanislavKa\\GlassPlatform",value)) return false;
        if (GlassInstallUtil::IsOs64Bit() && !CaptureRegistryValue(state,HKEY_LOCAL_MACHINE,KEY_WOW64_64KEY,L"Software\\StanislavKa\\GlassPlatform",value)) return false;
    }
    if (!CaptureRegistryValue(state,HKEY_CURRENT_USER,0,L"Software\\StanislavKa\\OpenGlass",L"InstallPath")) return false;
    // Sparse-shell registration temporarily changes these Explorer block-list values.
    constexpr wchar_t blocked[]=L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Blocked";
    for (const wchar_t* clsid:{GlassShared::GlassShellMenu::kToggleExplorerClsidBraced,
                               GlassShared::GlassShellMenu::kOpenConfigExplorerClsidBraced})
        if (!CaptureRegistryValue(state,HKEY_CURRENT_USER,0,blocked,clsid)) return false;
    return true;
}

bool TrySnapshotProduct(GlassInstallRollback::SessionState& state,
    const wchar_t* productCode,
    const std::wstring& installDir,
    const std::atomic_bool* cancelled) noexcept {
    if (cancelled && cancelled->load(std::memory_order_acquire)) { state.copyError=ERROR_CANCELLED; return false; }
    if (!productCode) return true;
    const INSTALLSTATE st = MsiQueryProductStateW(productCode);
    if (st != INSTALLSTATE_DEFAULT) return true;
    if (installDir.empty()) { state.copyError=ERROR_PATH_NOT_FOUND; return false; }
    GlassInstallRollback::ProductSnapshot snap;
    snap.productCode = productCode;
    snap.installDir = installDir;
    snap.displayVersion = GlassInstallMsi::QueryDisplayVersion(productCode);
    if (!state.workspaceDir.empty()) {
        const std::wstring name = productCode;
        const std::wstring dest = state.workspaceDir + L"\\cached-" + name.substr(1, name.size() > 2 ? name.size() - 2 : name.size()) + L".msi";
        if (GlassInstallMsi::CopyLocalPackage(productCode, dest, cancelled))
            snap.cachedMsiPath = dest;
    }
    if (snap.cachedMsiPath.empty()) {
        state.copyError=GetLastError();
        if (!state.copyError) state.copyError=ERROR_FILE_NOT_FOUND;
        GlassInstallLog::LineFormat(L"Snapshot failed: cached MSI unavailable for %s",productCode);
        return false;
    }
    state.oldProducts.push_back(std::move(snap));
    state.hadPriorInstall = true;
    return true;
}

} // namespace

namespace GlassInstallRollback {

bool CreateSnapshot(
    SessionState& state,
    const std::wstring& x64Dir,
    const std::wstring& x86Dir,
    const std::wstring& openGlassDir,
    bool includeOpenGlass,
    CopyProgressFn onProgress,
    const std::atomic_bool* cancelled) noexcept {
    CleanupSnapshot(state);
    state.oldProducts.clear();
    state.installedThisSession.clear();
    state.modifiedProducts.clear();
    state.hadPriorInstall = false;
    state.x64Dir = x64Dir;
    state.x86Dir = x86Dir;
    state.openGlassDir = openGlassDir;
    state.backupX64.clear();
    state.backupX86.clear();
    state.backupOpenGlass.clear();
    state.copyError = 0;
    if (!CaptureRegistryValues(state)) return false;

    if (!GlassInstallUtil::CreateSecureTempDirectory(L"GlassRollback",state.workspaceDir)) {
        state.copyError=GetLastError();
        if (!state.copyError) state.copyError=ERROR_ACCESS_DENIED;
        GlassInstallLog::LineFormat(L"Rollback workspace creation failed (%lu)",state.copyError);
        return false;
    }
    const std::wstring folder=state.workspaceDir;

    if (!TrySnapshotProduct(state, GlassInstallMsi::kProductSettingsX64, x64Dir, cancelled) ||
        !TrySnapshotProduct(state, GlassInstallMsi::kProductSettingsX86, x86Dir, cancelled) ||
        !TrySnapshotProduct(state, GlassInstallMsi::kProductPlatformX64, x64Dir, cancelled) ||
        !TrySnapshotProduct(state, GlassInstallMsi::kProductPlatformX86, x86Dir, cancelled) ||
        !TrySnapshotProduct(state, GlassInstallMsi::kProductOpenGlass, openGlassDir, cancelled) ||
        !TrySnapshotProduct(state, L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5061}", x64Dir, cancelled) ||
        !TrySnapshotProduct(state, L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5062}", x86Dir, cancelled) ||
        !TrySnapshotProduct(state, L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5063}", openGlassDir, cancelled))
        return false;
    if (cancelled && cancelled->load(std::memory_order_acquire)) { state.copyError=ERROR_CANCELLED; return false; }

    const bool skipNestedOg = IsNestedSubPath(x64Dir, openGlassDir);
    const std::wstring skipInX64 = skipNestedOg ? openGlassDir : std::wstring{};
    // The OpenGlass subdirectory can contain user data even when no MSI is
    // registered. Access errors are not evidence that it is absent.
    bool ogDirectoryExists = false;
    if (!openGlassDir.empty()) {
        const DWORD attributes = GetFileAttributesW(openGlassDir.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES) ogDirectoryExists = true;
        else {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
                state.copyError = error ? error : ERROR_GEN_FAILURE;
                GlassInstallLog::LineFormat(L"Snapshot OpenGlass path unavailable: %s (%lu)",openGlassDir.c_str(),state.copyError);
                return false;
            }
        }
    }
    const bool backupOg = includeOpenGlass || ogDirectoryExists;

    uint64_t total = DirBytes(x64Dir, skipInX64, cancelled) + DirBytes(x86Dir, {}, cancelled)
        + (backupOg ? DirBytes(openGlassDir, {}, cancelled) : 0);
    if (cancelled && cancelled->load(std::memory_order_acquire)) { state.copyError=ERROR_CANCELLED; return false; }
    uint64_t copied = 0;
    bool copiedOk = true;
    const auto bump = [&](const std::wstring& src, const std::wstring& sub, const std::wstring& skipSub, std::wstring& outBackup) {
        if (src.empty()) return;
        const DWORD attr = GetFileAttributesW(src.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) {
            const DWORD error=GetLastError();
            if (error!=ERROR_FILE_NOT_FOUND && error!=ERROR_PATH_NOT_FOUND) {
                state.copyError=error?error:ERROR_GEN_FAILURE;
                GlassInstallLog::LineFormat(L"Snapshot path unavailable: %s (%lu)",src.c_str(),state.copyError);
                copiedOk=false;
            }
            return;
        }
        wchar_t dest[MAX_PATH]{};
        if (!PathCombineW(dest, folder.c_str(), sub.c_str())) {
            copiedOk = false;
            return;
        }
        CreateDirectoryW(dest, nullptr);
        outBackup = dest;
        state.hadPriorInstall = true; // Existing files must survive even if MSI registration is broken.
        if (!CopyTree(src, dest, skipSub, copied, total, onProgress, state.copyError, cancelled))
            copiedOk = false;
    };
    bump(x64Dir, L"x64", skipInX64, state.backupX64);
    bump(x86Dir, L"x86", {}, state.backupX86);
    if (backupOg)
        bump(openGlassDir, L"OpenGlass", {}, state.backupOpenGlass);

    GlassInstallLog::LineFormat(L"Rollback snapshot at %s (hadPrior=%d, bytes=%llu)",
        folder.c_str(), state.hadPriorInstall ? 1 : 0, static_cast<unsigned long long>(total));
    return copiedOk;
}

void NoteModified(SessionState& state, const wchar_t* productCode) noexcept {
    if (!productCode || !productCode[0]) return;
    for (const auto& code:state.modifiedProducts)
        if (_wcsicmp(code.c_str(),productCode)==0) return;
    state.modifiedProducts.emplace_back(productCode);
}

void NoteInstalled(SessionState& state, const wchar_t* productCode) noexcept {
    if (!productCode || !productCode[0]) return;
    for (const auto& code : state.installedThisSession) {
        if (_wcsicmp(code.c_str(), productCode) == 0) return;
    }
    state.installedThisSession.push_back(productCode);
}

bool ExecuteRollback(SessionState& state, CopyProgressFn onProgress, RestoreScope scope) noexcept {
    bool uninstallOk=true;
    for (auto it = state.installedThisSession.rbegin(); it != state.installedThisSession.rend(); ++it)
        if (!GlassInstallMsi::UninstallProduct(it->c_str())) uninstallOk=false;

    if (!state.hadPriorInstall
        && state.backupX64.empty() && state.backupX86.empty() && state.backupOpenGlass.empty()) {
        if (onProgress) onProgress(10000);
        return uninstallOk;
    }

    uint64_t copied = 0;
    bool ok = uninstallOk;
    unsigned copyError = 0;
    if (scope != RestoreScope::AllProducts) {
        // Cancellation during the module-monitor pages occurs before any
        // platform MSI is touched. Those old DLLs can still be loaded by
        // Explorer: do not overwrite the entire platform snapshot. Restore
        // only the Settings directory affected by the early MSI. The chosen
        // architecture is captured before any changes; do not re-detect it
        // during rollback, where a query error could select the wrong folder.
        const bool is64 = scope == RestoreScope::SettingsX64;
        const std::wstring& backup = is64 ? state.backupX64 : state.backupX86;
        const std::wstring& target = is64 ? state.x64Dir : state.x86Dir;
        if (!backup.empty() && !target.empty()) {
            const std::wstring from = GlassInstallUtil::TrimTrailingSlashes(backup) + L"\\GlassSettings";
            const std::wstring to = GlassInstallUtil::TrimTrailingSlashes(target) + L"\\GlassSettings";
            const DWORD attr = GetFileAttributesW(from.c_str());
            if (attr == INVALID_FILE_ATTRIBUTES) {
                const DWORD err = GetLastError();
                if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND) {
                    copyError = err ? err : ERROR_GEN_FAILURE;
                    ok = false;
                }
            } else if ((attr & FILE_ATTRIBUTE_DIRECTORY) == 0 || (attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
                copyError = ERROR_NOT_SUPPORTED;
                ok = false;
            } else {
                uint64_t total = DirBytes(from);
                if (!total) total = 1;
                if (!CopyTree(from, to, {}, copied, total, onProgress, copyError)) ok = false;
            }
        }
    } else {
        uint64_t total = DirBytes(state.backupX64) + DirBytes(state.backupX86) + DirBytes(state.backupOpenGlass);
        if (total == 0) total = 1;
        if (!state.backupX64.empty() && !state.x64Dir.empty()) {
            if (!CopyTree(state.backupX64, state.x64Dir, {}, copied, total, onProgress, copyError)) ok = false;
        }
        if (copyError != ERROR_DISK_FULL && !state.backupX86.empty() && !state.x86Dir.empty()) {
            if (!CopyTree(state.backupX86, state.x86Dir, {}, copied, total, onProgress, copyError)) ok = false;
        }
        if (copyError != ERROR_DISK_FULL && !state.backupOpenGlass.empty() && !state.openGlassDir.empty()) {
            if (!CopyTree(state.backupOpenGlass, state.openGlassDir, {}, copied, total, onProgress, copyError)) ok = false;
        }
    }
    if (copyError != 0) state.copyError = copyError;

    // Восстановить продуктовый реестр MSI в порядке: платформы -> настройки -> OpenGlass.
    auto restoreOne = [&](const ProductSnapshot& snap) {
        bool modified=false;
        for (const auto& code:state.modifiedProducts)
            if (_wcsicmp(code.c_str(),snap.productCode.c_str())==0) { modified=true; break; }
        if (!modified) return;
        if (GlassInstallMsi::IsProductInstalled(snap.productCode.c_str()) &&
            !snap.displayVersion.empty() && GlassInstallMsi::QueryDisplayVersion(snap.productCode.c_str())==snap.displayVersion)
            return;
        if (snap.cachedMsiPath.empty() || snap.installDir.empty()) { ok=false; return; }
        const std::wstring patched=state.workspaceDir+L"\\restore-"+snap.productCode+L".msi";
        if (!GlassInstallMsi::PrepareRestoreMsiCopy(snap.cachedMsiPath, patched)) {
            ok = false;
            return;
        }
        const std::wstring props = GlassInstallMsi::BuildInstallPropertyString(snap.installDir, true);
        const UINT code = GlassInstallMsi::InstallProduct(patched, props, nullptr);
        DeleteFileW(patched.c_str());
        if (!GlassInstallMsi::MsiSuccess(code)) {
            GlassInstallLog::LineFormat(L"Rollback reinstall failed %s exit %u", snap.productCode.c_str(), code);
            ok = false;
            return;
        }
        GlassInstallMsi::PatchArpUninstallString(snap.productCode, snap.installDir);
        const std::wstring now = GlassInstallMsi::QueryDisplayVersion(snap.productCode.c_str());
        if (!snap.displayVersion.empty() && _wcsicmp(now.c_str(), snap.displayVersion.c_str()) != 0)
            GlassInstallLog::Line(L"Rollback: DisplayVersion mismatch after restore");
    };

    const wchar_t* order[] = {
        GlassInstallMsi::kProductPlatformX64,
        GlassInstallMsi::kProductPlatformX86,
        GlassInstallMsi::kProductSettingsX64,
        GlassInstallMsi::kProductSettingsX86,
        GlassInstallMsi::kProductOpenGlass,
        L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5061}",
        L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5062}",
        L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5063}",
    };
    for (const wchar_t* wanted : order) {
        for (const auto& snap : state.oldProducts) {
            if (_wcsicmp(snap.productCode.c_str(), wanted) == 0)
                restoreOne(snap);
        }
    }
    if (!RestoreRegistryValues(state)) ok=false;
    if (onProgress) onProgress(10000);
    return ok;
}

bool RestoreRegistryValues(const SessionState& state) noexcept {
    bool ok=true;
    for (const auto& item:state.registryValues) {
        HKEY key=nullptr;
        if (item.existed) {
            if (RegCreateKeyExW(item.hive,item.key.c_str(),0,nullptr,0,KEY_SET_VALUE|item.view,nullptr,&key,nullptr)!=ERROR_SUCCESS) { ok=false; continue; }
            const LSTATUS result=RegSetValueExW(key,item.name.c_str(),0,item.type,
                item.data.empty()?nullptr:item.data.data(),static_cast<DWORD>(item.data.size()));
            if (result!=ERROR_SUCCESS) ok=false;
            RegCloseKey(key);
        } else {
            const LSTATUS result=RegOpenKeyExW(item.hive,item.key.c_str(),0,KEY_SET_VALUE|item.view,&key);
            if (result==ERROR_SUCCESS) {
                const LSTATUS deleted=RegDeleteValueW(key,item.name.c_str());
                if (deleted!=ERROR_SUCCESS && deleted!=ERROR_FILE_NOT_FOUND) ok=false;
                RegCloseKey(key);
            } else if (result!=ERROR_FILE_NOT_FOUND && result!=ERROR_PATH_NOT_FOUND) ok=false;
        }
    }
    return ok;
}

void CleanupSnapshot(SessionState& state) noexcept {
    if (!state.workspaceDir.empty()) {
        DeleteDirectoryRecursive(state.workspaceDir);
        state.workspaceDir.clear();
    }
    state.backupX64.clear();
    state.backupX86.clear();
    state.backupOpenGlass.clear();
}

} // namespace GlassInstallRollback
