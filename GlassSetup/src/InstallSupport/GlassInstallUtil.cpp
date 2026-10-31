#include "GlassInstallUtil.h"
#include "GlassInstallLog.h"
#include "GlassEmptyDirectories.h"
#include <shlobj.h>
#include <GlassShellMenuConstants.h>
#include <objbase.h>
#include <oleauto.h>
#include <taskschd.h>
#include <wrl/client.h>
#include <shobjidl.h>
#include <sddl.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <mutex>
#include <vector>

namespace GlassInstallUtil {

namespace {

std::mutex g_unlockerMutex;
HINSTANCE g_unlockerInstance = nullptr;
UINT g_unlockerX64ResId = 0;
UINT g_unlockerX86ResId = 0;
std::wstring g_extractedUnlockerDir;
std::wstring g_extractedUnlockerX64Path;
std::wstring g_extractedUnlockerX86Dir;
std::wstring g_extractedUnlockerX86Path;

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

bool HasMatchingLegacyShellRegistration(const std::wstring& root) noexcept {
    using namespace GlassShared::GlassShellMenu;
    bool handler=false;
    for (const wchar_t* shellRoot : {L"DesktopBackground",L"Directory\\Background"}) {
        const std::wstring key=std::wstring(L"Software\\Classes\\")+shellRoot+
            L"\\ShellEx\\ContextMenuHandlers\\"+kHandlerSubkeyName;
        wchar_t clsid[128]{};DWORD bytes=sizeof(clsid);
        if (RegGetValueW(HKEY_CURRENT_USER,key.c_str(),nullptr,RRF_RT_REG_SZ,
                nullptr,clsid,&bytes)==ERROR_SUCCESS && _wcsicmp(clsid,kClsidStringBraced)==0)
            handler=true;
    }
    if (!handler) return false;
    const std::wstring key=std::wstring(L"Software\\Classes\\CLSID\\")+kClsidStringBraced+L"\\InprocServer32";
    wchar_t value[32768]{};DWORD bytes=sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER,key.c_str(),nullptr,RRF_RT_REG_SZ,nullptr,value,&bytes)==ERROR_SUCCESS &&
        SameInstallPath(value,TrimTrailingSlashes(root)+L"\\GlassShellExt.dll");
}

enum class RegistryPathRead { Missing, Found, Error };

RegistryPathRead ReadInstallPathValue(HKEY hive, const wchar_t* subKey,
    const wchar_t* valueName, REGSAM view, std::wstring& out) {
    HKEY key=nullptr;
    LSTATUS status=RegOpenKeyExW(hive,subKey,0,KEY_QUERY_VALUE|view,&key);
    if (status==ERROR_FILE_NOT_FOUND || status==ERROR_PATH_NOT_FOUND) return RegistryPathRead::Missing;
    if (status!=ERROR_SUCCESS) { SetLastError(status); return RegistryPathRead::Error; }
    wchar_t buffer[32768]{};
    DWORD bytes=sizeof(buffer),type=0;
    status=RegQueryValueExW(key,valueName,nullptr,&type,reinterpret_cast<LPBYTE>(buffer),&bytes);
    RegCloseKey(key);
    if (status==ERROR_FILE_NOT_FOUND || status==ERROR_PATH_NOT_FOUND) return RegistryPathRead::Missing;
    if (status!=ERROR_SUCCESS) { SetLastError(status); return RegistryPathRead::Error; }
    if (type!=REG_SZ || bytes<sizeof(wchar_t) || bytes>sizeof(buffer) || bytes%sizeof(wchar_t)!=0) {
        SetLastError(ERROR_INVALID_DATA); return RegistryPathRead::Error;
    }
    const size_t count=bytes/sizeof(wchar_t);
    if (buffer[count-1]!=L'\0' || !buffer[0]) { SetLastError(ERROR_INVALID_DATA); return RegistryPathRead::Error; }
    const size_t length=std::wcslen(buffer);
    for (size_t i=length+1;i<count;++i) if (buffer[i]!=L'\0') {
        SetLastError(ERROR_INVALID_DATA); return RegistryPathRead::Error;
    }
    out=TrimTrailingSlashes(std::wstring(buffer,length));
    if (out.empty()) { SetLastError(ERROR_INVALID_DATA); return RegistryPathRead::Error; }
    return RegistryPathRead::Found;
}

// Installation registers missing values; it does not reconcile existing
// copies, including values of a type that this resolver cannot use. MSI owns
// its own registry component, handled separately by the rollback snapshot.
// This Win32 sequence is not atomic against another registry writer.
bool WriteRegSzIfMissing(HKEY root, const wchar_t* subKey, const wchar_t* valueName,
    REGSAM sam, const std::wstring& val) noexcept {
    HKEY key=nullptr;
    LSTATUS status=RegOpenKeyExW(root,subKey,0,KEY_QUERY_VALUE|sam,&key);
    if (status==ERROR_SUCCESS) {
        DWORD bytes=0;
        status=RegQueryValueExW(key,valueName,nullptr,nullptr,nullptr,&bytes);
        RegCloseKey(key); key=nullptr;
        if (status==ERROR_SUCCESS) return true;
        if (status!=ERROR_FILE_NOT_FOUND) { SetLastError(status); return false; }
    } else if (status!=ERROR_FILE_NOT_FOUND && status!=ERROR_PATH_NOT_FOUND) {
        SetLastError(status); return false;
    }

    status=RegCreateKeyExW(root,subKey,0,nullptr,0,KEY_QUERY_VALUE|KEY_SET_VALUE|sam,nullptr,&key,nullptr);
    if (status!=ERROR_SUCCESS) { SetLastError(status); return false; }
    DWORD existingBytes=0;
    status=RegQueryValueExW(key,valueName,nullptr,nullptr,nullptr,&existingBytes);
    if (status==ERROR_FILE_NOT_FOUND) {
        const DWORD bytes=static_cast<DWORD>((val.size()+1)*sizeof(wchar_t));
        status=RegSetValueExW(key,valueName,0,REG_SZ,reinterpret_cast<const BYTE*>(val.c_str()),bytes);
    }
    RegCloseKey(key);
    if (status!=ERROR_SUCCESS) { SetLastError(status); return false; }
    return true;
}

bool DeleteRegistryValue(HKEY root, const wchar_t* subKey, const wchar_t* valueName, REGSAM sam) noexcept {
    HKEY key = nullptr;
    const LSTATUS opened = RegOpenKeyExW(root, subKey, 0, KEY_READ | KEY_SET_VALUE | sam, &key);
    if (opened == ERROR_FILE_NOT_FOUND || opened == ERROR_PATH_NOT_FOUND) return true;
    if (opened != ERROR_SUCCESS) { SetLastError(opened); return false; }
    if (valueName && *valueName) {
        const LSTATUS deleted = RegDeleteValueW(key, valueName);
        if (deleted != ERROR_SUCCESS && deleted != ERROR_FILE_NOT_FOUND) {
            RegCloseKey(key); SetLastError(deleted); return false;
        }
    }
    // Preserve the key and its security descriptor, even if it is now empty.
    // A rollback can then restore the value without recreating a customised key.
    RegCloseKey(key);
    return true;
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

void ConfigureEmbeddedUnlocker(HINSTANCE instance, UINT x64ResourceId, UINT x86ResourceId) noexcept {
    std::lock_guard<std::mutex> lock(g_unlockerMutex);
    g_unlockerInstance = instance;
    g_unlockerX64ResId = x64ResourceId;
    g_unlockerX86ResId = x86ResourceId;
}

std::wstring EmbeddedNativeDllX86() noexcept {
    std::lock_guard<std::mutex> lock(g_unlockerMutex);
    if (!g_extractedUnlockerX86Path.empty() &&
        PeMatches(g_extractedUnlockerX86Path, IMAGE_FILE_MACHINE_I386, "GlassSettings_UnlockerW"))
        return g_extractedUnlockerX86Path;
    if (!g_unlockerInstance || !g_unlockerX86ResId) return {};
    std::wstring dir;
    if (!CreateSecureTempDirectory(L"GlassUnlocker86", dir)) return {};
    // Use a distinct temporary basename so a previously loaded Settings DLL
    // cannot be mistaken for this installer's bundled x86 Native module.
    const std::wstring path = dir + L"\\GlassSetup.Native.x86.dll";
    if (!ExtractResourceToFile(g_unlockerInstance, g_unlockerX86ResId, RT_RCDATA, path) ||
        !PeMatches(path, IMAGE_FILE_MACHINE_I386, "GlassSettings_UnlockerW")) {
        DeleteFileW(path.c_str());
        RemoveDirectoryW(dir.c_str());
        return {};
    }
    g_extractedUnlockerX86Dir = dir;
    g_extractedUnlockerX86Path = path;
    return path;
}

void CleanupEmbeddedUnlocker() noexcept {
    std::lock_guard<std::mutex> lock(g_unlockerMutex);
    // The in-process x86 module must have been FreeLibrary'd first.
    if (!g_extractedUnlockerX86Path.empty()) {
        if (!DeleteFileW(g_extractedUnlockerX86Path.c_str())) {
            const DWORD error = GetLastError();
            const bool deferred = MoveFileExW(g_extractedUnlockerX86Path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT) != FALSE;
            GlassInstallLog::LineFormat(L"x86 Native cleanup failed (%lu), deferred=%d", error, deferred ? 1 : 0);
        }
        g_extractedUnlockerX86Path.clear();
    }
    if (!g_extractedUnlockerX86Dir.empty()) {
        if (!RemoveDirectoryW(g_extractedUnlockerX86Dir.c_str())) {
            const DWORD error = GetLastError();
            const bool deferred = MoveFileExW(g_extractedUnlockerX86Dir.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT) != FALSE;
            GlassInstallLog::LineFormat(L"x86 Native temp directory cleanup failed (%lu), deferred=%d", error, deferred ? 1 : 0);
        }
        g_extractedUnlockerX86Dir.clear();
    }
    if (!g_extractedUnlockerX64Path.empty()) {
        if (!DeleteFileW(g_extractedUnlockerX64Path.c_str())) {
            const DWORD error = GetLastError();
            const bool deferred = MoveFileExW(g_extractedUnlockerX64Path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT) != FALSE;
            GlassInstallLog::LineFormat(L"x64 Native cleanup failed (%lu), deferred=%d", error, deferred ? 1 : 0);
        }
        g_extractedUnlockerX64Path.clear();
    }
    if (!g_extractedUnlockerDir.empty()) {
        if (!RemoveDirectoryW(g_extractedUnlockerDir.c_str())) {
            const DWORD error = GetLastError();
            const bool deferred = MoveFileExW(g_extractedUnlockerDir.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT) != FALSE;
            GlassInstallLog::LineFormat(L"x64 Native temp directory cleanup failed (%lu), deferred=%d", error, deferred ? 1 : 0);
        }
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

namespace {

// Normalise before checking roots or comparing paths. Refuse device paths and
// UNC share roots as well as drive roots; these are never product directories.
bool NormalProductDirectory(const std::wstring& path, std::wstring& normalized) {
    normalized.clear();
    const bool driveAbsolute = path.size() >= 3 &&
        ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
        path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
    const bool uncAbsolute = path.size() >= 3 && path[0] == L'\\' && path[1] == L'\\';
    if ((!driveAbsolute && !uncAbsolute) || PathIsRelativeW(path.c_str()) ||
        path.rfind(L"\\\\?\\", 0) == 0 || path.rfind(L"\\\\.\\", 0) == 0) return false;
    const DWORD needed = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (!needed || needed > 32767) return false;
    std::vector<wchar_t> buffer(static_cast<size_t>(needed) + 1);
    const DWORD length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (!length || length >= buffer.size()) return false;
    normalized.assign(buffer.data(), length);
    for (auto& ch : normalized) if (ch == L'/') ch = L'\\';
    if (PathIsRootW(normalized.c_str())) return false;
    if (normalized.rfind(L"\\\\", 0) == 0) {
        const size_t server = normalized.find(L'\\', 2);
        const size_t shareEnd = server == std::wstring::npos ? server : normalized.find(L'\\', server + 1);
        if (server == std::wstring::npos || shareEnd == std::wstring::npos || shareEnd + 1 >= normalized.size())
            return false;
    }
    normalized = TrimTrailingSlashes(normalized);
    return !normalized.empty();
}

struct LockedDirectory {
    HANDLE handle = INVALID_HANDLE_VALUE;
    explicit LockedDirectory(HANDLE h = INVALID_HANDLE_VALUE) noexcept : handle(h) {}
    LockedDirectory(const LockedDirectory&) = delete;
    LockedDirectory& operator=(const LockedDirectory&) = delete;
    LockedDirectory(LockedDirectory&& other) noexcept : handle(other.handle) { other.handle = INVALID_HANDLE_VALUE; }
    ~LockedDirectory() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    explicit operator bool() const noexcept { return handle != INVALID_HANDLE_VALUE; }
};

struct FindGuard {
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~FindGuard() { if (handle != INVALID_HANDLE_VALUE) FindClose(handle); }
};

struct EmptyDirectoryFileSystem {
    LockedDirectory OpenDirectory(const std::wstring& path, DWORD access) const noexcept {
        // Keep each ancestor open without FILE_SHARE_DELETE while visiting its
        // children. OPEN_REPARSE_POINT and the handle check reject links even
        // if a child was replaced between enumeration and opening it.
        HANDLE handle = CreateFileW(path.c_str(), access,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
                GlassInstallLog::LineFormat(L"Empty-directory cleanup skipped %s (%lu)", path.c_str(), error);
            return LockedDirectory{};
        }
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle, &info)) {
            const DWORD error = GetLastError();
            GlassInstallLog::LineFormat(L"Empty-directory cleanup could not inspect %s (%lu)", path.c_str(), error);
            CloseHandle(handle);
            return LockedDirectory{};
        }
        if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            GlassInstallLog::LineFormat(L"Empty-directory cleanup skipped non-directory or reparse point %s", path.c_str());
            CloseHandle(handle);
            return LockedDirectory{};
        }
        return LockedDirectory{handle};
    }

    LockedDirectory OpenOrdinaryDirectory(const std::wstring& path) const noexcept {
        return OpenDirectory(path, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | DELETE);
    }

    bool LockAncestors(const std::wstring& path, std::vector<LockedDirectory>& locked) const {
        size_t position = 0;
        if (path.rfind(L"\\\\", 0) == 0) {
            const size_t server = path.find(L'\\', 2);
            if (server == std::wstring::npos) return false;
            position = path.find(L'\\', server + 1); // include the UNC share root
        } else {
            position = path.find(L'\\', 3); // drive root itself is not a link
        }
        while (position != std::wstring::npos) {
            auto directory = OpenDirectory(path.substr(0, position), FILE_READ_ATTRIBUTES);
            if (!directory) {
                GlassInstallLog::LineFormat(L"Empty-directory cleanup refused an inaccessible or linked ancestor of %s", path.c_str());
                return false;
            }
            locked.push_back(std::move(directory));
            position = path.find(L'\\', position + 1);
        }
        return true;
    }

    bool ReadEntries(const LockedDirectory&, const std::wstring& path,
        std::vector<GlassEmptyDirectories::Entry>& entries) const {
        WIN32_FIND_DATAW data{};
        FindGuard find{FindFirstFileW((path + L"\\*").c_str(), &data)};
        if (find.handle == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND) return true;
            GlassInstallLog::LineFormat(L"Empty-directory enumeration failed %s (%lu)", path.c_str(), error);
            return false;
        }
        do {
            entries.push_back({data.cFileName,
                (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0});
        } while (FindNextFileW(find.handle, &data));
        const DWORD error = GetLastError();
        if (error == ERROR_NO_MORE_FILES) return true;
        GlassInstallLog::LineFormat(L"Empty-directory enumeration interrupted %s (%lu)", path.c_str(), error);
        return false;
    }

    void RemoveIfEmpty(const LockedDirectory& directory, const std::wstring& path) const noexcept {
        FILE_DISPOSITION_INFO disposition{};
        disposition.DeleteFile = TRUE;
        if (SetFileInformationByHandle(directory.handle, FileDispositionInfo, &disposition, sizeof(disposition))) {
            // Actual deletion can be deferred by other handles after this handle closes.
            GlassInstallLog::LineFormat(L"Marked empty directory for removal %s", path.c_str());
        } else {
            const DWORD error = GetLastError();
            // Report even non-empty directories: hidden files, skipped links,
            // pending deletes, or concurrent changes can all retain the parent.
            GlassInstallLog::LineFormat(L"Empty-directory cleanup retained %s (%lu)", path.c_str(), error);
        }
    }
};

} // namespace

bool SameInstallPath(const std::wstring& left, const std::wstring& right) noexcept {
    try {
        std::wstring a, b;
        return NormalProductDirectory(left, a) && NormalProductDirectory(right, b) &&
            _wcsicmp(a.c_str(), b.c_str()) == 0;
    } catch (...) { return false; }
}

bool IsWithinInstallRoot(const std::wstring& root, const std::wstring& path) noexcept {
    try {
        std::wstring parent, child;
        if (!NormalProductDirectory(root,parent) || !NormalProductDirectory(path,child) ||
            child.size()<parent.size() || _wcsnicmp(parent.c_str(),child.c_str(),parent.size())!=0)
            return false;
        return child.size()==parent.size() || child[parent.size()]==L'\\';
    } catch (...) { return false; }
}

bool IsSafeInstallPath(const std::wstring& path) noexcept {
    try {
        std::wstring normalized;
        if (!NormalProductDirectory(path,normalized)) return false;
        size_t start=3; // drive root has no child component to inspect
        if (normalized.rfind(L"\\\\",0)==0) {
            const size_t server=normalized.find(L'\\',2);
            const size_t share=server==std::wstring::npos ? server : normalized.find(L'\\',server+1);
            if (share==std::wstring::npos) return false;
            start=share+1;
        }
        while (start<=normalized.size()) {
            const size_t end=normalized.find(L'\\',start);
            const std::wstring component=normalized.substr(0,end);
            const DWORD attr=GetFileAttributesW(component.c_str());
            if (attr==INVALID_FILE_ATTRIBUTES) {
                const DWORD error=GetLastError();
                // Once an ancestor does not exist, no deeper component can
                // already be a link. Recheck immediately before mutations.
                return error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND;
            }
            if ((attr & FILE_ATTRIBUTE_REPARSE_POINT) ||
                (end!=std::wstring::npos && !(attr & FILE_ATTRIBUTE_DIRECTORY))) return false;
            if (end==std::wstring::npos) {
                if (attr & FILE_ATTRIBUTE_DIRECTORY) return true;
                // Overwriting an NTFS hard link can modify a file outside the
                // product tree even though its name is lexically inside it.
                HANDLE file=CreateFileW(component.c_str(),FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
                    FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
                if (file==INVALID_HANDLE_VALUE) return false;
                BY_HANDLE_FILE_INFORMATION info{};
                const bool safe=GetFileInformationByHandle(file,&info) &&
                    !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) &&
                    info.nNumberOfLinks==1;
                CloseHandle(file);
                return safe;
            }
            start=end+1;
        }
        return true;
    } catch (...) { return false; }
}

namespace {

// FOLDERID_ProgramFilesX64 is not supported for a 32-bit process, including
// on x64 Windows. Query the Windows machine-wide native registry view instead
// of trusting ProgramW6432 from an inherited, potentially modified environment.
#ifndef _WIN64
LSTATUS ReadNativeProgramFilesValue(const wchar_t* name, std::wstring& value) {
    value.clear();
    HKEY key=nullptr;
    LSTATUS status=RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion",0,KEY_QUERY_VALUE|KEY_WOW64_64KEY,&key);
    if (status!=ERROR_SUCCESS) return status;
    wchar_t buffer[32768]{};
    DWORD type=0, bytes=sizeof(buffer);
    status=RegQueryValueExW(key,name,nullptr,&type,reinterpret_cast<LPBYTE>(buffer),&bytes);
    RegCloseKey(key);
    if (status!=ERROR_SUCCESS) return status;
    if (type!=REG_SZ || bytes<sizeof(wchar_t) || bytes>sizeof(buffer) ||
        bytes%sizeof(wchar_t)!=0) return ERROR_INVALID_DATA;
    const size_t count=bytes/sizeof(wchar_t);
    if (buffer[count-1]!=L'\0') return ERROR_INVALID_DATA;
    const size_t length=std::wcslen(buffer);
    if (!length) return ERROR_INVALID_DATA;
    for (size_t i=length+1;i<count;++i) if (buffer[i]!=L'\0') return ERROR_INVALID_DATA;
    value.assign(buffer,length);
    return ERROR_SUCCESS;
}
#endif

DWORD ValidateSystemProgramFiles(const std::wstring& input, std::wstring& normalized) {
    // Accept only a local, fully qualified path to an existing ordinary
    // directory. Do not normalize an ambiguous '..', device, or relative path
    // into a new installation target.
    if (input.size()<3 || !((input[0]>=L'A' && input[0]<=L'Z') ||
            (input[0]>=L'a' && input[0]<=L'z')) || input[1]!=L':' ||
        (input[2]!=L'\\' && input[2]!=L'/')) return ERROR_INVALID_NAME;
    for (size_t i=2;i<input.size();++i)
        if (input[i]<32 || input[i]==L':' || input[i]==L'%' || input[i]==L'"' ||
            input[i]==L'?' || input[i]==L'*' || input[i]==L'|' || input[i]==L'<' || input[i]==L'>')
            return ERROR_INVALID_NAME;
    if (!NormalProductDirectory(input,normalized)) return ERROR_INVALID_NAME;
    std::wstring supplied=TrimTrailingSlashes(input);
    for (auto& ch:supplied) if (ch==L'/') ch=L'\\';
    if (_wcsicmp(supplied.c_str(),normalized.c_str())!=0) return ERROR_INVALID_NAME;
    const DWORD attributes=GetFileAttributesW(normalized.c_str());
    if (attributes==INVALID_FILE_ATTRIBUTES) {
        const DWORD error=GetLastError();
        return error ? error : ERROR_PATH_NOT_FOUND;
    }
    if (!(attributes&FILE_ATTRIBUTE_DIRECTORY) || (attributes&FILE_ATTRIBUTE_REPARSE_POINT)) return ERROR_INVALID_DATA;
    const std::wstring target=normalized+L"\\GlassPlatform";
    const DWORD targetAttributes=GetFileAttributesW(target.c_str());
    if (targetAttributes==INVALID_FILE_ATTRIBUTES) {
        const DWORD error=GetLastError();
        if (error!=ERROR_FILE_NOT_FOUND && error!=ERROR_PATH_NOT_FOUND) return error ? error : ERROR_INVALID_DATA;
    } else if (!(targetAttributes&FILE_ATTRIBUTE_DIRECTORY)) return ERROR_INVALID_DATA;
    if (!IsSafeInstallPath(target)) return ERROR_INVALID_DATA;
    return ERROR_SUCCESS;
}

} // namespace

namespace {

bool QueryNativeOs64Bit(bool& os64) noexcept {
#ifdef _WIN64
    os64=true;
    return true;
#else
    BOOL wow=FALSE;
    if (!IsWow64Process(GetCurrentProcess(),&wow)) return false;
    os64=wow!=FALSE;
    return true;
#endif
}

// Resolve one default only when its corresponding product path is absent.
// The other architecture may have an unrelated registered location.
DWORD ResolveDefaultPlatformRoot(bool x64, bool os64, std::wstring& root) {
    root.clear();
    std::wstring raw;
    if (x64) {
        if (!os64) return ERROR_INVALID_PARAMETER;
#ifdef _WIN64
        PWSTR folder=nullptr;
        const HRESULT hr=SHGetKnownFolderPath(FOLDERID_ProgramFiles,KF_FLAG_DEFAULT,nullptr,&folder);
        struct FolderGuard { PWSTR p; ~FolderGuard(){ if (p) CoTaskMemFree(p); } } guard{folder};
        if (FAILED(hr) || !folder) return FAILED(hr) ? HRESULT_CODE(hr) : ERROR_PATH_NOT_FOUND;
        raw.assign(folder);
#else
        const LSTATUS native=ReadNativeProgramFilesValue(L"ProgramFilesDir",raw);
        if (native!=ERROR_SUCCESS) return static_cast<DWORD>(native);
        std::wstring nativeAlias;
        const LSTATUS alias=ReadNativeProgramFilesValue(L"ProgramW6432Dir",nativeAlias);
        if (alias!=ERROR_SUCCESS) return static_cast<DWORD>(alias);
        if (!SameInstallPath(raw,nativeAlias)) return ERROR_INVALID_DATA;
#endif
    } else {
        PWSTR folder=nullptr;
        HRESULT hr=SHGetKnownFolderPath(FOLDERID_ProgramFilesX86,KF_FLAG_DEFAULT,nullptr,&folder);
        if ((FAILED(hr) || !folder) && !os64) {
            if (folder) CoTaskMemFree(folder);
            folder=nullptr;
            hr=SHGetKnownFolderPath(FOLDERID_ProgramFiles,KF_FLAG_DEFAULT,nullptr,&folder);
        }
        struct FolderGuard { PWSTR p; ~FolderGuard(){ if (p) CoTaskMemFree(p); } } guard{folder};
        if (FAILED(hr) || !folder) return FAILED(hr) ? HRESULT_CODE(hr) : ERROR_PATH_NOT_FOUND;
        raw.assign(folder);
    }
    std::wstring base;
    const DWORD error=ValidateSystemProgramFiles(raw,base);
    if (error!=ERROR_SUCCESS) return error;
    root=base+L"\\GlassPlatform";
    return ERROR_SUCCESS;
}

} // namespace

bool ResolveFixedPlatformRoots(std::wstring& x64, std::wstring& x86) noexcept {
    x64.clear(); x86.clear();
    auto fail=[&](DWORD error) { x64.clear(); x86.clear(); SetLastError(error); return false; };
    try {
        bool os64=false;
        if (!QueryNativeOs64Bit(os64)) return fail(GetLastError());
        DWORD error=ResolveDefaultPlatformRoot(false,os64,x86);
        if (error!=ERROR_SUCCESS) return fail(error);
        if (os64) {
            error=ResolveDefaultPlatformRoot(true,os64,x64);
            if (error!=ERROR_SUCCESS) return fail(error);
            if (IsWithinInstallRoot(x64,x86) || IsWithinInstallRoot(x86,x64))
                return fail(ERROR_INVALID_DATA);
        }
        SetLastError(ERROR_SUCCESS);
        return true;
    } catch (...) { return fail(ERROR_NOT_ENOUGH_MEMORY); }
}

void PruneEmptyDirectories(const std::wstring& root, const std::vector<std::wstring>& protectedRoots) noexcept {
    try {
        std::wstring normalized;
        if (!NormalProductDirectory(root, normalized)) {
            GlassInstallLog::LineFormat(L"Empty-directory cleanup refused unsafe root %s", root.c_str());
            return;
        }
        GlassInstallLog::LineFormat(L"Empty-directory cleanup starting %s", normalized.c_str());
        EmptyDirectoryFileSystem fs;
        std::vector<LockedDirectory> ancestors, protectedHandles;
        if (!fs.LockAncestors(normalized, ancestors)) return;
        for (const auto& kept : protectedRoots) {
            if (kept.empty()) continue;
            std::wstring protectedPath;
            if (!NormalProductDirectory(kept, protectedPath)) {
                GlassInstallLog::LineFormat(L"Empty-directory cleanup refused unsafe kept root %s", kept.c_str());
                return;
            }
            const DWORD attributes = GetFileAttributesW(protectedPath.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                const DWORD error = GetLastError();
                if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) continue;
                GlassInstallLog::LineFormat(L"Empty-directory cleanup could not inspect kept root %s (%lu)", kept.c_str(), error);
                return;
            }
            if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                GlassInstallLog::LineFormat(L"Empty-directory cleanup refused linked kept root %s", protectedPath.c_str());
                return;
            }
            // Sharing denies DELETE opens through any spelling/8.3 alias of a
            // kept directory. Its non-empty parent is also retained, while
            // unrelated empty siblings can still be pruned.
            auto handle = fs.OpenDirectory(protectedPath, FILE_READ_ATTRIBUTES);
            if (!handle) return;
            protectedHandles.push_back(std::move(handle));
        }
        GlassEmptyDirectories::Prune(fs, normalized);
    } catch (...) {
        GlassInstallLog::Line(L"Empty-directory cleanup stopped because of an unexpected error");
    }
}

bool RunHiddenCommand(const std::wstring& commandLine, DWORD* exitCode, DWORD timeoutMs,
    bool* started, bool* completed, const std::atomic_bool* cancelled) noexcept {
    if (started) *started=false;
    if (completed) *completed=false;
    if (cancelled && cancelled->load(std::memory_order_acquire)) {
        if (exitCode) *exitCode=ERROR_CANCELLED;
        return false;
    }
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
    DWORD wait = WAIT_TIMEOUT;
    bool wasCancelled = false;
    if (cancelled) {
        const ULONGLONG start = GetTickCount64();
        for (;;) {
            if (cancelled->load(std::memory_order_acquire)) { wasCancelled = true; break; }
            const ULONGLONG elapsed = GetTickCount64() - start;
            if (elapsed >= timeoutMs) break;
            const DWORD remaining = static_cast<DWORD>(timeoutMs - elapsed);
            wait = WaitForSingleObject(pi.hProcess, remaining < 100 ? remaining : 100);
            if (wait != WAIT_TIMEOUT) break;
        }
    } else wait = WaitForSingleObject(pi.hProcess, timeoutMs);
    if (wait != WAIT_OBJECT_0 || wasCancelled) {
        const DWORD error = wasCancelled ? ERROR_CANCELLED :
            wait == WAIT_TIMEOUT ? static_cast<DWORD>(WAIT_TIMEOUT) : GetLastError();
        // This handle belongs to the child created above, never a user process.
        // Even WAIT_FAILED must not leave a helper running after failure.
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
        CloseHandle(pi.hProcess);
        if (exitCode) *exitCode = error;
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

bool QueryImageRunning(const wchar_t* imageName, bool& found) noexcept {
    found = false;
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool ok = true;
    if (Process32FirstW(snap, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, imageName) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &entry));
        if (!found && GetLastError() != ERROR_NO_MORE_FILES) ok = false;
    } else if (GetLastError() != ERROR_NO_MORE_FILES) ok = false;
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(snap);
    if (!ok) SetLastError(error);
    return ok;
}

bool ImageRunning(const wchar_t* imageName) noexcept {
    bool found = false;
    return QueryImageRunning(imageName, found) && found;
}

namespace {
using Microsoft::WRL::ComPtr;

struct SchedulerComGuard {
    const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~SchedulerComGuard() { if (SUCCEEDED(result)) CoUninitialize(); }
    bool ok() const noexcept { return SUCCEEDED(result) || result == RPC_E_CHANGED_MODE; }
};
struct SchedulerBstr {
    BSTR value = nullptr;
    explicit SchedulerBstr(const wchar_t* text = nullptr) noexcept : value(text ? SysAllocString(text) : nullptr) {}
    ~SchedulerBstr() { if (value) SysFreeString(value); }
    SchedulerBstr(const SchedulerBstr&) = delete;
    SchedulerBstr& operator=(const SchedulerBstr&) = delete;
};
bool SchedulerError(HRESULT hr) noexcept {
    const DWORD code = HRESULT_FACILITY(hr) == FACILITY_WIN32 ? HRESULT_CODE(hr) : ERROR_GEN_FAILURE;
    SetLastError(code ? code : ERROR_GEN_FAILURE);
    return false;
}

struct OpenGlassTaskCandidates {
    ComPtr<IRegisteredTask> current;
    ComPtr<IRegisteredTask> legacy;
};
enum class OpenGlassTaskOperation { Query, Start, Stop };

bool InspectOpenGlassTaskFolder(ITaskFolder* folder, OpenGlassTaskState& state,
    OpenGlassTaskCandidates& candidates, unsigned depth, OpenGlassTaskOperation operation,
    bool* stopRequested, const std::atomic_bool* cancelled) noexcept {
    if (!folder || depth > 64) return SchedulerError(E_FAIL);
    if (cancelled && cancelled->load(std::memory_order_acquire)) {
        SetLastError(ERROR_CANCELLED);
        return false;
    }
    ComPtr<IRegisteredTaskCollection> tasks;
    HRESULT hr = folder->GetTasks(TASK_ENUM_HIDDEN, tasks.GetAddressOf());
    if (FAILED(hr) || !tasks) return SchedulerError(FAILED(hr) ? hr : E_FAIL);
    LONG count = 0;
    hr = tasks->get_Count(&count);
    if (FAILED(hr) || count < 0) return SchedulerError(FAILED(hr) ? hr : E_FAIL);
    for (LONG i = 1; i <= count; ++i) {
        VARIANT index{}; VariantInit(&index); index.vt = VT_I4; index.lVal = i;
        ComPtr<IRegisteredTask> task;
        hr = tasks->get_Item(index, task.GetAddressOf());
        if (FAILED(hr) || !task) return SchedulerError(FAILED(hr) ? hr : E_FAIL);
        SchedulerBstr name;
        hr = task->get_Name(&name.value);
        if (FAILED(hr) || !name.value) return SchedulerError(FAILED(hr) ? hr : E_FAIL);
        const bool isCurrent = _wcsicmp(name.value, L"OpenGlass Host") == 0;
        const bool isLegacy = _wcsicmp(name.value, L"OpenGlass Legacy Host") == 0;
        if (!isCurrent && !isLegacy) continue;
        if (isCurrent && !candidates.current) candidates.current = task;
        if (isLegacy && !candidates.legacy) candidates.legacy = task;
        VARIANT_BOOL enabled = VARIANT_FALSE;
        hr = task->get_Enabled(&enabled);
        if (FAILED(hr)) return SchedulerError(hr);
        ComPtr<IRunningTaskCollection> running;
        hr = task->GetInstances(0, running.GetAddressOf());
        if (FAILED(hr) || !running) return SchedulerError(FAILED(hr) ? hr : E_FAIL);
        LONG instances = 0;
        hr = running->get_Count(&instances);
        if (FAILED(hr) || instances < 0) return SchedulerError(FAILED(hr) ? hr : E_FAIL);
        state.multipleTasks = state.multipleTasks || state.found;
        state.multipleRunningInstances = state.multipleRunningInstances || instances > 1 ||
            (state.anyRunning && instances > 0);
        state.found = true;
        state.anyDisabled = state.anyDisabled || enabled == VARIANT_FALSE;
        state.anyRunning = state.anyRunning || instances > 0;
        if (operation == OpenGlassTaskOperation::Stop && instances > 0) {
            if (cancelled && cancelled->load(std::memory_order_acquire)) {
                SetLastError(ERROR_CANCELLED);
                return false;
            }
            // Stop can partially succeed even when it returns an error. Record
            // the attempt before calling COM, so rollback does not forget it.
            if (stopRequested) *stopRequested = true;
            hr = task->Stop(0);
            if (FAILED(hr)) return SchedulerError(hr);
        }
    }
    ComPtr<ITaskFolderCollection> folders;
    hr = folder->GetFolders(0, folders.GetAddressOf());
    if (FAILED(hr) || !folders) return SchedulerError(FAILED(hr) ? hr : E_FAIL);
    hr = folders->get_Count(&count);
    if (FAILED(hr) || count < 0) return SchedulerError(FAILED(hr) ? hr : E_FAIL);
    for (LONG i = 1; i <= count; ++i) {
        VARIANT index{}; VariantInit(&index); index.vt = VT_I4; index.lVal = i;
        ComPtr<ITaskFolder> child;
        hr = folders->get_Item(index, child.GetAddressOf());
        if (FAILED(hr) || !child) return SchedulerError(FAILED(hr) ? hr : E_FAIL);
        if (!InspectOpenGlassTaskFolder(child.Get(), state, candidates, depth + 1,
                                        operation, stopRequested, cancelled)) return false;
    }
    return true;
}

// When requested, run only a still-enabled task via IRegisteredTask::Run;
// unlike the shared UI helper this never enables a task as a side effect.
bool AccessOpenGlassTasks(OpenGlassTaskState& state, OpenGlassTaskOperation operation,
    bool* stopRequested = nullptr, const std::atomic_bool* cancelled = nullptr) noexcept {
    state = {};
    if (cancelled && cancelled->load(std::memory_order_acquire)) {
        SetLastError(ERROR_CANCELLED);
        return false;
    }
    SchedulerComGuard com;
    if (!com.ok()) return SchedulerError(com.result);
    ComPtr<ITaskService> service;
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
        IID_ITaskService, reinterpret_cast<void**>(service.GetAddressOf()));
    if (FAILED(hr) || !service) return SchedulerError(FAILED(hr) ? hr : E_FAIL);
    VARIANT empty{}; VariantInit(&empty);
    hr = service->Connect(empty, empty, empty, empty);
    if (FAILED(hr)) return SchedulerError(hr);
    SchedulerBstr path(L"\\");
    if (!path.value) return SchedulerError(E_OUTOFMEMORY);
    ComPtr<ITaskFolder> root;
    hr = service->GetFolder(path.value, root.GetAddressOf());
    if (FAILED(hr) || !root) return SchedulerError(FAILED(hr) ? hr : E_FAIL);
    OpenGlassTaskCandidates candidates;
    if (!InspectOpenGlassTaskFolder(root.Get(), state, candidates, 0, operation,
                                    stopRequested, cancelled)) return false;
    if (operation != OpenGlassTaskOperation::Start) return true;
    if (!state.found || state.anyDisabled) return SchedulerError(E_FAIL);
    if (state.anyRunning) return true;
    ComPtr<IRegisteredTask> task = candidates.current ? candidates.current : candidates.legacy;
    if (!task) return SchedulerError(E_FAIL);
    VARIANT_BOOL enabled = VARIANT_FALSE;
    hr = task->get_Enabled(&enabled);
    if (FAILED(hr) || enabled == VARIANT_FALSE) return SchedulerError(FAILED(hr) ? hr : E_FAIL);
    ComPtr<IRunningTask> running;
    hr = task->Run(empty, running.GetAddressOf());
    return SUCCEEDED(hr) && running ? true : SchedulerError(FAILED(hr) ? hr : E_FAIL);
}
} // namespace

bool QueryOpenGlassTasks(OpenGlassTaskState& state) noexcept {
    return AccessOpenGlassTasks(state, OpenGlassTaskOperation::Query);
}

bool RunEnabledOpenGlassTask() noexcept {
    OpenGlassTaskState state;
    return AccessOpenGlassTasks(state, OpenGlassTaskOperation::Start);
}

bool StopRunningOpenGlassTasks(bool& requested, const std::atomic_bool* cancelled) noexcept {
    requested = false;
    OpenGlassTaskState state;
    if (!AccessOpenGlassTasks(state, OpenGlassTaskOperation::Query, nullptr, cancelled)) return false;
    // We can faithfully restart one task instance, not an ambiguous set.
    if (state.anyRunning && (state.multipleTasks || state.multipleRunningInstances)) {
        SetLastError(ERROR_INVALID_DATA);
        return false;
    }
    if (!state.anyRunning) return true;
    return AccessOpenGlassTasks(state, OpenGlassTaskOperation::Stop, &requested, cancelled);
}

bool QueryOpenGlassRunning(bool& running, const OpenGlassTaskState* knownTasks) noexcept {
    running = false;
    // The service/task state, not the ordinary DWM window or merely the
    // presence of an executable, determines whether a managed host is active.
    // A disabled service can still be running, so inspect its actual status.
    const SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    const SC_HANDLE service = OpenServiceW(scm, L"OpenGlassHost", SERVICE_QUERY_STATUS);
    const bool serviceExists = service != nullptr;
    const DWORD openError = service ? ERROR_SUCCESS : GetLastError();
    CloseServiceHandle(scm);
    if (!service && openError != ERROR_SERVICE_DOES_NOT_EXIST) { SetLastError(openError); return false; }
    if (service) {
        SERVICE_STATUS_PROCESS status{};
        DWORD needed = 0;
        const BOOL ok = QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
            reinterpret_cast<BYTE*>(&status), sizeof(status), &needed);
        const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
        CloseServiceHandle(service);
        if (!ok) { SetLastError(error); return false; }
        if (status.dwCurrentState != SERVICE_STOPPED) {
            running = true; // includes start/stop pending and paused
            return true;
        }
    }
    OpenGlassTaskState tasks;
    if (knownTasks) tasks = *knownTasks;
    else if (!QueryOpenGlassTasks(tasks)) return false;
    if (tasks.anyRunning) { running = true; return true; }
    if (serviceExists || tasks.found) return true; // managed service/task is stopped

    // If no managed backend exists, detect an independently started host so
    // callers can refuse to stop an instance they cannot safely restore.
    return QueryImageRunning(L"OpenGlassHost.exe", running);
}

bool QueryOpenGlassFiles(const std::wstring& openGlassDir, const std::wstring& platformDir, bool& present) noexcept {
    present = false;
    if (openGlassDir.empty()) return false;
    try {
        const std::wstring og = TrimTrailingSlashes(openGlassDir);
        const std::wstring platform = TrimTrailingSlashes(platformDir);
        std::vector<std::wstring> candidates = {og + L"\\OpenGlass.dll", og + L"\\OpenGlassHost.exe",
            og + L"\\OpenGlass\\OpenGlass.dll"};
        if (!platform.empty()) candidates.push_back(platform + L"\\OpenGlass.dll");
        for (const auto& path : candidates) {
            const DWORD attr = GetFileAttributesW(path.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES) {
                if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) { present = true; return true; }
                continue;
            }
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return false;
        }
        return true;
    } catch (...) { return false; }
}

BOOL CALLBACK CloseWindowsForPid(HWND hwnd, LPARAM param) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == static_cast<DWORD>(param)) PostMessageW(hwnd, WM_CLOSE, 0, 0);
    return TRUE;
}

void CloseGlassSettingsProcesses(const std::atomic_bool* cancelled) noexcept {
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snap, &entry)) {
        do {
            if (cancelled && cancelled->load(std::memory_order_acquire)) break;
            if (_wcsicmp(entry.szExeFile, L"GlassSettings.exe") != 0) continue;
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, entry.th32ProcessID);
            if (!process) continue;
            wchar_t path[32768]{};
            DWORD length = static_cast<DWORD>(std::size(path));
            const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
            if (alive && QueryFullProcessImageNameW(process, 0, path, &length)) {
                const wchar_t* name = wcsrchr(path, L'\\');
                if (_wcsicmp(name ? name + 1 : path, L"GlassSettings.exe") == 0)
                    EnumWindows(CloseWindowsForPid, static_cast<LPARAM>(entry.th32ProcessID));
            }
            CloseHandle(process);
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    const ULONGLONG start = GetTickCount64();
    while ((!cancelled || !cancelled->load(std::memory_order_acquire)) &&
        ImageRunning(L"GlassSettings.exe") && GetTickCount64() - start < 8000) Sleep(200);
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
        // The x64 helper is embedded in Setup/Unins and extracted to a
        // restricted temporary directory. A missing resource fails closed.
        if (!g_unlockerInstance || !g_unlockerX64ResId ||
            !FindResourceW(g_unlockerInstance,MAKEINTRESOURCEW(g_unlockerX64ResId),RT_RCDATA)) return {};
        return EnsureExtractedUnlockerX64();
    }
    // Setup also embeds x86; prefer that resource over an installed helper.
    // The resource is trusted only to the extent the Setup executable is trusted.
    if (g_unlockerX86ResId) return EmbeddedNativeDllX86();
    const std::wstring root = x86Dir;
    if (!root.empty()) {
        const std::wstring dll = TrimTrailingSlashes(root) + L"\\GlassSettings\\GlassSettings.Native.dll";
        if (PeMatches(dll, machine, "GlassSettings_UnlockerW"))
            return dll;
    }
    // Unins without an embedded x86 resource can use the resolved product
    // root, which may be registry-derived. PeMatches checks format and export,
    // not the origin or signature of that DLL.
    return {};
}

namespace {

RegistryPathRead ReadPlatformInstallPath(bool x64, bool os64,
    std::wstring& path, std::wstring& source) {
    path.clear(); source.clear();
    if (x64 && !os64) return RegistryPathRead::Missing;
    const wchar_t* explicitValue=x64?L"InstallPathX64":L"InstallPathX86";
    const REGSAM machineView=x64?KEY_WOW64_64KEY:KEY_WOW64_32KEY;
    const auto read=[&](HKEY hive,const wchar_t* key,const wchar_t* value,
                        REGSAM view,const wchar_t* label) {
        const auto result=ReadInstallPathValue(hive,key,value,view,path);
        if (result==RegistryPathRead::Found) source=label;
        return result;
    };
    // Preserve the established machine-before-user precedence. The view and
    // value actually used are reported, rather than comparing or repairing
    // other registry copies.
    auto status=read(HKEY_LOCAL_MACHINE,L"Software\\GlassPlatform",explicitValue,machineView,
        x64?L"HKLM(64) InstallPathX64":L"HKLM(32) InstallPathX86");
    if (status!=RegistryPathRead::Missing) return status;
    status=read(HKEY_LOCAL_MACHINE,L"Software\\GlassPlatform",L"InstallPath",machineView,
        x64?L"HKLM(64) InstallPath":L"HKLM(32) InstallPath");
    if (status!=RegistryPathRead::Missing) return status;
    status=read(HKEY_CURRENT_USER,L"Software\\GlassPlatform",explicitValue,0,
        x64?L"HKCU InstallPathX64":L"HKCU InstallPathX86");
    if (status!=RegistryPathRead::Missing) return status;
    if (!x64 && os64)
        return read(HKEY_CURRENT_USER,L"Software\\Wow6432Node\\GlassPlatform",L"InstallPath",0,
            L"HKCU Wow6432Node InstallPath");
    return read(HKEY_CURRENT_USER,L"Software\\GlassPlatform",L"InstallPath",0,L"HKCU InstallPath");
}

RegistryPathRead ReadOpenGlassInstallPath(std::wstring& path, std::wstring& source) {
    path.clear(); source.clear();
    const auto read=[&](HKEY hive,const wchar_t* key,const wchar_t* value,
                        REGSAM view,const wchar_t* label) {
        const auto result=ReadInstallPathValue(hive,key,value,view,path);
        if (result==RegistryPathRead::Found) source=label;
        return result;
    };
    auto status=read(HKEY_LOCAL_MACHINE,L"Software\\StanislavKa\\GlassPlatform",
        L"OpenGlassInstallPath",KEY_WOW64_64KEY,L"HKLM(64) OpenGlassInstallPath");
    if (status!=RegistryPathRead::Missing) return status;
    status=read(HKEY_CURRENT_USER,L"Software\\StanislavKa\\GlassPlatform",
        L"OpenGlassInstallPath",0,L"HKCU OpenGlassInstallPath");
    if (status!=RegistryPathRead::Missing) return status;
    return read(HKEY_CURRENT_USER,L"Software\\StanislavKa\\OpenGlass",
        L"InstallPath",0,L"HKCU legacy OpenGlass InstallPath");
}

DWORD ValidateRegisteredInstallRoot(std::wstring& path) {
    // Keep the registered destination, but reject malformed names, roots,
    // device paths and existing links before passing it to elevated operations.
    if (path.empty()) return ERROR_INVALID_NAME;
    for (size_t i=0;i<path.size();++i) {
        const wchar_t ch=path[i];
        if (ch<32 || ch==L'"' || ch==L'<' || ch==L'>' || ch==L'|' ||
            ch==L'?' || ch==L'*' || (ch==L':' && i!=1)) return ERROR_INVALID_NAME;
    }
    std::wstring normalized;
    if (!NormalProductDirectory(path,normalized)) return ERROR_INVALID_NAME;
    std::wstring supplied=TrimTrailingSlashes(path);
    for (auto& ch:supplied) if (ch==L'/') ch=L'\\';
    if (_wcsicmp(supplied.c_str(),normalized.c_str())!=0) return ERROR_INVALID_NAME;
    const DWORD attributes=GetFileAttributesW(normalized.c_str());
    if (attributes==INVALID_FILE_ATTRIBUTES) {
        const DWORD error=GetLastError();
        if (error!=ERROR_FILE_NOT_FOUND && error!=ERROR_PATH_NOT_FOUND)
            return error ? error : ERROR_PATH_NOT_FOUND;
    } else if (!(attributes&FILE_ATTRIBUTE_DIRECTORY)) return ERROR_INVALID_DATA;
    if (!IsSafeInstallPath(normalized)) return ERROR_INVALID_DATA;
    path=std::move(normalized);
    return ERROR_SUCCESS;
}

} // namespace

bool QueryPlatformInstallPath(bool x64, std::wstring& out) noexcept {
    try {
        bool os64=false;
        if (!QueryNativeOs64Bit(os64)) return false;
        std::wstring source;
        return ReadPlatformInstallPath(x64,os64,out,source)==RegistryPathRead::Found;
    } catch (...) { out.clear(); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return false; }
}

bool ResolveInstallRoots(InstallRoots& roots) noexcept {
    roots={};
    auto fail=[&](DWORD error) { roots={}; SetLastError(error?error:ERROR_INVALID_DATA); return false; };
    try {
        bool os64=false;
        if (!QueryNativeOs64Bit(os64)) return fail(GetLastError());
        const auto resolvePlatform=[&](bool x64,std::wstring& path,std::wstring& source) {
            const auto status=ReadPlatformInstallPath(x64,os64,path,source);
            if (status==RegistryPathRead::Error) return GetLastError();
            if (status==RegistryPathRead::Found) return ValidateRegisteredInstallRoot(path);
            const DWORD error=ResolveDefaultPlatformRoot(x64,os64,path);
            if (error==ERROR_SUCCESS) source=L"Windows Program Files default";
            return error;
        };
        DWORD error=resolvePlatform(false,roots.x86,roots.sourceX86);
        if (error!=ERROR_SUCCESS) return fail(error);
        if (os64) {
            error=resolvePlatform(true,roots.x64,roots.sourceX64);
            if (error!=ERROR_SUCCESS) return fail(error);
            const auto og=ReadOpenGlassInstallPath(roots.openGlass,roots.sourceOpenGlass);
            if (og==RegistryPathRead::Error) return fail(GetLastError());
            if (og==RegistryPathRead::Found) error=ValidateRegisteredInstallRoot(roots.openGlass);
            else {
                roots.openGlass=roots.x64+L"\\OpenGlass";
                roots.sourceOpenGlass=L"x64 root default";
                error=ValidateRegisteredInstallRoot(roots.openGlass);
            }
            if (error!=ERROR_SUCCESS) return fail(error);
        }
        SetLastError(ERROR_SUCCESS);
        return true;
    } catch (...) { return fail(ERROR_NOT_ENOUGH_MEMORY); }
}

bool WritePlatformInstallPath(bool x64, const std::wstring& installDir) noexcept {
    try {
        const std::wstring dir=TrimTrailingSlashes(installDir);
        if (dir.empty()) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
        bool os64=false;
        if (!QueryNativeOs64Bit(os64)) return false;
        const wchar_t* explicitVal=x64?L"InstallPathX64":L"InstallPathX86";
        const REGSAM lmSam=x64?KEY_WOW64_64KEY:KEY_WOW64_32KEY;
        if (!WriteRegSzIfMissing(HKEY_CURRENT_USER,L"Software\\GlassPlatform",explicitVal,0,dir) ||
            !WriteRegSzIfMissing(HKEY_LOCAL_MACHINE,L"Software\\GlassPlatform",explicitVal,lmSam,dir) ||
            !WriteRegSzIfMissing(HKEY_LOCAL_MACHINE,L"Software\\GlassPlatform",L"InstallPath",lmSam,dir)) return false;
        if (x64==os64 &&
            !WriteRegSzIfMissing(HKEY_CURRENT_USER,L"Software\\GlassPlatform",L"InstallPath",0,dir)) return false;
        if (!x64 && os64 &&
            !WriteRegSzIfMissing(HKEY_CURRENT_USER,L"Software\\Wow6432Node\\GlassPlatform",L"InstallPath",0,dir)) return false;
        // Only the authoritative value is rechecked. Conflicting copies are
        // neither compared with each other nor rewritten to match it.
        std::wstring effective,source;
        const auto status=ReadPlatformInstallPath(x64,os64,effective,source);
        if (status==RegistryPathRead::Error) return false;
        if (status!=RegistryPathRead::Found || !SameInstallPath(effective,dir)) {
            SetLastError(ERROR_INVALID_DATA); return false;
        }
        return true;
    } catch (...) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return false; }
}

bool ClearPlatformInstallPath(bool x64) noexcept {
    const wchar_t* explicitVal = x64 ? L"InstallPathX64" : L"InstallPathX86";
    const REGSAM lmSam = x64 ? KEY_WOW64_64KEY : KEY_WOW64_32KEY;
    if (!DeleteRegistryValue(HKEY_CURRENT_USER, L"Software\\GlassPlatform", explicitVal, 0) ||
        !DeleteRegistryValue(HKEY_LOCAL_MACHINE, L"Software\\GlassPlatform", explicitVal, lmSam) ||
        !DeleteRegistryValue(HKEY_LOCAL_MACHINE, L"Software\\GlassPlatform", L"InstallPath", lmSam)) return false;
    if (x64 == IsOs64Bit()) {
        return DeleteRegistryValue(HKEY_CURRENT_USER, L"Software\\GlassPlatform", L"InstallPath", 0);
    }
    if (!x64 && IsOs64Bit()) {
        return DeleteRegistryValue(HKEY_CURRENT_USER, L"Software\\Wow6432Node\\GlassPlatform", L"InstallPath", 0);
    }
    return true;
}

bool QueryOpenGlassInstallPath(std::wstring& out) noexcept {
    try {
        std::wstring source;
        return ReadOpenGlassInstallPath(out,source)==RegistryPathRead::Found;
    } catch (...) { out.clear(); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return false; }
}

bool WriteOpenGlassInstallPath(const std::wstring& x64RootDir, const std::wstring& openGlassDir) noexcept {
    try {
        const std::wstring root=TrimTrailingSlashes(x64RootDir);
        const std::wstring og=TrimTrailingSlashes(openGlassDir);
        if (root.empty() || og.empty()) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
        if (!WriteRegSzIfMissing(HKEY_CURRENT_USER,L"Software\\StanislavKa\\GlassPlatform",L"InstallPath",0,root) ||
            !WriteRegSzIfMissing(HKEY_LOCAL_MACHINE,L"Software\\StanislavKa\\GlassPlatform",L"InstallPath",KEY_WOW64_64KEY,root) ||
            !WriteRegSzIfMissing(HKEY_CURRENT_USER,L"Software\\StanislavKa\\GlassPlatform",L"OpenGlassInstallPath",0,og) ||
            !WriteRegSzIfMissing(HKEY_LOCAL_MACHINE,L"Software\\StanislavKa\\GlassPlatform",L"OpenGlassInstallPath",KEY_WOW64_64KEY,og)) return false;
        std::wstring effective,source;
        const auto status=ReadOpenGlassInstallPath(effective,source);
        if (status==RegistryPathRead::Error) return false;
        if (status!=RegistryPathRead::Found || !SameInstallPath(effective,og)) {
            SetLastError(ERROR_INVALID_DATA); return false;
        }
        return true;
    } catch (...) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return false; }
}

bool ClearOpenGlassInstallPath() noexcept {
    return DeleteRegistryValue(HKEY_CURRENT_USER, L"Software\\StanislavKa\\GlassPlatform", L"OpenGlassInstallPath", 0) &&
        DeleteRegistryValue(HKEY_LOCAL_MACHINE, L"Software\\StanislavKa\\GlassPlatform", L"OpenGlassInstallPath", KEY_WOW64_64KEY) &&
        DeleteRegistryValue(HKEY_CURRENT_USER, L"Software\\StanislavKa\\OpenGlass", L"InstallPath", 0);
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
    bool* registrationMayHaveChanged,
    bool* registrationConflict) noexcept {
    if (registrationMayHaveChanged) *registrationMayHaveChanged=false;
    if (registrationConflict) *registrationConflict=false;
    const bool os64 = IsOs64Bit();
    const auto arch = os64
        ? GlassShared::GlassPlatformTask::PlatformArch::X64
        : GlassShared::GlassPlatformTask::PlatformArch::X86;
    const std::wstring& selectedRoot=os64 ? x64Dir : x86Dir;
    const std::wstring root=TrimTrailingSlashes(selectedRoot);
    if (root.empty() || root.find(L'"')!=std::wstring::npos || !IsSafeInstallPath(root)) return false;
    const std::wstring dll = NativeDllForArch(arch, x64Dir, x86Dir);
    const WORD machine=os64?IMAGE_FILE_MACHINE_AMD64:IMAGE_FILE_MACHINE_I386;
    if (dll.empty() || !PeMatches(dll,machine,kInstallerShellExport)) {
        // Windows 11 requires the sparse package; do not silently substitute legacy regsvr32.
        // On 32-bit Windows 10, legacy registration is the supported fallback.
        if (os64) return false;
        const std::wstring shellExt = root + L"\\GlassShellExt.dll";
        if (!IsSafeInstallPath(shellExt) || !PathFileExistsW(shellExt.c_str())) return false;
        if (HasExistingShellRegistrationKeys()) {
            const bool matches=HasMatchingLegacyShellRegistration(root);
            if (!matches) {
                GlassInstallLog::Line(L"ShellExt: existing legacy registration does not match the selected root");
                if (registrationConflict) *registrationConflict=true;
                return false;
            }
            if (registerExtension) return true;
        }
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
    std::wstring cmd = L"\"" + rundll + L"\" \"" + dll + L"\",GlassSettings_ShellExtRegisterSetupV1W ";
    cmd += registerExtension ? L"/register" : L"/unregister";
    cmd += L" /installroot \"" + root + L"\"";
    DWORD exitCode = 1;
    bool started=false,completed=false;
    const bool ran = RunHiddenCommand(cmd, &exitCode, 45000, &started, &completed);
    constexpr DWORD kShellAlreadyRegistered=0x47505245; // Verified existing registration.
    constexpr DWORD kShellRegistrationConflict=0x47504346; // Preserved, no change made.
    const bool alreadyRegistered=registerExtension && completed && exitCode==kShellAlreadyRegistered;
    const bool conflict=completed && exitCode==kShellRegistrationConflict;
    if (registrationMayHaveChanged) *registrationMayHaveChanged=started && !alreadyRegistered && !conflict;
    GlassInstallLog::LineFormat(L"ShellExt %s via Native.dll: ran=%d exit=%lu existing=%d conflict=%d",
        registerExtension ? L"register" : L"unregister", ran ? 1 : 0, exitCode, alreadyRegistered?1:0, conflict?1:0);
    if (registrationConflict) *registrationConflict=conflict;
    return alreadyRegistered || (ran && exitCode == 0);
}

} // namespace GlassInstallUtil
