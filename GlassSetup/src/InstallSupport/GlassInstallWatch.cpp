#include "GlassInstallWatch.h"
#include "GlassInstallUtil.h"
#include "GlassInstallLog.h"
#include <GlassPlatformTask.h>
#include <InjectModuleScan.h>
#include <InjectProcessMap.h>
#include <shlwapi.h>
#include <cstring>
#include <string>

namespace {

namespace GP = GlassShared::GlassPlatformTask;

void MergeModuleList(std::wstring& into, const std::wstring& add) {
    if (add.empty()) return;
    size_t start = 0;
    while (start < add.size()) {
        size_t comma = add.find(L", ", start);
        const std::wstring token = add.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start);
        if (!token.empty() && into.find(token) == std::wstring::npos) {
            if (!into.empty()) into += L", ";
            into += token;
        }
        if (comma == std::wstring::npos) break;
        start = comma + 2;
    }
}

void MergeHolder(std::vector<GlassInstallWatch::ModuleHolder>& holders, DWORD pid,
    const std::wstring& exe, const std::wstring& modules) {
    if (pid == 0 || modules.empty()) return;
    if (_wcsicmp(exe.c_str(), L"GlassSetup.exe") == 0) return;
    if (_wcsicmp(exe.c_str(), L"unins000.exe") == 0) return;
    for (auto& holder : holders) {
        if (holder.pid != pid) continue;
        if (!exe.empty()) holder.exe = exe;
        MergeModuleList(holder.modules, modules);
        return;
    }
    holders.push_back({pid, exe, modules});
}

std::wstring ModuleNamesFromBits(std::uint32_t bits) {
    std::wstring text;
    auto add = [&](std::uint32_t flag, const wchar_t* name) {
        if ((bits & flag) == 0) return;
        if (!text.empty()) text += L", ";
        text += name;
    };
    add(Shared::InjectModuleScan::ModuleCompat, L"GlassCompat.dll");
    add(Shared::InjectModuleScan::ModuleCore, L"GlassCore.dll");
    add(Shared::InjectModuleScan::ModuleWnd, L"WndCompat.dll");
    add(Shared::InjectModuleScan::ModuleUICompat, L"UICompat.dll");
    add(Shared::InjectModuleScan::ModuleExplorer, L"ExplorerCompat.dll");
    add(Shared::InjectModuleScan::ModuleTaskbar, L"TaskbarCompat.dll");
    return text;
}

void AppendScanResults(std::vector<GlassInstallWatch::ModuleHolder>& holders,
    const Shared::InjectModuleScan::Result* results, DWORD count) {
    for (DWORD i = 0; i < count; ++i) {
        const std::wstring modules = ModuleNamesFromBits(results[i].moduleBits);
        MergeHolder(holders, results[i].pid, results[i].exeName, modules);
    }
}

void ScanInjectMapInto(std::vector<GlassInstallWatch::ModuleHolder>& holders, const wchar_t* mapName) {
    Shared::InjectProcessMap::Slot slots[Shared::InjectProcessMap::kSlotCount]{};
    DWORD slotCount = 0;
    if (!Shared::InjectProcessMap::TryReadSlotsFromMapping(
            mapName, slots, Shared::InjectProcessMap::kSlotCount, &slotCount))
        return;
    std::vector<DWORD> pids;
    pids.reserve(slotCount);
    for (DWORD i = 0; i < slotCount; ++i) {
        if (slots[i].pid == 0 || slots[i].moduleBits == 0) continue;
        pids.push_back(slots[i].pid);
    }
    if (pids.empty()) return;
    Shared::InjectModuleScan::Result results[Shared::InjectModuleScan::kMaxResults]{};
    DWORD count = 0;
    if (Shared::InjectModuleScan::Scan(
            pids.data(), static_cast<DWORD>(pids.size()), results,
            Shared::InjectModuleScan::kMaxResults, &count))
        AppendScanResults(holders, results, count);
}

bool NeedNativeX64Helper() noexcept {
    BOOL selfWow = FALSE;
    return IsWow64Process(GetCurrentProcess(), &selfWow) && selfWow;
}

void ConsumePipeBuffer(std::string& buffer, std::vector<GlassInstallWatch::ModuleHolder>& tickOut,
    bool& gotEmpty, bool& gotEnd) {
    gotEmpty = false;
    gotEnd = false;
    tickOut.clear();
    for (;;) {
        const size_t nl = buffer.find('\n');
        if (nl == std::string::npos) break;
        std::string line = buffer.substr(0, nl);
        buffer.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "#empty") {
            gotEmpty = true;
            return;
        }
        if (line == "#end") {
            gotEnd = true;
            return;
        }
        GlassInstallWatch::ParseNativePipeUtf8(line + "\n", tickOut);
    }
}

} // namespace

namespace GlassInstallWatch {

void ParseNativePipeUtf8(const std::string& bytes, std::vector<ModuleHolder>& holders) {
    int wideCount = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    std::wstring text(wideCount > 0 ? static_cast<size_t>(wideCount) : 0, L'\0');
    if (wideCount > 0)
        MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), text.data(), wideCount);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find(L'\n', pos);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        pos = end + 1;
        const size_t tab1 = line.find(L'\t');
        const size_t tab2 = tab1 == std::wstring::npos ? std::wstring::npos : line.find(L'\t', tab1 + 1);
        if (tab1 == std::wstring::npos || tab2 == std::wstring::npos) continue;
        const DWORD pid = static_cast<DWORD>(wcstoul(line.substr(0, tab1).c_str(), nullptr, 10));
        MergeHolder(holders, pid, line.substr(tab1 + 1, tab2 - tab1 - 1), line.substr(tab2 + 1));
    }
}

void CollectInProcessHolders(std::vector<ModuleHolder>& holders) noexcept {
    (void)GP::RefreshInjectResidualSticky(GP::PlatformArch::X64);
    (void)GP::RefreshInjectResidualSticky(GP::PlatformArch::X86);
    Shared::InjectModuleScan::Result results[Shared::InjectModuleScan::kMaxResults]{};
    DWORD count = 0;
    if (Shared::InjectModuleScan::Scan(
            nullptr, 0, results, Shared::InjectModuleScan::kMaxResults, &count))
        AppendScanResults(holders, results, count);
    ScanInjectMapInto(holders, Shared::InjectProcessMap::kMappingNameX86);
    ScanInjectMapInto(holders, Shared::InjectProcessMap::kMappingNameX64);
    ScanInjectMapInto(holders, Shared::InjectProcessMap::kMappingNameLegacy);
}

bool WaitUntilClearOrTimeout(
    DWORD timeoutMs,
    const std::wstring& x64Dir,
    const std::wstring& x86Dir,
    bool scanNativeX64,
    std::vector<ModuleHolder>& lastHolders,
    bool& nativeScanFailed) noexcept {
    nativeScanFailed = false;
    lastHolders.clear();

    const DWORD waitStart = GetTickCount();
    auto waitExpired = [&]() -> bool { return GetTickCount() - waitStart >= timeoutMs; };
    HANDLE pipe = INVALID_HANDLE_VALUE;
    HANDLE watchProcess = nullptr;
    bool pipeConnected = false;
    std::string pipeBuffer;
    std::vector<ModuleHolder> x64Tick;
    bool x64Empty = !scanNativeX64 || !NeedNativeX64Helper();
    DWORD lastBeat = waitStart;

    if (scanNativeX64 && NeedNativeX64Helper()) {
        const std::wstring dll = GlassInstallUtil::NativeDllForArch(GP::PlatformArch::X64, x64Dir, x86Dir);
        const std::wstring rundll = GlassInstallUtil::NativeRundll64();
        if (dll.empty() || rundll.empty()) {
            nativeScanFailed = true;
            GlassInstallLog::Line(L"Watch: x64 Native.dll or Sysnative rundll32 missing");
        } else {
            wchar_t pipeName[128]{};
            swprintf_s(pipeName, L"\\\\.\\pipe\\GlassWatch-%u", GetTickCount());
            // PIPE_NOWAIT: ConnectNamedPipe не блокирует поток. PIPE_WAIT вешал установщик
            // навсегда, если rundll32 завершался до подключения — таймаут и локальный скан не начинались.
            pipe = CreateNamedPipeW(pipeName, PIPE_ACCESS_INBOUND,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT, 1, 65536, 65536, 0, nullptr);
            if (pipe == INVALID_HANDLE_VALUE) {
                nativeScanFailed = true;
                GlassInstallLog::LineFormat(L"Watch: CreateNamedPipe failed %lu", GetLastError());
            } else {
                (void)ConnectNamedPipe(pipe, nullptr);
                const std::wstring cmd = L"\"" + rundll + L"\" \"" + dll
                    + L"\",GlassSettings_UnlockerW /watchGlassModules \"" + pipeName + L"\" "
                    + std::to_wstring(timeoutMs);
                std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
                cmdBuf.push_back(L'\0');
                STARTUPINFOW si{};
                si.cb = sizeof(si);
                si.dwFlags = STARTF_USESHOWWINDOW;
                si.wShowWindow = SW_HIDE;
                PROCESS_INFORMATION pi{};
                if (CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
                    CloseHandle(pi.hThread);
                    watchProcess = pi.hProcess;
                    GlassInstallLog::LineFormat(L"Watch: started resident scan (%s) dll=%s", pipeName, dll.c_str());
                } else {
                    nativeScanFailed = true;
                    GlassInstallLog::LineFormat(L"Watch: CreateProcess failed %lu", GetLastError());
                    CloseHandle(pipe);
                    pipe = INVALID_HANDLE_VALUE;
                }
            }
        }
    }

    for (;;) {
        if (waitExpired()) {
            GlassInstallLog::LineFormat(L"Watch: timeout %lu ms, holders=%u",
                timeoutMs, static_cast<unsigned>(lastHolders.size()));
            break;
        }

        if (pipe != INVALID_HANDLE_VALUE && !pipeConnected) {
            if (ConnectNamedPipe(pipe, nullptr)) {
                pipeConnected = true;
            } else {
                const DWORD err = GetLastError();
                if (err == ERROR_PIPE_CONNECTED) {
                    pipeConnected = true;
                } else if (err != ERROR_PIPE_LISTENING) {
                    GlassInstallLog::LineFormat(L"Watch: pipe connect failed %lu", err);
                    CloseHandle(pipe);
                    pipe = INVALID_HANDLE_VALUE;
                }
            }
            if (pipeConnected)
                GlassInstallLog::Line(L"Watch: pipe connected");
        }

        if (watchProcess && WaitForSingleObject(watchProcess, 0) == WAIT_OBJECT_0) {
            DWORD code = 1;
            GetExitCodeProcess(watchProcess, &code);
            GlassInstallLog::LineFormat(L"Watch: helper exit %lu", code);
            CloseHandle(watchProcess);
            watchProcess = nullptr;
        }

        if (pipeConnected && pipe != INVALID_HANDLE_VALUE) {
            char chunk[4096]{};
            DWORD avail = 0;
            if (PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
                DWORD read = 0;
                const DWORD toRead = avail > sizeof(chunk) ? sizeof(chunk) : avail;
                if (ReadFile(pipe, chunk, toRead, &read, nullptr) && read > 0)
                    pipeBuffer.append(chunk, chunk + read);
            }
            for (;;) {
                bool emptyMark = false;
                bool endMark = false;
                std::vector<ModuleHolder> tick;
                ConsumePipeBuffer(pipeBuffer, tick, emptyMark, endMark);
                if (emptyMark) {
                    x64Empty = true;
                    x64Tick.clear();
                    break;
                }
                if (endMark) {
                    for (const auto& row : tick)
                        MergeHolder(x64Tick, row.pid, row.exe, row.modules);
                    break;
                }
                if (tick.empty() && !endMark && !emptyMark) break;
            }
        }

        lastHolders.clear();
        CollectInProcessHolders(lastHolders);
        for (const auto& row : x64Tick)
            MergeHolder(lastHolders, row.pid, row.exe, row.modules);

        if (GetTickCount() - lastBeat >= 5000) {
            lastBeat = GetTickCount();
            GlassInstallLog::LineFormat(L"Watch: still held %u pipe=%d",
                static_cast<unsigned>(lastHolders.size()), pipeConnected ? 1 : 0);
        }

        if (x64Empty && lastHolders.empty()) {
            if (pipe != INVALID_HANDLE_VALUE) {
                DisconnectNamedPipe(pipe);
                CloseHandle(pipe);
                pipe = INVALID_HANDLE_VALUE;
            }
            if (watchProcess) {
                WaitForSingleObject(watchProcess, 2000);
                CloseHandle(watchProcess);
                watchProcess = nullptr;
            }
            return true;
        }

        Sleep(400);
    }

    if (pipe != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
    if (watchProcess) {
        TerminateProcess(watchProcess, 1);
        WaitForSingleObject(watchProcess, 3000);
        CloseHandle(watchProcess);
    }
    return false;
}

} // namespace GlassInstallWatch
