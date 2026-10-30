#include "GlassInstallUtil.h"
#include "GlassInstallLog.h"
#include <shlobj.h>
#include <GlassShellMenuConstants.h>
#include <shobjidl.h>
#include <sddl.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <cstring>
#include <mutex>
#include <vector>

namespace GlassInstallUtil {

namespace {

std::mutex g_unlockerMutex;
HINSTANCE g_unlockerInstance = nullptr;
UINT g_unlockerX64ResId = 0;
std::wstring g_extractedUnlockerDir;
std::wstring g_extractedUnlockerX64Path;

// Fail closed: do not overwrite an orphaned legacy shell registration on a clean install.
bool HasExistingShellRegistrationKeys() noexcept {
    using namespace GlassShared::GlassShellMenu;
    std::vector<std::wstring> keys;
    for (const wchar_t* root : {L"DesktopBackground",L"Directory\\Background"}) {
        const std::wstring prefix=std::wstring(L"Software\\Classes\\")+root;
        keys.push_back(prefix+L"\\ShellEx\\ContextMenuHandlers\\"+kHandlerSubkeyName);
        keys.push_back(prefix+L"\\Shell\\"+kDesktopToggleVerbSubkey);
        keys.push_back(prefix+L"\\Shell\\"+kDesktopOpenConfigVerbSubkey);
    }
    for (const wchar_t* clsid : {kClsidStringBraced,kToggleExplorerClsidBraced,kOpenConfigExplorerClsidBraced})
        keys.push_back(std::wstring(L"Software\\Classes\\CLSID\\")+clsid);
    for (const auto& key : keys) {
        HKEY handle=nullptr;
        const LSTATUS status=RegOpenKeyExW(HKEY_CURRENT_USER,key.c_str(),0,KEY_READ,&handle);
        if (handle) RegCloseKey(handle);
        if (status==ERROR_SUCCESS || (status!=ERROR_FILE_NOT_FOUND && status!=ERROR_PATH_NOT_FOUND)) return true;
    }
    return false;
}

bool TryReadRegSz(HKEY root, const wchar_t* subKey, const wchar_t* valueName, REGSAM sam, std::wstring& out) noexcept {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKey, 0, KEY_READ | sam, &key) != ERROR_SUCCESS)
        return false;
    wchar_t buf[MAX_PATH * 2]{};
    DWORD cb = sizeof(buf);
    DWORD type = 0;
    const LSTATUS st = RegQueryValueExW(key, valueName, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &cb);
    RegCloseKey(key);
    if (st != ERROR_SUCCESS || type != REG_SZ || !buf[0])
        return false;
    out = TrimTrailingSlashes(buf);
    return !out.empty();
}

void WriteRegSz(HKEY root, const wchar_t* subKey, const wchar_t* valueName, REGSAM sam, const std::wstring& val) noexcept {
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, subKey, 0, nullptr, 0, KEY_SET_VALUE | sam, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    const DWORD bytes = static_cast<DWORD>((val.size() + 1) * sizeof(wchar_t));
    RegSetValueExW(key, valueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(val.c_str()), bytes);
    RegCloseKey(key);
}

void DeleteRegValueAndEmptyKey(HKEY root, const wchar_t* subKey, const wchar_t* valueName, REGSAM sam) noexcept {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKey, 0, KEY_READ | KEY_SET_VALUE | sam, &key) != ERROR_SUCCESS)
        return;
    if (valueName && *valueName)
        RegDeleteValueW(key, valueName);
    DWORD subKeys = 0;
    DWORD values = 0;
    const LSTATUS q = RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subKeys, nullptr, nullptr,
        &values, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(key);
    if (q == ERROR_SUCCESS && subKeys == 0 && values == 0) {
        using RegDeleteKeyExFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, REGSAM, DWORD);
        static auto pRegDeleteKeyExW = reinterpret_cast<RegDeleteKeyExFn>(
            GetProcAddress(GetModuleHandleW(L"advapi32.dll"), "RegDeleteKeyExW"));
        if (pRegDeleteKeyExW && sam != 0)
            pRegDeleteKeyExW(root, subKey, sam, 0);
        else
            RegDeleteKeyW(root, subKey);
    }
}

std::wstring EnsureExtractedUnlockerX64() noexcept {
    std::lock_guard<std::mutex> lock(g_unlockerMutex);
    if (!g_extractedUnlockerX64Path.empty()
        && PeMatches(g_extractedUnlockerX64Path, IMAGE_FILE_MACHINE_AMD64, "GlassSettings_UnlockerW")) {
        return g_extractedUnlockerX64Path;
    }
    if (!g_unlockerInstance || g_unlockerX64ResId == 0)
        return {};
    std::wstring dir;
    if (!CreateSecureTempDirectory(L"GlassUnlocker",dir)) return {};
    const std::wstring dllPath = dir + L"\\GlassSettings.Native.dll";
    if (!ExtractResourceToFile(g_unlockerInstance, g_unlockerX64ResId, RT_RCDATA, dllPath)) {
        RemoveDirectoryW(dir.c_str());
        return {};
    }
    if (!PeMatches(dllPath, IMAGE_FILE_MACHINE_AMD64, "GlassSettings_UnlockerW")) {
        DeleteFileW(dllPath.c_str());
        RemoveDirectoryW(dir.c_str());
        return {};
    }
    g_extractedUnlockerDir = dir;
    g_extractedUnlockerX64Path = dllPath;
    return g_extractedUnlockerX64Path;
}

} // namespace

void ConfigureEmbeddedUnlocker(HINSTANCE instance, UINT x64ResourceId) noexcept {
    std::lock_guard<std::mutex> lock(g_unlockerMutex);
    g_unlockerInstance = instance;
    g_unlockerX64ResId = x64ResourceId;
}

void CleanupEmbeddedUnlocker() noexcept {
    std::lock_guard<std::mutex> lock(g_unlockerMutex);
    if (!g_extractedUnlockerX64Path.empty()) {
        if (!DeleteFileW(g_extractedUnlockerX64Path.c_str()))
            MoveFileExW(g_extractedUnlockerX64Path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        g_extractedUnlockerX64Path.clear();
    }
    if (!g_extractedUnlockerDir.empty()) {
        if (!RemoveDirectoryW(g_extractedUnlockerDir.c_str()))
            MoveFileExW(g_extractedUnlockerDir.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        g_extractedUnlockerDir.clear();
    }
}

bool CreateSecureTempDirectory(const wchar_t* prefix, std::wstring& out) noexcept {
    out.clear();
    wchar_t windows[MAX_PATH]{};
    if (!GetWindowsDirectoryW(windows,MAX_PATH)) return false;
    PSECURITY_DESCRIPTOR descriptor=nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)",SDDL_REVISION_1,&descriptor,nullptr)) return false;
    SECURITY_ATTRIBUTES attributes{sizeof(attributes),descriptor,FALSE};
    for (int i=0;i<5;++i) {
        GUID guid{};
        if (FAILED(CoCreateGuid(&guid))) break;
        wchar_t suffix[40]{}; StringFromGUID2(guid,suffix,static_cast<int>(std::size(suffix)));
        const std::wstring candidate=std::wstring(windows)+L"\\Temp\\"+(prefix?prefix:L"Glass")+L"-"+suffix;
        if (CreateDirectoryW(candidate.c_str(),&attributes)) { out=candidate; break; }
        if (GetLastError()!=ERROR_ALREADY_EXISTS) break;
    }
    LocalFree(descriptor);
    return !out.empty();
}

bool ExtractResourceToFile(
    HINSTANCE instance,
    UINT resourceId,
    const wchar_t* resourceType,
    const std::wstring& destPath,
    const std::atomic_bool* cancelled) noexcept {
    if (!instance || resourceId == 0 || destPath.empty())
        return false;
    HRSRC res = FindResourceW(instance, MAKEINTRESOURCEW(resourceId), resourceType);
    if (!res)
        return false;
    const DWORD size = SizeofResource(instance, res);
    if (size == 0)
        return false;
    HGLOBAL loaded = LoadResource(instance, res);
    if (!loaded)
        return false;
    const void* bytes = LockResource(loaded);
    if (!bytes)
        return false;
    HANDLE file = CreateFileW(destPath.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD offset=0;
    bool ok=true;
    const auto* data=static_cast<const BYTE*>(bytes);
    while (offset<size) {
        if (cancelled && cancelled->load(std::memory_order_acquire)) { ok=false; break; }
        const DWORD chunk=(size-offset)>1024*1024 ? 1024*1024 : size-offset;
        DWORD written=0;
        if (!WriteFile(file,data+offset,chunk,&written,nullptr) || written==0) { ok=false; break; }
        offset+=written;
    }
    CloseHandle(file);
    if (!ok || offset!=size) { DeleteFileW(destPath.c_str()); return false; }
    return true;
}

bool IsOs64Bit() noexcept {
#ifdef _WIN64
    return true;
#else
    BOOL wow = FALSE;
    return IsWow64Process(GetCurrentProcess(), &wow) && wow != FALSE;
#endif
}

std::wstring TrimTrailingSlashes(std::wstring path) {
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/'))
        path.pop_back();
    return path;
}

bool RunHiddenCommand(const std::wstring& commandLine, DWORD* exitCode, DWORD timeoutMs,
    bool* started, bool* completed) noexcept {
    if (started) *started=false;
    if (completed) *completed=false;
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
    if (started) *started=true;
    CloseHandle(pi.hThread);
    const DWORD wait = WaitForSingleObject(pi.hProcess, timeoutMs);
    if (wait != WAIT_OBJECT_0) {
        if (wait == WAIT_TIMEOUT) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 2000);
        }
        CloseHandle(pi.hProcess);
        if (exitCode) *exitCode = (wait == WAIT_TIMEOUT) ? static_cast<DWORD>(WAIT_TIMEOUT) : GetLastError();
        return false;
    }
    DWORD code = 1;
    const BOOL gotCode = GetExitCodeProcess(pi.hProcess, &code);
    if (completed) *completed=gotCode!=FALSE;
    CloseHandle(pi.hProcess);
    if (exitCode) *exitCode = gotCode ? code : 1u;
    return gotCode != FALSE;
}

bool RunHidden(const wchar_t* file, const std::wstring& args) noexcept {
    const std::wstring command = std::wstring(L"\"") + file + L"\" " + args;
    DWORD code = 1;
    return RunHiddenCommand(command, &code, 20000) && code == 0;
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

BOOL CALLBACK CloseWindowsForPid(HWND hwnd, LPARAM param) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == static_cast<DWORD>(param)) PostMessageW(hwnd, WM_CLOSE, 0, 0);
    return TRUE;
}

void CloseGlassSettingsProcesses() noexcept {
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snap, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"GlassSettings.exe") != 0) continue;
            EnumWindows(CloseWindowsForPid, static_cast<LPARAM>(entry.th32ProcessID));
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    const ULONGLONG start = GetTickCount64();
    while (ImageRunning(L"GlassSettings.exe") && GetTickCount64() - start < 8000)
        Sleep(200);
}

std::wstring NativeRundll64() {
#ifdef _WIN64
    wchar_t sys[MAX_PATH]{};
    if (!GetSystemDirectoryW(sys, MAX_PATH)) return {};
    const std::wstring path = std::wstring(sys) + L"\\rundll32.exe";
    return PathFileExistsW(path.c_str()) ? path : std::wstring{};
#else
    wchar_t windows[MAX_PATH]{};
    if (!GetWindowsDirectoryW(windows, MAX_PATH)) return {};
    const std::wstring path = std::wstring(windows) + L"\\Sysnative\\rundll32.exe";
    return PathFileExistsW(path.c_str()) ? path : std::wstring{};
#endif
}

std::wstring System64ExePath(const wchar_t* exeName) {
    if (!exeName || !*exeName) return {};
#ifndef _WIN64
    if (IsOs64Bit()) {
        wchar_t winDir[MAX_PATH]{};
        if (GetWindowsDirectoryW(winDir, MAX_PATH)) {
            const std::wstring sysnative = std::wstring(winDir) + L"\\Sysnative\\" + exeName;
            if (PathFileExistsW(sysnative.c_str()))
                return sysnative;
        }
    }
#endif
    wchar_t sysDir[MAX_PATH]{};
    if (!GetSystemDirectoryW(sysDir, MAX_PATH))
        return exeName;
    return std::wstring(sysDir) + L"\\" + exeName;
}

std::wstring RundllPathForTaskArch(GlassShared::GlassPlatformTask::PlatformArch arch) {
    wchar_t winDir[MAX_PATH]{};
    if (!GetWindowsDirectoryW(winDir, MAX_PATH))
        return L"rundll32.exe";
    if (arch == GlassShared::GlassPlatformTask::PlatformArch::X86 && IsOs64Bit()) {
        wchar_t wowDir[MAX_PATH]{};
        if (GetSystemWow64DirectoryW(wowDir, MAX_PATH))
            return std::wstring(wowDir) + L"\\rundll32.exe";
        return std::wstring(winDir) + L"\\SysWOW64\\rundll32.exe";
    }
    return std::wstring(winDir) + L"\\System32\\rundll32.exe";
}

bool PeMatches(const std::wstring& path, WORD machine, const char* exportName) noexcept {
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (file==INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file,&size) || size.QuadPart<512 || size.QuadPart>64ll*1024*1024) { CloseHandle(file); return false; }
    const size_t length=static_cast<size_t>(size.QuadPart);
    HANDLE map=CreateFileMappingW(file,nullptr,PAGE_READONLY,0,0,nullptr); CloseHandle(file);
    if (!map) return false;
    const auto* base=static_cast<const BYTE*>(MapViewOfFile(map,FILE_MAP_READ,0,0,0)); CloseHandle(map);
    if (!base) return false;
    auto bounds=[&](size_t offset,size_t bytes) { return offset<=length && bytes<=length-offset; };
    bool ok=false;
    if (bounds(0,sizeof(IMAGE_DOS_HEADER))) {
        const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const size_t nt=dos->e_lfanew>0 ? static_cast<size_t>(dos->e_lfanew) : length;
        if (dos->e_magic==IMAGE_DOS_SIGNATURE && bounds(nt,sizeof(DWORD)+sizeof(IMAGE_FILE_HEADER))) {
            DWORD signature=0; std::memcpy(&signature,base+nt,sizeof(signature));
            const auto* header=reinterpret_cast<const IMAGE_FILE_HEADER*>(base+nt+sizeof(DWORD));
            const size_t optional=nt+sizeof(DWORD)+sizeof(IMAGE_FILE_HEADER);
            const size_t sections=optional+header->SizeOfOptionalHeader;
            const size_t count=header->NumberOfSections;
            if (signature==IMAGE_NT_SIGNATURE && header->Machine==machine && count<=96 &&
                bounds(optional,header->SizeOfOptionalHeader) && bounds(sections,count*sizeof(IMAGE_SECTION_HEADER))) {
                if (!exportName || !*exportName) ok=true;
                else {
                    WORD magic=0;
                    if (header->SizeOfOptionalHeader>=sizeof(magic)) std::memcpy(&magic,base+optional,sizeof(magic));
                    const size_t dataDirectoryOffset=magic==IMAGE_NT_OPTIONAL_HDR64_MAGIC ?
                        offsetof(IMAGE_OPTIONAL_HEADER64,DataDirectory) : offsetof(IMAGE_OPTIONAL_HEADER32,DataDirectory);
                    if ((magic==IMAGE_NT_OPTIONAL_HDR64_MAGIC || magic==IMAGE_NT_OPTIONAL_HDR32_MAGIC) &&
                        dataDirectoryOffset+sizeof(IMAGE_DATA_DIRECTORY)<=header->SizeOfOptionalHeader) {
                        IMAGE_DATA_DIRECTORY exports{};
                        std::memcpy(&exports,base+optional+dataDirectoryOffset,sizeof(exports));
                        const auto* section=reinterpret_cast<const IMAGE_SECTION_HEADER*>(base+sections);
                        auto rvaToOffset=[&](DWORD rva,size_t bytes,size_t& offset) {
                            for (size_t n=0;n<count;++n) {
                                const uint64_t va=section[n].VirtualAddress, raw=section[n].SizeOfRawData;
                                if (rva<va || static_cast<uint64_t>(rva)-va>raw) continue;
                                const uint64_t delta=static_cast<uint64_t>(rva)-va;
                                if (bytes>raw-delta) continue;
                                const uint64_t candidate=static_cast<uint64_t>(section[n].PointerToRawData)+delta;
                                if (candidate>length || !bounds(static_cast<size_t>(candidate),bytes)) continue;
                                offset=static_cast<size_t>(candidate); return true;
                            }
                            return false;
                        };
                        size_t exportOffset=0;
                        if (exports.VirtualAddress && rvaToOffset(exports.VirtualAddress,sizeof(IMAGE_EXPORT_DIRECTORY),exportOffset)) {
                            const auto* exp=reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base+exportOffset);
                            const DWORD nameCount=exp->NumberOfNames;
                            size_t namesOffset=0;
                            if (nameCount<=8192 && rvaToOffset(exp->AddressOfNames,static_cast<size_t>(nameCount)*sizeof(DWORD),namesOffset)) {
                                for (DWORD n=0;n<nameCount;++n) {
                                    DWORD nameRva=0; std::memcpy(&nameRva,base+namesOffset+n*sizeof(DWORD),sizeof(nameRva));
                                    size_t nameOffset=0;
                                    const size_t wanted=std::strlen(exportName);
                                    if (rvaToOffset(nameRva,wanted+1,nameOffset) &&
                                        std::memcmp(base+nameOffset,exportName,wanted)==0 && base[nameOffset+wanted]==0) {
                                        ok=true; break;
                                    }
                                }
                            }
                        }
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
    if (x64) {
        // Elevated setup/uninstall never load a helper from a user-writable registry path.
        // Release builds embed it; missing/corrupt resources fail closed, including Debug.
        if (!g_unlockerInstance || !g_unlockerX64ResId ||
            !FindResourceW(g_unlockerInstance,MAKEINTRESOURCEW(g_unlockerX64ResId),RT_RCDATA)) return {};
        return EnsureExtractedUnlockerX64();
    }
    const std::wstring root = x86Dir;
    if (!root.empty()) {
        const std::wstring dll = TrimTrailingSlashes(root) + L"\\GlassSettings\\GlassSettings.Native.dll";
        if (PeMatches(dll, machine, "GlassSettings_UnlockerW"))
            return dll;
    }
    std::wstring regRoot;
    if (QueryPlatformInstallPath(x64, regRoot) && !regRoot.empty()) {
        const std::wstring dll = regRoot + L"\\GlassSettings\\GlassSettings.Native.dll";
        if (PeMatches(dll, machine, "GlassSettings_UnlockerW"))
            return dll;
    }
    std::wstring fromTask = GlassShared::GlassPlatformTask::TryGetAutorunGlassCompatInstallRoot(arch);
    if (!fromTask.empty()) {
        const std::wstring dll = TrimTrailingSlashes(fromTask) + L"\\GlassSettings\\GlassSettings.Native.dll";
        if (PeMatches(dll, machine, "GlassSettings_UnlockerW"))
            return dll;
    }
    return {};
}

bool QueryPlatformInstallPath(bool x64, std::wstring& out) noexcept {
    out.clear();
    if (x64 && !IsOs64Bit())
        return false;

    const wchar_t* explicitVal = x64 ? L"InstallPathX64" : L"InstallPathX86";
    // Prefer the machine-owned value when an unprivileged user can edit HKCU.
    const REGSAM lmSam = x64 ? KEY_WOW64_64KEY : KEY_WOW64_32KEY;
    if (TryReadRegSz(HKEY_LOCAL_MACHINE, L"Software\\GlassPlatform", explicitVal, lmSam, out))
        return true;
    if (TryReadRegSz(HKEY_LOCAL_MACHINE, L"Software\\GlassPlatform", L"InstallPath", lmSam, out))
        return true;
    if (TryReadRegSz(HKEY_CURRENT_USER, L"Software\\GlassPlatform", explicitVal, 0, out))
        return true;

    if (!x64 && IsOs64Bit()) {
        return TryReadRegSz(HKEY_CURRENT_USER, L"Software\\Wow6432Node\\GlassPlatform", L"InstallPath", 0, out);
    }

    if (TryReadRegSz(HKEY_CURRENT_USER, L"Software\\GlassPlatform", L"InstallPath", 0, out)) {
        if (x64 && IsOs64Bit()) {
            // HKCU\Software is shared: an old x86 MSI could have overwritten this value,
            // including for a custom directory outside Program Files (x86).
            const std::wstring compat=out+L"\\GlassCompat.dll";
            if (!PeMatches(compat,IMAGE_FILE_MACHINE_AMD64,nullptr)) { out.clear(); return false; }
        }
        return true;
    }
    return false;
}

void WritePlatformInstallPath(bool x64, const std::wstring& installDir) noexcept {
    const std::wstring dir = TrimTrailingSlashes(installDir);
    if (dir.empty()) return;
    const wchar_t* explicitVal = x64 ? L"InstallPathX64" : L"InstallPathX86";
    const REGSAM lmSam = x64 ? KEY_WOW64_64KEY : KEY_WOW64_32KEY;
    WriteRegSz(HKEY_CURRENT_USER, L"Software\\GlassPlatform", explicitVal, 0, dir);
    WriteRegSz(HKEY_LOCAL_MACHINE, L"Software\\GlassPlatform", explicitVal, lmSam, dir);
    WriteRegSz(HKEY_LOCAL_MACHINE, L"Software\\GlassPlatform", L"InstallPath", lmSam, dir);
    if (x64 == IsOs64Bit()) {
        WriteRegSz(HKEY_CURRENT_USER, L"Software\\GlassPlatform", L"InstallPath", 0, dir);
    } else if (!x64 && IsOs64Bit()) {
        WriteRegSz(HKEY_CURRENT_USER, L"Software\\Wow6432Node\\GlassPlatform", L"InstallPath", 0, dir);
    }
}

void ClearPlatformInstallPath(bool x64) noexcept {
    const wchar_t* explicitVal = x64 ? L"InstallPathX64" : L"InstallPathX86";
    const REGSAM lmSam = x64 ? KEY_WOW64_64KEY : KEY_WOW64_32KEY;
    DeleteRegValueAndEmptyKey(HKEY_CURRENT_USER, L"Software\\GlassPlatform", explicitVal, 0);
    DeleteRegValueAndEmptyKey(HKEY_LOCAL_MACHINE, L"Software\\GlassPlatform", explicitVal, lmSam);
    DeleteRegValueAndEmptyKey(HKEY_LOCAL_MACHINE, L"Software\\GlassPlatform", L"InstallPath", lmSam);
    if (x64 == IsOs64Bit()) {
        DeleteRegValueAndEmptyKey(HKEY_CURRENT_USER, L"Software\\GlassPlatform", L"InstallPath", 0);
    } else if (!x64 && IsOs64Bit()) {
        DeleteRegValueAndEmptyKey(HKEY_CURRENT_USER, L"Software\\Wow6432Node\\GlassPlatform", L"InstallPath", 0);
    }
}

bool QueryOpenGlassInstallPath(std::wstring& out) noexcept {
    out.clear();
    if (TryReadRegSz(HKEY_LOCAL_MACHINE,L"Software\\StanislavKa\\GlassPlatform",L"OpenGlassInstallPath",KEY_WOW64_64KEY,out)) return true;
    if (TryReadRegSz(HKEY_CURRENT_USER,L"Software\\StanislavKa\\GlassPlatform",L"OpenGlassInstallPath",0,out)) return true;
    // Previous MSI versions wrote the actual OpenGlass directory here.
    return TryReadRegSz(HKEY_CURRENT_USER,L"Software\\StanislavKa\\OpenGlass",L"InstallPath",0,out);
}

void WriteOpenGlassInstallPath(const std::wstring& x64RootDir, const std::wstring& openGlassDir) noexcept {
    const std::wstring root = TrimTrailingSlashes(x64RootDir);
    const std::wstring og = TrimTrailingSlashes(openGlassDir);
    if (!root.empty()) {
        WriteRegSz(HKEY_CURRENT_USER, L"Software\\StanislavKa\\GlassPlatform", L"InstallPath", 0, root);
        WriteRegSz(HKEY_LOCAL_MACHINE, L"Software\\StanislavKa\\GlassPlatform", L"InstallPath", KEY_WOW64_64KEY, root);
    }
    if (!og.empty()) {
        WriteRegSz(HKEY_CURRENT_USER, L"Software\\StanislavKa\\GlassPlatform", L"OpenGlassInstallPath", 0, og);
        WriteRegSz(HKEY_LOCAL_MACHINE, L"Software\\StanislavKa\\GlassPlatform", L"OpenGlassInstallPath", KEY_WOW64_64KEY, og);
    }
}

void ClearOpenGlassInstallPath() noexcept {
    DeleteRegValueAndEmptyKey(HKEY_CURRENT_USER, L"Software\\StanislavKa\\GlassPlatform", L"OpenGlassInstallPath", 0);
    DeleteRegValueAndEmptyKey(HKEY_LOCAL_MACHINE, L"Software\\StanislavKa\\GlassPlatform", L"OpenGlassInstallPath", KEY_WOW64_64KEY);
    DeleteRegValueAndEmptyKey(HKEY_CURRENT_USER, L"Software\\StanislavKa\\OpenGlass", L"InstallPath", 0);
}

bool StartMenuShortcutExists() noexcept {
    PWSTR programs=nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Programs,0,nullptr,&programs)) || !programs) return false;
    const std::wstring link=std::wstring(programs)+L"\\GlassPlatform\\GlassSettings.lnk";
    CoTaskMemFree(programs);
    return GetFileAttributesW(link.c_str())!=INVALID_FILE_ATTRIBUTES;
}

bool CreateStartMenuShortcut(const std::wstring& targetExe, const std::wstring& workingDir) noexcept {
    if (targetExe.empty() || !PathFileExistsW(targetExe.c_str()))
        return false;
    PWSTR programs = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Programs, KF_FLAG_CREATE, nullptr, &programs)) || !programs)
        return false;
    const std::wstring folder = std::wstring(programs) + L"\\GlassPlatform";
    CoTaskMemFree(programs);
    if (!CreateDirectoryW(folder.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        return false;
    const std::wstring lnkPath = folder + L"\\GlassSettings.lnk";
    if (GetFileAttributesW(lnkPath.c_str())!=INVALID_FILE_ATTRIBUTES) return true;

    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool needUninit = (coHr == S_OK || coHr == S_FALSE);

    IShellLinkW* link = nullptr;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) && link) {
        link->SetPath(targetExe.c_str());
        if (!workingDir.empty())
            link->SetWorkingDirectory(workingDir.c_str());
        link->SetIconLocation(targetExe.c_str(), 0);
        IPersistFile* persist = nullptr;
        if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&persist))) && persist) {
            ok = SUCCEEDED(persist->Save(lnkPath.c_str(), TRUE));
            persist->Release();
        }
        link->Release();
    }

    if (needUninit)
        CoUninitialize();
    return ok;
}

void DeleteStartMenuShortcut() noexcept {
    auto removeIn = [](REFKNOWNFOLDERID id) {
        PWSTR base = nullptr;
        if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &base)) || !base) return;
        const std::wstring folder = std::wstring(base) + L"\\GlassPlatform";
        CoTaskMemFree(base);
        DeleteFileW((folder + L"\\GlassSettings.lnk").c_str());
        RemoveDirectoryW(folder.c_str());
    };
    removeIn(FOLDERID_Programs);
}

bool InvokeShellExtRegistration(
    bool registerExtension,
    const std::wstring& x64Dir,
    const std::wstring& x86Dir,
    bool* registrationMayHaveChanged) noexcept {
    if (registrationMayHaveChanged) *registrationMayHaveChanged=false;
    const bool os64 = IsOs64Bit();
    const auto arch = os64
        ? GlassShared::GlassPlatformTask::PlatformArch::X64
        : GlassShared::GlassPlatformTask::PlatformArch::X86;
    const std::wstring dll = NativeDllForArch(arch, x64Dir, x86Dir);
    const WORD machine=os64?IMAGE_FILE_MACHINE_AMD64:IMAGE_FILE_MACHINE_I386;
    if (dll.empty() || !PeMatches(dll,machine,"GlassSettings_ShellExtRegisterW")) {
        // Windows 11 requires the sparse package; do not silently substitute legacy regsvr32.
        // On 32-bit Windows 10, legacy registration is the supported fallback.
        if (os64) return false;
        const std::wstring root = x86Dir;
        if (root.empty()) return false;
        const std::wstring shellExt = TrimTrailingSlashes(root) + L"\\GlassShellExt.dll";
        if (!PathFileExistsW(shellExt.c_str())) return false;
        if (registerExtension && HasExistingShellRegistrationKeys()) return true;
        const std::wstring regsvr = System64ExePath(L"regsvr32.exe");
        std::wstring args = registerExtension ? L"/s \"" : L"/u /s \"";
        args += shellExt + L"\"";
        const std::wstring command=L"\""+regsvr+L"\" "+args;
        bool started=false;
        DWORD exitCode=1;
        const bool result=RunHiddenCommand(command,&exitCode,45000,&started) && exitCode==0;
        if (registrationMayHaveChanged) *registrationMayHaveChanged=started;
        return result;
    }
    const std::wstring rundll = os64 ? NativeRundll64() : RundllPathForTaskArch(arch);
    if (rundll.empty()) return false;
    std::wstring cmd = L"\"" + rundll + L"\" \"" + dll + L"\",GlassSettings_ShellExtRegisterW ";
    cmd += registerExtension ? L"/register /setup" : L"/unregister";
    DWORD exitCode = 1;
    bool started=false,completed=false;
    const bool ran = RunHiddenCommand(cmd, &exitCode, 45000, &started, &completed);
    constexpr DWORD kShellAlreadyRegistered=0x47505245; // Explicit sentinel, not a generic rundll32 error.
    const bool alreadyRegistered=registerExtension && completed && exitCode==kShellAlreadyRegistered;
    if (registrationMayHaveChanged) *registrationMayHaveChanged=started && !alreadyRegistered;
    GlassInstallLog::LineFormat(L"ShellExt %s via Native.dll: ran=%d exit=%lu existing=%d",
        registerExtension ? L"register" : L"unregister", ran ? 1 : 0, exitCode, alreadyRegistered?1:0);
    return alreadyRegistered || (ran && exitCode == 0);
}

} // namespace GlassInstallUtil
