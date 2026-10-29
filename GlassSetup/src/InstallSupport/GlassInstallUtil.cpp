#include "GlassInstallUtil.h"
#include "GlassInstallLog.h"
#include <shlwapi.h>
#include <tlhelp32.h>
#include <vector>

namespace GlassInstallUtil {

bool RunHiddenCommand(const std::wstring& commandLine, DWORD* exitCode, DWORD timeoutMs) noexcept {
    std::vector<wchar_t> cmdBuf(commandLine.begin(), commandLine.end());
    cmdBuf.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_FORCEOFFFEEDBACK;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, nullptr, &si, &pi)) {
        if (exitCode) *exitCode = GetLastError();
        return false;
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, timeoutMs);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (exitCode) *exitCode = code;
    return true;
}

bool RunHidden(const wchar_t* file, const std::wstring& args) noexcept {
    const std::wstring command = std::wstring(L"\"") + file + L"\" " + args;
    DWORD code = 1;
    return RunHiddenCommand(command, &code, 20000);
}

bool ImageRunning(const wchar_t* imageName) noexcept {
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snap, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, imageName) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return found;
}

void CloseGlassSettingsProcesses() noexcept {
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snap, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"GlassSettings.exe") != 0) continue;
            EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
                DWORD pid = 0;
                GetWindowThreadProcessId(hwnd, &pid);
                if (pid == static_cast<DWORD>(param))
                    PostMessageW(hwnd, WM_CLOSE, 0, 0);
                return TRUE;
            }, static_cast<LPARAM>(entry.th32ProcessID));
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    const DWORD start = GetTickCount();
    while (ImageRunning(L"GlassSettings.exe") && GetTickCount() - start < 8000)
        Sleep(200);
}

std::wstring NativeRundll64() {
    wchar_t windows[MAX_PATH]{};
    if (!GetWindowsDirectoryW(windows, MAX_PATH)) return {};
    const std::wstring path = std::wstring(windows) + L"\\Sysnative\\rundll32.exe";
    return PathFileExistsW(path.c_str()) ? path : std::wstring{};
}

bool PeMatches(const std::wstring& path, WORD machine, const char* exportName) noexcept {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 512 || size.QuadPart > 64ll * 1024 * 1024) {
        CloseHandle(file);
        return false;
    }
    const auto fileSize = static_cast<size_t>(size.QuadPart);
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CloseHandle(file);
    if (!mapping) return false;
    const auto* base = static_cast<const BYTE*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    CloseHandle(mapping);
    if (!base) return false;
    bool ok = false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic == IMAGE_DOS_SIGNATURE && dos->e_lfanew > 0
        && static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) < fileSize) {
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.Machine == machine) {
            if (!exportName || !exportName[0]) {
                ok = true;
            } else {
                DWORD exportRva = 0;
                const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
                const WORD sectionCount = nt->FileHeader.NumberOfSections;
                if (machine == IMAGE_FILE_MACHINE_AMD64) {
                    exportRva = reinterpret_cast<const IMAGE_NT_HEADERS64*>(nt)->OptionalHeader
                        .DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
                } else {
                    exportRva = reinterpret_cast<const IMAGE_NT_HEADERS32*>(nt)->OptionalHeader
                        .DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
                }
                auto rvaToPtr = [&](DWORD rva) -> const char* {
                    for (WORD i = 0; i < sectionCount; ++i) {
                        const DWORD va = section[i].VirtualAddress;
                        const DWORD raw = section[i].SizeOfRawData;
                        if (rva < va || rva >= va + raw) continue;
                        const size_t off = static_cast<size_t>(section[i].PointerToRawData) + (rva - va);
                        if (off >= fileSize) return nullptr;
                        return reinterpret_cast<const char*>(base + off);
                    }
                    return nullptr;
                };
                const auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(
                    rvaToPtr(exportRva));
                const auto* names = exp ? reinterpret_cast<const DWORD*>(rvaToPtr(exp->AddressOfNames)) : nullptr;
                const DWORD nameCount = exp && exp->NumberOfNames < 8192 ? exp->NumberOfNames : 0;
                for (DWORD i = 0; i < nameCount; ++i) {
                    const char* item = rvaToPtr(names[i]);
                    if (item && strcmp(item, exportName) == 0) {
                        ok = true;
                        break;
                    }
                }
            }
        }
    }
    UnmapViewOfFile(base);
    return ok;
}

std::wstring NativeDllForArch(
    GlassShared::GlassPlatformTask::PlatformArch arch,
    const std::wstring& x64Dir,
    const std::wstring& x86Dir) noexcept {
    const bool x64 = arch == GlassShared::GlassPlatformTask::PlatformArch::X64;
    const WORD machine = x64 ? IMAGE_FILE_MACHINE_AMD64 : IMAGE_FILE_MACHINE_I386;
    const std::wstring root = x64 ? x64Dir : x86Dir;
    if (!root.empty()) {
        const std::wstring dll = root + L"\\GlassSettings\\GlassSettings.Native.dll";
        if (PeMatches(dll, machine, "GlassSettings_UnlockerW"))
            return dll;
    }
    std::wstring fromTask = GlassShared::GlassPlatformTask::TryGetAutorunGlassCompatInstallRoot(arch);
    if (!fromTask.empty()) {
        const std::wstring dll = fromTask + L"\\GlassSettings\\GlassSettings.Native.dll";
        if (PeMatches(dll, machine, "GlassSettings_UnlockerW"))
            return dll;
    }
    return {};
}

} // namespace GlassInstallUtil
