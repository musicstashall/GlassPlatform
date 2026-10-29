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
    };
    for (const wchar_t* name : blocked)
        if (_wcsicmp(exe, name) == 0) return true;
    return false;
}

bool TargetIsNative64(DWORD pid) noexcept {
    BOOL selfWow = FALSE;
    if (!IsWow64Process(GetCurrentProcess(), &selfWow) || !selfWow)
        return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    BOOL targetWow = FALSE;
    const BOOL ok = IsWow64Process(process, &targetWow);
    CloseHandle(process);
    return ok && !targetWow;
}

bool ProcessAlive(DWORD pid) noexcept {
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process) return false;
    const DWORD wait = WaitForSingleObject(process, 0);
    CloseHandle(process);
    return wait == WAIT_TIMEOUT;
}

std::vector<GlassInstallUnlock::ModuleHolder> ScanNativeOneShot(
    const std::wstring& x64Dir,
    const std::wstring& x86Dir,
    bool& failed) {
    failed = false;
    std::vector<GlassInstallUnlock::ModuleHolder> holders;
    BOOL selfWow = FALSE;
    if (!IsWow64Process(GetCurrentProcess(), &selfWow) || !selfWow)
        return holders;
    const std::wstring dll = GlassInstallUtil::NativeDllForArch(GP::PlatformArch::X64, x64Dir, x86Dir);
    const std::wstring rundll = GlassInstallUtil::NativeRundll64();
    if (dll.empty() || rundll.empty()) {
        failed = true;
        GlassInstallLog::Line(dll.empty()
            ? L"Unlocker: x64 GlassSettings.Native.dll not found"
            : L"Unlocker: Sysnative rundll32.exe not found");
        return holders;
    }
    wchar_t tempDir[MAX_PATH]{};
    wchar_t outPath[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, tempDir) || !GetTempFileNameW(tempDir, L"gsh", 0, outPath)) {
        failed = true;
        return holders;
    }
    const std::wstring cmd = L"\"" + rundll + L"\" \"" + dll
        + L"\",GlassSettings_UnlockerW /scanGlassModules \"" + outPath + L"\"";
    DWORD exitCode = 1;
    if (!GlassInstallUtil::RunHiddenCommand(cmd, &exitCode, 120000) || exitCode != 0) {
        failed = true;
        GlassInstallLog::LineFormat(L"Unlocker: x64 scan failed, exit %lu", exitCode);
    }
    HANDLE out = CreateFileW(outPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out != INVALID_HANDLE_VALUE) {
        DWORD size = GetFileSize(out, nullptr);
        std::string bytes(size && size != INVALID_FILE_SIZE ? size : 0, '\0');
        DWORD got = 0;
        if (!bytes.empty())
            ReadFile(out, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr);
        CloseHandle(out);
        bytes.resize(got);
        GlassInstallWatch::ParseNativePipeUtf8(bytes, holders);
    } else if (!failed) {
        failed = true;
    }
    DeleteFileW(outPath);
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
    for (auto& h : holders) {
        if (h.pid != add.pid) continue;
        if (!add.exe.empty()) h.exe = add.exe;
        if (!add.modules.empty()) {
            if (!h.modules.empty() && h.modules.find(add.modules) == std::wstring::npos)
                h.modules += L", ";
            if (h.modules.find(add.modules) == std::wstring::npos)
                h.modules += add.modules;
        }
        return;
    }
    holders.push_back(add);
}

std::vector<ModuleHolder> CollectHolders(
    const std::wstring& x64Dir,
    const std::wstring& x86Dir,
    bool scanNativeX64,
    bool& nativeScanFailed) noexcept {
    nativeScanFailed = false;
    std::vector<ModuleHolder> holders;
    GlassInstallWatch::CollectInProcessHolders(holders);
    if (scanNativeX64) {
        bool failed = false;
        const auto nativeRows = ScanNativeOneShot(x64Dir, x86Dir, failed);
        for (const auto& native : nativeRows)
            MergeInto(holders, native);
        nativeScanFailed = failed;
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

void CloseProcessWindows(DWORD pid) noexcept {
    EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
        DWORD windowPid = 0;
        GetWindowThreadProcessId(hwnd, &windowPid);
        if (windowPid == static_cast<DWORD>(param))
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return TRUE;
    }, static_cast<LPARAM>(pid));
}

bool ForceUnloadHolder(const ModuleHolder& holder, const std::wstring& x64Dir,
    const std::wstring& x86Dir) noexcept {
    const DWORD pid = holder.pid;
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

    if (!TargetIsNative64(pid)) {
        const auto result = Shared::ForceUnloadGlassModules::ForceUnload(pid);
        if (result == Shared::ForceUnloadGlassModules::ForceUnloadResult::Ok)
            return true;
        if (result == Shared::ForceUnloadGlassModules::ForceUnloadResult::NeedsX86Agent) {
            std::wstring root = Shared::ForceUnloadAgent::TryGetX86InstallRoot();
            if (root.empty()) root = x86Dir;
            return Shared::ForceUnloadAgent::SpawnX86ForceUnloadAgent(pid, root.c_str());
        }
        return false;
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
    if (IsExplorerExeName(exe)) {
        CloseProcessWindows(pid);
        return;
    }
    if (IsProtectedProcess(exe)) return;
    CloseProcessWindows(pid);
    const DWORD start = GetTickCount();
    while (ProcessAlive(pid) && GetTickCount() - start < 8000)
        Sleep(200);
    if (!ProcessAlive(pid)) return;
    HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (!process) return;
    TerminateProcess(process, 1);
    WaitForSingleObject(process, 5000);
    CloseHandle(process);
}

} // namespace GlassInstallUnlock
