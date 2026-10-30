#include "GlassInstallWatch.h"
#include "GlassInstallUtil.h"
#include "GlassInstallLog.h"
#include <GlassPlatformTask.h>
#include <InjectModuleScan.h>
#include <InjectProcessMap.h>
#include <shlwapi.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>

namespace {

namespace GP = GlassShared::GlassPlatformTask;

std::wstring TrimToken(std::wstring s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == L' ' || s[b] == L'\t')) ++b;
    size_t e = s.size();
    while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t')) --e;
    return s.substr(b, e - b);
}

bool ContainsModuleToken(const std::wstring& list, const std::wstring& candidate) {
    if (candidate.empty()) return true;
    size_t pos = 0;
    while (pos < list.size()) {
        size_t comma = list.find(L',', pos);
        const std::wstring item = TrimToken(list.substr(
            pos, comma == std::wstring::npos ? std::wstring::npos : comma - pos));
        if (!item.empty() && _wcsicmp(item.c_str(), candidate.c_str()) == 0)
            return true;
        if (comma == std::wstring::npos) break;
        pos = comma + 1;
    }
    return false;
}

void MergeHolder(std::vector<GlassInstallWatch::ModuleHolder>& holders, DWORD pid,
    const std::wstring& exe, const std::wstring& modules) {
    if (pid == 0 || modules.empty()) return;
    for (auto& holder : holders) {
        if (holder.pid != pid) continue;
        if (!exe.empty()) holder.exe = exe;
        GlassInstallWatch::MergeModuleTokens(holder.modules, modules);
        return;
    }
    GlassInstallWatch::ModuleHolder fresh{pid, exe, {}};
    GlassInstallWatch::MergeModuleTokens(fresh.modules, modules);
    holders.push_back(std::move(fresh));
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

bool ScanInjectMapInto(std::vector<GlassInstallWatch::ModuleHolder>& holders, const wchar_t* mapName) {
    Shared::InjectProcessMap::Slot slots[Shared::InjectProcessMap::kSlotCount]{};
    DWORD slotCount = 0;
    if (!Shared::InjectProcessMap::TryReadSlotsFromMapping(
            mapName, slots, Shared::InjectProcessMap::kSlotCount, &slotCount))
        return true; // the mapping is optional when the corresponding host is absent
    std::vector<DWORD> pids;
    pids.reserve(slotCount);
    for (DWORD i = 0; i < slotCount; ++i) {
        if (slots[i].pid == 0 || slots[i].moduleBits == 0) continue;
        pids.push_back(slots[i].pid);
    }
    if (pids.empty()) return true;
    Shared::InjectModuleScan::Result results[Shared::InjectModuleScan::kMaxResults]{};
    DWORD count = 0;
    if (!Shared::InjectModuleScan::ScanForSetup(
            pids.data(), static_cast<DWORD>(pids.size()), results,
            Shared::InjectModuleScan::kMaxResults, &count)) return false;
    AppendScanResults(holders, results, count);
    return true;
}

bool NeedNativeX64Helper() noexcept {
#ifdef _WIN64
    return false;
#else
    BOOL selfWow = FALSE;
    // On an x64 OS an unknown WOW64 state must not silently skip the helper.
    return !IsWow64Process(GetCurrentProcess(), &selfWow) || selfWow;
#endif
}

bool ConsumeNextPipeEvent(
    std::string& buffer,
    std::vector<GlassInstallWatch::ModuleHolder>& pendingRows,
    bool& gotEmpty,
    bool& gotEnd,
    bool& malformed) {
    gotEmpty = false;
    gotEnd = false;
    malformed = false;
    bool progressed = false;
    for (;;) {
        const size_t nl = buffer.find('\n');
        if (nl == std::string::npos) break;
        progressed = true;
        std::string line = buffer.substr(0, nl);
        buffer.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "#empty") {
            malformed = !pendingRows.empty();
            gotEmpty = !malformed;
            return true;
        }
        if (line == "#end") {
            // A non-empty frame must contain at least one usable holder.
            // In particular, never treat an incomplete/filtered frame as empty.
            malformed = pendingRows.empty();
            gotEnd = !malformed;
            return true;
        }
        if (!GlassInstallWatch::ParseNativePipeUtf8(line + "\n", pendingRows)) {
            malformed = true;
            return true;
        }
    }
    return progressed;
}

} // namespace

namespace GlassInstallWatch {

void MergeModuleTokens(std::wstring& into, const std::wstring& add) {
    if (add.empty()) return;
    size_t pos = 0;
    while (pos < add.size()) {
        size_t comma = add.find(L',', pos);
        const std::wstring token = TrimToken(add.substr(
            pos, comma == std::wstring::npos ? std::wstring::npos : comma - pos));
        if (!token.empty() && !ContainsModuleToken(into, token)) {
            if (!into.empty()) into += L", ";
            into += token;
        }
        if (comma == std::wstring::npos) break;
        pos = comma + 1;
    }
}

bool ParseNativePipeUtf8(const std::string& bytes, std::vector<ModuleHolder>& holders) {
    if (bytes.empty()) return true;
    if (bytes.size() > 1024 * 1024 || bytes.back() != '\n') return false;
    const int wideCount = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (wideCount <= 0) return false;
    std::wstring text(static_cast<size_t>(wideCount), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
            static_cast<int>(bytes.size()), text.data(), wideCount) != wideCount) return false;
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t end = text.find(L'\n', pos);
        if (end == std::wstring::npos) return false;
        std::wstring line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        pos = end + 1;
        const size_t tab1 = line.find(L'\t');
        const size_t tab2 = tab1 == std::wstring::npos ? std::wstring::npos : line.find(L'\t', tab1 + 1);
        if (tab1 == 0 || tab1 == std::wstring::npos || tab2 == std::wstring::npos ||
            tab2 == tab1 + 1 || tab2 + 1 == line.size() || line.find(L'\t', tab2 + 1) != std::wstring::npos)
            return false;
        unsigned long long pid = 0;
        for (size_t i = 0; i < tab1; ++i) {
            if (line[i] < L'0' || line[i] > L'9') return false;
            pid = pid * 10 + static_cast<unsigned>(line[i] - L'0');
            if (pid > 0xffffffffULL) return false;
        }
        if (!pid) return false;
        MergeHolder(holders, static_cast<DWORD>(pid), line.substr(tab1 + 1, tab2 - tab1 - 1), line.substr(tab2 + 1));
    }
    return true;
}

bool CollectInProcessHolders(std::vector<ModuleHolder>& holders) noexcept {
    Shared::InjectModuleScan::Result results[Shared::InjectModuleScan::kMaxResults]{};
    DWORD count = 0;
    if (!Shared::InjectModuleScan::ScanForSetup(
            nullptr, 0, results, Shared::InjectModuleScan::kMaxResults, &count)) {
        GlassInstallLog::Line(L"Watch: local process scan was incomplete (including Explorer)");
        return false;
    }
    AppendScanResults(holders, results, count);
    // The shared Toolhelp scanner skips itself. The wizard can be injected
    // while the old platform is running, so check our own modules explicitly;
    // never overwrite DLLs still loaded by Setup/Unins.
    std::uint32_t selfBits = 0;
    using namespace Shared::InjectModuleScan;
    if (GetModuleHandleW(L"GlassCompat.dll")) selfBits |= ModuleCompat;
    if (GetModuleHandleW(L"GlassCore.dll")) selfBits |= ModuleCore;
    if (GetModuleHandleW(L"WndCompat.dll")) selfBits |= ModuleWnd;
    if (GetModuleHandleW(L"UICompat.dll")) selfBits |= ModuleUICompat;
    if (GetModuleHandleW(L"ExplorerCompat.dll")) selfBits |= ModuleExplorer;
    if (GetModuleHandleW(L"TaskbarCompat.dll")) selfBits |= ModuleTaskbar;
    if (selfBits) {
        wchar_t selfPath[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr, selfPath, static_cast<DWORD>(std::size(selfPath)));
        if (!length || length >= std::size(selfPath)) return false;
        const wchar_t* name = wcsrchr(selfPath, L'\\');
        MergeHolder(holders, GetCurrentProcessId(), name ? name + 1 : selfPath, ModuleNamesFromBits(selfBits));
        (void)Shared::InjectProcessMap::SignalUnloadRequest(GetCurrentProcessId());
    }
    if (!ScanInjectMapInto(holders, Shared::InjectProcessMap::kMappingNameX86) ||
        !ScanInjectMapInto(holders, Shared::InjectProcessMap::kMappingNameX64) ||
        !ScanInjectMapInto(holders, Shared::InjectProcessMap::kMappingNameLegacy)) {
        GlassInstallLog::Line(L"Watch: a tracked process could not be inspected");
        return false;
    }

    // Shared::Scan deliberately excludes GlassSettings itself. An instance can
    // still hold its EXE/MSI files even without any injected Glass DLLs.
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool ok = true;
    if (Process32FirstW(snap, &entry)) {
        do {
            if (entry.th32ProcessID != GetCurrentProcessId() &&
                _wcsicmp(entry.szExeFile, L"GlassSettings.exe") == 0)
                MergeHolder(holders, entry.th32ProcessID, entry.szExeFile, L"GlassSettings.exe");
        } while (Process32NextW(snap, &entry));
        ok = GetLastError() == ERROR_NO_MORE_FILES;
    } else ok = GetLastError() == ERROR_NO_MORE_FILES;
    CloseHandle(snap);
    if (!ok) GlassInstallLog::Line(L"Watch: GlassSettings process enumeration failed");
    return ok;
}

bool WaitUntilClearOrTimeout(
    DWORD timeoutMs,
    const std::wstring& x64Dir,
    const std::wstring& x86Dir,
    bool scanNativeX64,
    std::vector<ModuleHolder>& lastHolders,
    bool& nativeScanFailed,
    const std::atomic<bool>* cancelRequested) noexcept {
    nativeScanFailed = false;
    lastHolders.clear();

    const ULONGLONG waitStart = GetTickCount64();
    auto waitExpired = [&]() -> bool { return GetTickCount64() - waitStart >= timeoutMs; };
    HANDLE pipe = INVALID_HANDLE_VALUE;
    HANDLE watchProcess = nullptr;
    bool pipeConnected = false;
    bool receivedFrame = false;
    std::string pipeBuffer;
    std::vector<ModuleHolder> pendingRows;
    std::vector<ModuleHolder> x64Tick;
    bool x64Empty = !scanNativeX64 || !NeedNativeX64Helper();
    ULONGLONG lastBeat = waitStart;
    ULONGLONG lastFrame = 0;
    DWORD helperExitCode=STILL_ACTIVE;

    if (scanNativeX64 && NeedNativeX64Helper()) {
        const std::wstring dll = GlassInstallUtil::NativeDllForArch(GP::PlatformArch::X64, x64Dir, x86Dir);
        const std::wstring rundll = GlassInstallUtil::NativeRundll64();
        if (dll.empty() || rundll.empty()) {
            nativeScanFailed = true;
            GlassInstallLog::Line(L"Watch: x64 Native.dll or Sysnative rundll32 missing");
        } else {
            wchar_t pipeName[128]{};
            swprintf_s(pipeName, L"\\\\.\\pipe\\GlassWatch-%lu-%llu",
                GetCurrentProcessId(), static_cast<unsigned long long>(GetTickCount64()));
            PSECURITY_DESCRIPTOR descriptor=nullptr;
            SECURITY_ATTRIBUTES security{sizeof(security),nullptr,FALSE};
            if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
                    L"D:P(A;;GA;;;SY)(A;;GA;;;BA)",SDDL_REVISION_1,&descriptor,nullptr)) {
                security.lpSecurityDescriptor=descriptor;
                pipe = CreateNamedPipeW(pipeName, PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS,
                    1, 65536, 65536, 0, &security);
                LocalFree(descriptor);
            }
            if (pipe == INVALID_HANDLE_VALUE) {
                nativeScanFailed = true;
                GlassInstallLog::LineFormat(L"Watch: CreateNamedPipe failed %lu", GetLastError());
            } else {
                (void)ConnectNamedPipe(pipe, nullptr);
                const std::wstring cmd = L"\"" + rundll + L"\" \"" + dll
                    + L"\",GlassSettings_UnlockerW /watchGlassModulesSetup \"" + pipeName + L"\" "
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
        if (nativeScanFailed) break;
        if (cancelRequested && cancelRequested->load(std::memory_order_acquire)) {
            GlassInstallLog::Line(L"Watch: aborted by user cancel");
            break;
        }
        if (waitExpired()) {
            if (scanNativeX64 && NeedNativeX64Helper() &&
                (!receivedFrame || !pendingRows.empty() || !pipeBuffer.empty())) nativeScanFailed=true;
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
                    nativeScanFailed = true;
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
            helperExitCode=code;
            CloseHandle(watchProcess);
            watchProcess = nullptr;
        }

        if (pipeConnected && pipe != INVALID_HANDLE_VALUE) {
            char chunk[4096]{};
            DWORD avail = 0;
            for (;;) {
                if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr)) {
                    nativeScanFailed=true;
                    GlassInstallLog::LineFormat(L"Watch: pipe read failed %lu", GetLastError());
                    break;
                }
                if (!avail) break;
                DWORD read = 0;
                const DWORD toRead = avail > sizeof(chunk) ? sizeof(chunk) : avail;
                if (!ReadFile(pipe, chunk, toRead, &read, nullptr) || read == 0) {
                    nativeScanFailed=true;
                    break;
                }
                pipeBuffer.append(chunk, chunk + read);
                if (pipeBuffer.size() > 1024 * 1024) { nativeScanFailed=true; break; }
            }
            for (;;) {
                bool emptyMark = false, endMark = false, malformed = false;
                if (!ConsumeNextPipeEvent(pipeBuffer, pendingRows, emptyMark, endMark, malformed)) break;
                if (malformed) {
                    nativeScanFailed=true;
                    GlassInstallLog::Line(L"Watch: invalid x64 helper frame");
                    break;
                }
                if (emptyMark) {
                    receivedFrame=true;
                    lastFrame=GetTickCount64();
                    x64Empty = true;
                    x64Tick.clear();
                    pendingRows.clear();
                } else if (endMark) {
                    receivedFrame=true;
                    lastFrame=GetTickCount64();
                    x64Empty = pendingRows.empty();
                    x64Tick = std::move(pendingRows);
                    pendingRows.clear();
                }
            }
        }

        if (nativeScanFailed || helperExitCode!=STILL_ACTIVE) {
            nativeScanFailed=true;
            break; // the Setup helper must keep producing frames until disconnected
        }
        if (scanNativeX64 && NeedNativeX64Helper() && receivedFrame &&
            GetTickCount64()-lastFrame > 15000) {
            GlassInstallLog::Line(L"Watch: x64 helper stopped sending scan frames");
            nativeScanFailed=true;
            break;
        }
        lastHolders.clear();
        if (!CollectInProcessHolders(lastHolders)) {
            nativeScanFailed=true;
            break;
        }
        for (const auto& row : x64Tick)
            MergeHolder(lastHolders, row.pid, row.exe, row.modules);

        if (GetTickCount64() - lastBeat >= 5000) {
            lastBeat = GetTickCount64();
            GlassInstallLog::LineFormat(L"Watch: still held %u pipe=%d",
                static_cast<unsigned>(lastHolders.size()), pipeConnected ? 1 : 0);
        }

        // A new row or a partial marker supersedes the preceding empty frame.
        // Do not proceed until that frame is complete and has been checked.
        if (x64Empty && lastHolders.empty() && pendingRows.empty() && pipeBuffer.empty()) {
            if (pipe != INVALID_HANDLE_VALUE) {
                DisconnectNamedPipe(pipe);
                CloseHandle(pipe);
                pipe = INVALID_HANDLE_VALUE;
            }
            if (watchProcess) {
                if (WaitForSingleObject(watchProcess, 2000) == WAIT_TIMEOUT) {
                    TerminateProcess(watchProcess, 1);
                    WaitForSingleObject(watchProcess, 3000);
                }
                CloseHandle(watchProcess);
                watchProcess = nullptr;
            }
            return true;
        }

        Sleep(300);
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
