#include "GlassInstallUnlock.h"
#include "GlassInstallUtil.h"
#include "GlassInstallLog.h"
#include <ForceUnloadAgent.h>
#include <ForceUnloadGlassModules.h>
#include <GlassPlatformTask.h>
#include <InjectModuleScan.h>
#include <InjectProcessMap.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <cwchar>
#include <iterator>

namespace {

namespace GP = GlassShared::GlassPlatformTask;

bool IsExplorerExeName(const wchar_t* exe) noexcept {
    return exe && _wcsicmp(exe, L"explorer.exe") == 0;
}

bool IsProtectedProcess(const wchar_t* exe) noexcept {
    if (!exe || !exe[0]) return true;
    const wchar_t* blocked[] = {
        L"System", L"Registry", L"smss.exe", L"csrss.exe", L"wininit.exe",
        L"services.exe", L"lsass.exe", L"winlogon.exe", L"dwm.exe",
        L"fontdrvhost.exe", L"Memory Compression", L"Secure System",
        L"GlassSetup.exe", L"unins000.exe",
    };
    for (const wchar_t* name : blocked)
        if (_wcsicmp(exe, name) == 0) return true;
    return false;
}

bool QueryTargetIsNative64(DWORD pid, bool& native64) noexcept {
    native64 = false;
    BOOL selfWow = FALSE;
    if (!IsWow64Process(GetCurrentProcess(), &selfWow)) return false;
    if (!selfWow) return true; // this Win32 wizard is on 32-bit Windows
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    BOOL targetWow = FALSE;
    const BOOL ok = IsWow64Process(process, &targetWow);
    CloseHandle(process);
    if (!ok) return false;
    native64 = !targetWow;
    return true;
}

bool ProcessAlive(DWORD pid) noexcept {
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process) return false;
    const DWORD wait = WaitForSingleObject(process, 0);
    CloseHandle(process);
    return wait == WAIT_TIMEOUT;
}

struct ProcessGuard {
    HANDLE handle = nullptr;
    ~ProcessGuard() { if (handle) CloseHandle(handle); }
};

bool OpenMatchingProcess(DWORD pid, const wchar_t* expected, ProcessGuard& guard) noexcept {
    if (!pid || !expected || !*expected) return false;
    guard.handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!guard.handle) return false;
    wchar_t path[32768]{};
    DWORD length = static_cast<DWORD>(std::size(path));
    const bool alive = WaitForSingleObject(guard.handle, 0) == WAIT_TIMEOUT;
    if (!alive || !QueryFullProcessImageNameW(guard.handle, 0, path, &length)) return false;
    const wchar_t* name = wcsrchr(path, L'\\');
    // Keep the handle until the action completes, preventing PID reuse after
    // this identity check from targeting an unrelated process.
    return _wcsicmp(name ? name + 1 : path, expected) == 0;
}

std::vector<GlassInstallUnlock::ModuleHolder> ScanNativeOneShot(
    const std::wstring& x64Dir,
    const std::wstring& x86Dir,
    bool& failed,
    const std::atomic_bool* cancelRequested) {
    failed = false;
    std::vector<GlassInstallUnlock::ModuleHolder> holders;
    BOOL selfWow = FALSE;
    if (!IsWow64Process(GetCurrentProcess(), &selfWow)) {
        failed = true;
        GlassInstallLog::LineFormat(L"Unlocker: could not determine process architecture (%lu)", GetLastError());
        return holders;
    }
    if (!selfWow) return holders;
    const std::wstring dll = GlassInstallUtil::NativeDllForArch(GP::PlatformArch::X64, x64Dir, x86Dir);
    const std::wstring rundll = GlassInstallUtil::NativeRundll64();
    if (dll.empty() || rundll.empty()) {
        failed = true;
        GlassInstallLog::Line(dll.empty()
            ? L"Unlocker: x64 GlassSettings.Native.dll not found"
            : L"Unlocker: Sysnative rundll32.exe not found");
        return holders;
    }
    std::wstring tempDir;
    if (!GlassInstallUtil::CreateSecureTempDirectory(L"GlassScan",tempDir)) {
        failed = true;
        return holders;
    }
    const std::wstring outPath=tempDir+L"\\scan.tsv";
    const std::wstring cmd = L"\"" + rundll + L"\" \"" + dll
        + L"\",GlassSettings_UnlockerW /scanGlassModulesSetup \"" + outPath + L"\"";
    DWORD exitCode = 1;
    if (!GlassInstallUtil::RunHiddenCommand(cmd, &exitCode, 120000, nullptr, nullptr, cancelRequested) || exitCode != 0) {
        failed = true;
        GlassInstallLog::LineFormat(L"Unlocker: x64 scan failed, exit %lu", exitCode);
    }
    HANDLE out = CreateFileW(outPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(out, &size) || size.QuadPart < 0 || size.QuadPart > 1024 * 1024) {
            failed = true;
        } else {
            std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
            DWORD got = 0;
            if (!bytes.empty() && (!ReadFile(out, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr) || got != bytes.size()))
                failed = true;
            if (!failed && !GlassInstallWatch::ParseNativePipeUtf8(bytes, holders)) failed = true;
        }
        CloseHandle(out);
    } else {
        failed = true;
    }
    if (failed) GlassInstallLog::Line(L"Unlocker: x64 scan output missing, incomplete or invalid");
    DeleteFileW(outPath.c_str());
    RemoveDirectoryW(tempDir.c_str());
    return holders;
}

} // namespace

namespace GlassInstallUnlock {

bool TargetIsWow64(DWORD pid) noexcept {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    BOOL wow = FALSE;
    const BOOL ok = IsWow64Process(process, &wow);
    CloseHandle(process);
    return ok && wow == TRUE;
}

void MergeInto(std::vector<ModuleHolder>& holders, const ModuleHolder& add) {
    if (add.pid == 0 || add.modules.empty()) return;
    if (_wcsicmp(add.exe.c_str(), L"GlassSetup.exe") == 0) return;
    if (_wcsicmp(add.exe.c_str(), L"unins000.exe") == 0) return;
    for (auto& h : holders) {
        if (h.pid != add.pid) continue;
        if (!add.exe.empty()) h.exe = add.exe;
        GlassInstallWatch::MergeModuleTokens(h.modules, add.modules);
        return;
    }
    ModuleHolder fresh{add.pid, add.exe, {}};
    GlassInstallWatch::MergeModuleTokens(fresh.modules, add.modules);
    holders.push_back(std::move(fresh));
}

std::vector<ModuleHolder> CollectHolders(
    const std::wstring& x64Dir,
    const std::wstring& x86Dir,
    bool scanNativeX64,
    bool& nativeScanFailed,
    const std::atomic_bool* cancelRequested) noexcept {
    nativeScanFailed = false;
    std::vector<ModuleHolder> holders;
    if (!GlassInstallWatch::CollectInProcessHolders(holders)) {
        nativeScanFailed = true;
        return holders;
    }
    if (cancelRequested && cancelRequested->load(std::memory_order_acquire)) return holders;
    if (scanNativeX64) {
        bool failed = false;
        const auto nativeRows = ScanNativeOneShot(x64Dir, x86Dir, failed, cancelRequested);
        for (const auto& native : nativeRows)
            MergeInto(holders, native);
        nativeScanFailed = nativeScanFailed || failed;
    }
    return holders;
}

std::wstring FormatHolders(const std::vector<ModuleHolder>& holders) {
    std::wstring text;
    const size_t limit = holders.size() < 24 ? holders.size() : 24;
    for (size_t i = 0; i < limit; ++i) {
        if (!text.empty()) text += L"\r\n";
        text += holders[i].exe;
        if (TargetIsWow64(holders[i].pid))
            text += L" (86)";
        text += L" (PID ";
        text += std::to_wstring(holders[i].pid);
        text += L"): ";
        text += holders[i].modules;
    }
    if (holders.size() > limit)
        text += L"\r\n…";
    return text;
}

BOOL CALLBACK PostCloseForProcess(HWND hwnd, LPARAM param) {
    DWORD windowPid = 0;
    GetWindowThreadProcessId(hwnd, &windowPid);
    if (windowPid == static_cast<DWORD>(param)) PostMessageW(hwnd, WM_CLOSE, 0, 0);
    return TRUE;
}

void CloseProcessWindows(DWORD pid) noexcept {
    EnumWindows(PostCloseForProcess, static_cast<LPARAM>(pid));
}

void CloseMatchingProcessWindows(DWORD pid, const wchar_t* exe) noexcept {
    if (pid == GetCurrentProcessId() || IsProtectedProcess(exe)) return;
    ProcessGuard process;
    if (OpenMatchingProcess(pid, exe, process)) CloseProcessWindows(pid);
}

BOOL CALLBACK PostCloseExplorerFileWindows(HWND hwnd, LPARAM param) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != static_cast<DWORD>(param)) return TRUE;
    wchar_t cls[80]{};
    if (GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls))) &&
        (_wcsicmp(cls, L"CabinetWClass") == 0 || _wcsicmp(cls, L"ExploreWClass") == 0))
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    return TRUE;
}

bool ForceUnloadHolder(const ModuleHolder& holder, const std::wstring& x64Dir,
    const std::wstring& x86Dir) noexcept {
    const DWORD pid = holder.pid;
    if (pid == GetCurrentProcessId()) {
        (void)Shared::InjectProcessMap::SignalUnloadRequest(pid);
        return false; // never force-unload the installer itself
    }
    if (IsProtectedProcess(holder.exe.c_str())) return false;
    ProcessGuard process;
    if (!OpenMatchingProcess(pid, holder.exe.c_str(), process)) {
        GlassInstallLog::LineFormat(L"Unlocker: holder identity changed or is unavailable, pid %lu", pid);
        return false;
    }
    if (_wcsicmp(holder.exe.c_str(), L"GlassSettings.exe") == 0) {
        CloseProcessWindows(pid);
        return false;
    }
    if (IsExplorerExeName(holder.exe.c_str())) {
        if (Shared::InjectProcessMap::SignalUnloadRequest(pid))
            Sleep(1200);
        return false;
    }
    if (Shared::InjectProcessMap::SignalUnloadRequest(pid))
        Sleep(500);

    bool native64 = false;
    if (!QueryTargetIsNative64(pid, native64)) return false;
    if (!native64) {
#ifdef GLASS_SETUP_NATIVE_MONITOR
        // Use the same x86 GlassSettings.Native instance that performs the
        // scan, rather than a second installer-specific unload implementation.
        return GlassInstallWatch::ForceUnloadInLocalNative(pid);
#else
        const auto result = Shared::ForceUnloadGlassModules::ForceUnload(pid);
        if (result == Shared::ForceUnloadGlassModules::ForceUnloadResult::Ok)
            return true;
        if (result == Shared::ForceUnloadGlassModules::ForceUnloadResult::NeedsX86Agent) {
            // The uninstaller uses the same resolved InstallPath as MSI and
            // never replaces it with a task or another registry candidate.
            const std::wstring compat = GlassInstallUtil::TrimTrailingSlashes(x86Dir) + L"\\GlassCompat.dll";
            if (x86Dir.empty() || !GlassInstallUtil::IsSafeInstallPath(compat)) return false;
            return Shared::ForceUnloadAgent::SpawnX86ForceUnloadAgent(pid, x86Dir.c_str());
        }
        return false;
#endif
    }

    const std::wstring dll = GlassInstallUtil::NativeDllForArch(GP::PlatformArch::X64, x64Dir, x86Dir);
    const std::wstring rundll = GlassInstallUtil::NativeRundll64();
    if (dll.empty() || rundll.empty()) return false;
    const std::wstring command = L"\"" + rundll + L"\" \"" + dll + L"\",GlassSettings_UnlockerW /forceUnload "
        + std::to_wstring(pid);
    DWORD exitCode = 1;
    if (!GlassInstallUtil::RunHiddenCommand(command, &exitCode, 20000)) return false;
    return exitCode == 0;
}

void UnlockProcess(DWORD pid, const wchar_t* exe) noexcept {
    if (pid == GetCurrentProcessId()) {
        (void)Shared::InjectProcessMap::SignalUnloadRequest(pid);
        return; // never close or terminate the wizard itself
    }
    if (IsProtectedProcess(exe)) return;
    ProcessGuard guard;
    if (!OpenMatchingProcess(pid, exe, guard)) {
        GlassInstallLog::LineFormat(L"Unlocker: process identity changed or is unavailable, pid %lu", pid);
        return;
    }
    if (IsExplorerExeName(exe)) {
        // Never send WM_CLOSE to the desktop, taskbar, or other shell windows.
        // Only close explicit File Explorer windows after the user's consent;
        // the shell process itself is neither terminated nor force-unloaded.
        (void)Shared::InjectProcessMap::SignalUnloadRequest(pid);
        EnumWindows(PostCloseExplorerFileWindows, static_cast<LPARAM>(pid));
        return;
    }
    CloseProcessWindows(pid);
    const ULONGLONG start = GetTickCount64();
    while (ProcessAlive(pid) && GetTickCount64() - start < 8000)
        Sleep(200);
    if (!ProcessAlive(pid)) return;
    HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (!process) return;
    TerminateProcess(process, 1);
    WaitForSingleObject(process, 5000);
    CloseHandle(process);
}

} // namespace GlassInstallUnlock
