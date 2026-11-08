#include "GlassInstallUtil.h"
#include "GlassInstallLog.h"
#include "GlassMaintenanceCache.h"
#include "GlassEmptyDirectories.h"
#include <shlobj.h>
#include <wintrust.h>
#include <softpub.h>
#include <wincrypt.h>
#include <InjectModuleScan.h>
#include <algorithm>
#include <array>
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
        SameInstallPath(value,TrimTrailingSlashes(root)+L"\\"+kShellExtRelativePath);
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

namespace {
// Removes the key when no values and no subkeys remain. DeleteRegistryValue keeps
// empty keys for rollback fidelity; this runs at the end of an uninstall instead:
// the MSI packages leave HKCU\Software\GlassPlatform (Wow6432Node for the 32-bit
// package) behind after removing their RegistryValue component, and every
// install/uninstall cycle would recreate a stray empty key.
bool DeleteRegistryKeyIfEmpty(HKEY root, const wchar_t* subKey, REGSAM sam) noexcept {
    HKEY key=nullptr;
    LSTATUS status=RegOpenKeyExW(root,subKey,0,KEY_READ|sam,&key);
    if (status==ERROR_FILE_NOT_FOUND || status==ERROR_PATH_NOT_FOUND) return true;
    if (status!=ERROR_SUCCESS) { SetLastError(status); return false; }
    wchar_t name[2]{}; DWORD cch=2;
    const LSTATUS valueEnum=RegEnumValueW(key,0,name,&cch,nullptr,nullptr,nullptr,nullptr);
    cch=2;
    const LSTATUS keyEnum=RegEnumKeyExW(key,0,name,&cch,nullptr,nullptr,nullptr,nullptr);
    RegCloseKey(key);
    if (valueEnum!=ERROR_NO_MORE_ITEMS || keyEnum!=ERROR_NO_MORE_ITEMS) return true;
    status=RegDeleteKeyExW(root,subKey,sam,0);
    if (status!=ERROR_SUCCESS && status!=ERROR_FILE_NOT_FOUND && status!=ERROR_PATH_NOT_FOUND) {
        SetLastError(status); return false;
    }
    return true;
}

// Best-effort sweep of the install-path keys in every view the writers use.
void RemoveEmptyInstallPathKeys() noexcept {
    (void)DeleteRegistryKeyIfEmpty(HKEY_CURRENT_USER,L"Software\\GlassPlatform",KEY_WOW64_64KEY);
    (void)DeleteRegistryKeyIfEmpty(HKEY_CURRENT_USER,L"Software\\GlassPlatform",KEY_WOW64_32KEY);
    (void)DeleteRegistryKeyIfEmpty(HKEY_CURRENT_USER,L"Software\\Wow6432Node\\GlassPlatform",KEY_WOW64_64KEY);
    (void)DeleteRegistryKeyIfEmpty(HKEY_LOCAL_MACHINE,L"Software\\GlassPlatform",KEY_WOW64_64KEY);
    (void)DeleteRegistryKeyIfEmpty(HKEY_LOCAL_MACHINE,L"Software\\GlassPlatform",KEY_WOW64_32KEY);
}
} // namespace

} // namespace

namespace {
bool SignedPublisher(const std::wstring& path,std::array<BYTE,32>& digest) noexcept {
    WINTRUST_FILE_INFO file{sizeof(file)};file.pcwszFilePath=path.c_str();
    WINTRUST_DATA data{sizeof(data)};data.dwUIChoice=WTD_UI_NONE;data.fdwRevocationChecks=WTD_REVOKE_NONE;
    data.dwUnionChoice=WTD_CHOICE_FILE;data.pFile=&file;data.dwStateAction=WTD_STATEACTION_VERIFY;
    data.dwProvFlags=WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID policy=WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status=WinVerifyTrust(nullptr,&policy,&data);
    // A pinned self-signed Glass certificate is supported offline, but invalid
    // file digests/signatures, expiry, revocation and wrong usage are not.
    bool ok=status==ERROR_SUCCESS || status==CERT_E_UNTRUSTEDROOT || status==CERT_E_CHAINING;
    auto* provider=WTHelperProvDataFromStateData(data.hWVTStateData);
    auto* signer=provider?WTHelperGetProvSignerFromChain(provider,0,FALSE,0):nullptr;
    if(!signer || !signer->csCertChain || !signer->pasCertChain || !signer->pasCertChain[0].pCert)ok=false;
    if(ok) {
        const auto* cert=signer->pasCertChain[0].pCert;
        DWORD size=static_cast<DWORD>(digest.size());
        ok=CryptHashCertificate2(BCRYPT_SHA256_ALGORITHM,0,nullptr,cert->pbCertEncoded,cert->cbCertEncoded,digest.data(),&size) && size==digest.size();
    }
    data.dwStateAction=WTD_STATEACTION_CLOSE;WinVerifyTrust(nullptr,&policy,&data);
    return ok;
}
// Protect against renaming an admin-owned leaf through a user-writable ancestor.
// Ancestors may be owned/written by Windows servicing (TrustedInstaller), but
// installed MSI leaves/files permit the same privileged Windows servicing SID.
bool NativeAncestorsProtected(std::wstring path, bool directory=true) noexcept {
    if(path.size()<3 || path[1]!=L':' || path[2]!=L'\\')return false;
    BYTE admins[SECURITY_MAX_SID_SIZE]{},system[SECURITY_MAX_SID_SIZE]{};
    DWORD na=sizeof(admins),ns=sizeof(system);PSID servicing=nullptr;
    if(!CreateWellKnownSid(WinBuiltinAdministratorsSid,nullptr,admins,&na) ||
       !CreateWellKnownSid(WinLocalSystemSid,nullptr,system,&ns) ||
       !ConvertStringSidToSidW(L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464",&servicing))return false;
    const auto privileged=[&](PSID sid){return sid && (EqualSid(sid,admins)||EqualSid(sid,system)||EqualSid(sid,servicing));};
    bool ok=true;
    while(ok) {
        const DWORD attr=GetFileAttributesW(path.c_str());
        if(attr==INVALID_FILE_ATTRIBUTES || ((attr&FILE_ATTRIBUTE_DIRECTORY)!=0)!=directory || (attr&FILE_ATTRIBUTE_REPARSE_POINT)){
            GlassInstallLog::LineFormat(L"Native trust: invalid type, missing or reparse path: %s",path.c_str());
            ok=false;break;
        }
        PSID owner=nullptr;PACL acl=nullptr;PSECURITY_DESCRIPTOR sd=nullptr;
        const DWORD security=GetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()),SE_FILE_OBJECT,
            OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,&owner,nullptr,&acl,nullptr,&sd);
        if(security!=ERROR_SUCCESS){
            GlassInstallLog::LineFormat(L"Native trust: GetNamedSecurityInfoW failed (%lu): %s",security,path.c_str());
            if(sd)LocalFree(sd);
            ok=false;break;
        }
        ok=privileged(owner) && acl;
        for(DWORD i=0;ok && i<acl->AceCount;++i) {
            void* raw=nullptr;if(!GetAce(acl,i,&raw)){ok=false;break;}
            const auto* header=static_cast<ACE_HEADER*>(raw);
            if(header->AceFlags&INHERIT_ONLY_ACE)continue;
            if(header->AceType==ACCESS_DENIED_ACE_TYPE)continue;
            if(header->AceType!=ACCESS_ALLOWED_ACE_TYPE){ok=false;break;}
            auto* ace=static_cast<ACCESS_ALLOWED_ACE*>(raw);
            // For directories, creating a new subdirectory alone cannot replace
            // this already-existing pinned DLL. In particular C:\ may grant
            // FILE_ADD_SUBDIRECTORY to users. Mutating/deleting an existing
            // child, or changing its ACL/owner, must remain privileged.
            const DWORD replace=GENERIC_ALL|GENERIC_WRITE|DELETE|WRITE_DAC|WRITE_OWNER|FILE_DELETE_CHILD|FILE_WRITE_DATA|
                (directory?0u:(FILE_APPEND_DATA|FILE_WRITE_EA|FILE_WRITE_ATTRIBUTES));
            if((ace->Mask&replace) && !privileged(&ace->SidStart)) {
                GlassInstallLog::LineFormat(L"Native trust: non-privileged write ACE mask=0x%08lX: %s",ace->Mask,path.c_str());
                ok=false;
            }
        }
        if(!ok)GlassInstallLog::LineFormat(L"Native trust: owner/DACL validation failed: %s",path.c_str());
        LocalFree(sd);
        if(path.size()==3)break;
        const auto slash=path.find_last_of(L'\\');if(slash==std::wstring::npos){ok=false;break;}
        path.resize(slash==2?3:slash);
        directory=true;
    }
    LocalFree(servicing);return ok;
}

bool TrustedInstalledNative(const std::wstring& path,const std::wstring& root) noexcept {
    const auto parent=TrimTrailingSlashes(root)+L"\\GlassSettings";
    if(path!=parent+L"\\GlassSettings.Native.dll")return false;
    // Installed MSI directories retain normal Windows ACLs: TrustedInstaller
    // and inherit-only CREATOR OWNER ACEs are legitimate here. Do not apply the
    // private ProgramData cache's Administrators/SYSTEM-only policy, and never
    // rewrite MSI-owned ACLs just to make the executable pass validation.
    if(!NativeAncestorsProtected(path,false))return false;
    // Pin the file while validating. Installed roots/files are admin-only; no
    // executable is copied/extracted to Temp or loaded from a writable source.
    HANDLE pin=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(pin==INVALID_HANDLE_VALUE) {
        GlassInstallLog::LineFormat(L"Native trust: cannot pin DLL for reading (%lu): %s",GetLastError(),path.c_str());
        return false;
    }
    wchar_t self[32768]{};const DWORD n=GetModuleFileNameW(nullptr,self,_countof(self));
    std::array<BYTE,32> expected{},actual{};
    const bool ok=n && n<_countof(self) && SignedPublisher(self,expected) && SignedPublisher(path,actual) && expected==actual;
    CloseHandle(pin);
    if(!ok)GlassInstallLog::LineFormat(L"Native trust: Setup/DLL Authenticode publisher verification failed: %s",path.c_str());
    return ok;
}
} // namespace

std::wstring InstalledNativeDllX86() noexcept {
    InstallRoots roots;if(!ResolveInstallRoots(roots))return {};
    return NativeDllForArch(GlassShared::GlassPlatformTask::PlatformArch::X86,roots.x64,roots.x86);
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
    if (!instance || resourceId == 0 || destPath.empty()) {
        SetLastError(ERROR_INVALID_PARAMETER);return false;
    }
    HRSRC res = FindResourceW(instance, MAKEINTRESOURCEW(resourceId), resourceType);
    if (!res)
        return false;
    const DWORD size = SizeofResource(instance, res);
    if (size == 0) {SetLastError(ERROR_RESOURCE_DATA_NOT_FOUND);return false;}
    HGLOBAL loaded = LoadResource(instance, res);
    if (!loaded)
        return false;
    const void* bytes = LockResource(loaded);
    if (!bytes) {SetLastError(ERROR_RESOURCE_DATA_NOT_FOUND);return false;}
    HANDLE file = CreateFileW(destPath.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD offset=0;
    DWORD failure=ERROR_SUCCESS;
    const auto* data=static_cast<const BYTE*>(bytes);
    while (offset<size) {
        if (cancelled && cancelled->load(std::memory_order_acquire)) { failure=ERROR_CANCELLED; break; }
        const DWORD chunk=(size-offset)>1024*1024 ? 1024*1024 : size-offset;
        DWORD written=0;
        if (!WriteFile(file,data+offset,chunk,&written,nullptr)) { failure=GetLastError();break; }
        if(written==0){failure=ERROR_WRITE_FAULT;break;}
        offset+=written;
    }
    if(!CloseHandle(file)&&failure==ERROR_SUCCESS)failure=GetLastError();
    if(failure!=ERROR_SUCCESS||offset!=size) {
        if(failure==ERROR_SUCCESS)failure=ERROR_WRITE_FAULT;
        DeleteFileW(destPath.c_str());SetLastError(failure);return false;
    }
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

bool IsWindows11OrGreater() noexcept {
    // RtlGetVersion is not subject to the manifest-based GetVersionEx lie.
    using RtlGetVersionFn = LONG (WINAPI*)(PRTL_OSVERSIONINFOW);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto fn = ntdll ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
    if (!fn) return false;
    RTL_OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (fn(&vi) != 0) return false;
    return vi.dwMajorVersion > 10 || (vi.dwMajorVersion == 10 && vi.dwBuildNumber >= 22000);
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

// Unknown image identity fails closed. Never send WM_CLOSE by basename alone.
namespace {
bool VisitSelectedSettings(const std::wstring& root64,const std::wstring& root32,
    unsigned mask,bool close,bool& running,const std::atomic_bool* cancelled) {
    running=false;
    if(!(mask&36u))return true;
    if(((mask&4u)&&root64.empty()) || ((mask&32u)&&root32.empty())) {SetLastError(ERROR_INVALID_PARAMETER);return false;}
    const auto exe64=TrimTrailingSlashes(root64)+L"\\GlassSettings\\GlassSettings.exe";
    const auto exe32=TrimTrailingSlashes(root32)+L"\\GlassSettings\\GlassSettings.exe";
    const HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snap==INVALID_HANDLE_VALUE)return false;
    PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);bool ok=true;DWORD error=ERROR_SUCCESS;
    if(Process32FirstW(snap,&entry)) {
        do {
            if(cancelled&&cancelled->load(std::memory_order_acquire)){ok=false;error=ERROR_CANCELLED;break;}
            if(_wcsicmp(entry.szExeFile,L"GlassSettings.exe")!=0)continue;
            const HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,entry.th32ProcessID);
            if(!process) {
                error=GetLastError();
                if(error==ERROR_INVALID_PARAMETER)continue; // Process exited after snapshot.
                ok=false;break;
            }
            const DWORD wait=WaitForSingleObject(process,0);
            if(wait==WAIT_OBJECT_0){CloseHandle(process);continue;}
            wchar_t path[32768]{};DWORD length=static_cast<DWORD>(std::size(path));
            if(wait!=WAIT_TIMEOUT || !QueryFullProcessImageNameW(process,0,path,&length)) {
                error=GetLastError();CloseHandle(process);ok=false;break;
            }
            const bool selected=((mask&4u)&&SameInstallPath(path,exe64)) || ((mask&32u)&&SameInstallPath(path,exe32));
            if(selected) {
                running=true;
                if(close)EnumWindows(CloseWindowsForPid,static_cast<LPARAM>(entry.th32ProcessID));
            }
            CloseHandle(process);
        } while(Process32NextW(snap,&entry));
        if(ok&&GetLastError()!=ERROR_NO_MORE_FILES){ok=false;error=GetLastError();}
    } else if(GetLastError()!=ERROR_NO_MORE_FILES){ok=false;error=GetLastError();}
    CloseHandle(snap);
    if(!ok)SetLastError(error?error:ERROR_GEN_FAILURE);
    return ok;
}
}

bool QuerySelectedSettingsRunning(const std::wstring& root64,const std::wstring& root32,
    unsigned mask,bool& running) noexcept {
    try{return VisitSelectedSettings(root64,root32,mask,false,running,nullptr);}
    catch(...){running=false;SetLastError(ERROR_NOT_ENOUGH_MEMORY);return false;}
}

bool CloseSelectedSettingsProcesses(const std::wstring& root64,const std::wstring& root32,
    unsigned mask,const std::atomic_bool* cancelled) noexcept {
    try {
        bool running=false;
        if(!VisitSelectedSettings(root64,root32,mask,true,running,cancelled))return false;
        const ULONGLONG start=GetTickCount64();
        while(running) {
            if(cancelled&&cancelled->load(std::memory_order_acquire)){SetLastError(ERROR_CANCELLED);return false;}
            if(GetTickCount64()-start>=8000){SetLastError(ERROR_TIMEOUT);return false;}
            Sleep(200);
            if(!VisitSelectedSettings(root64,root32,mask,false,running,cancelled))return false;
        }
        return true;
    }catch(...){SetLastError(ERROR_NOT_ENOUGH_MEMORY);return false;}
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

bool PeBufferMatches(const void* data, size_t length, WORD machine, const char* exportName) noexcept {
    if (!data || length<512 || length>64u*1024*1024) return false;
    const auto* base=static_cast<const BYTE*>(data);
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
    return ok;
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
    const bool ok=PeBufferMatches(base,length,machine,exportName);
    UnmapViewOfFile(base);
    return ok;
}

bool ResourcePeMatches(HINSTANCE instance, UINT resourceId, WORD machine, const char* exportName) noexcept {
    // RCDATA contains the raw on-disk PE layout (not an executable image).
    // Do not extract it, load a DLL, or consult any installed Settings path.
    if (!instance || !resourceId) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    HRSRC resource=FindResourceW(instance,MAKEINTRESOURCEW(resourceId),RT_RCDATA);
    if (!resource) return false;
    const DWORD length=SizeofResource(instance,resource);
    if (length<512 || length>64u*1024*1024) { SetLastError(ERROR_INVALID_DATA); return false; }
    HGLOBAL loaded=LoadResource(instance,resource);
    if (!loaded) return false;
    const void* data=LockResource(loaded);
    if (!data) { SetLastError(ERROR_RESOURCE_DATA_NOT_FOUND); return false; }
    if (!PeBufferMatches(data,length,machine,exportName)) { SetLastError(ERROR_BAD_EXE_FORMAT); return false; }
    return true;
}

namespace {
std::wstring InstalledSecureUxThemeNativeDll(
    GlassShared::GlassPlatformTask::PlatformArch arch,
    const std::wstring& x64Dir,const std::wstring& x86Dir,const char* exportName) noexcept {
    const bool x64=arch==GlassShared::GlassPlatformTask::PlatformArch::X64;
    const WORD machine=x64?IMAGE_FILE_MACHINE_AMD64:IMAGE_FILE_MACHINE_I386;
    const std::wstring& root=x64?x64Dir:x86Dir;
    if(root.empty())return {};
    const std::wstring dll=TrimTrailingSlashes(root)+L"\\GlassSettings\\GlassSettings.Native.dll";
    if(!PeMatches(dll,machine,exportName)) {
        GlassInstallLog::LineFormat(L"Native helper: file, architecture or required export %S failed: %s",exportName,dll.c_str());
        return {};
    }
    return TrustedInstalledNative(dll,root)?dll:std::wstring();
}
}

std::wstring NativeDllForArch(
    GlassShared::GlassPlatformTask::PlatformArch arch,
    const std::wstring& x64Dir,
    const std::wstring& x86Dir) noexcept {
    const auto dll=InstalledSecureUxThemeNativeDll(arch,x64Dir,x86Dir,"GlassSettings_UnlockerW");
    const WORD machine=arch==GlassShared::GlassPlatformTask::PlatformArch::X64?IMAGE_FILE_MACHINE_AMD64:IMAGE_FILE_MACHINE_I386;
    if(!dll.empty() && !PeMatches(dll,machine,Shared::InjectModuleScan::kNativeInstallerProtocolExport)) {
        GlassInstallLog::LineFormat(L"Native helper: required installer scan protocol export missing: %s",dll.c_str());
        return {};
    }
    return dll;
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
    // Installation facts are machine-wide, independent of the interactive account.
    auto status=read(HKEY_LOCAL_MACHINE,L"Software\\GlassPlatform",explicitValue,machineView,
        x64?L"HKLM(64) InstallPathX64":L"HKLM(32) InstallPathX86");
    if (status!=RegistryPathRead::Missing) return status;
    status=read(HKEY_LOCAL_MACHINE,L"Software\\GlassPlatform",L"InstallPath",machineView,
        x64?L"HKLM(64) InstallPath":L"HKLM(32) InstallPath");
    if (status!=RegistryPathRead::Missing) return status;
    return RegistryPathRead::Missing;
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
    return RegistryPathRead::Missing;
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
        if (!WriteRegSzIfMissing(HKEY_LOCAL_MACHINE,L"Software\\GlassPlatform",explicitVal,lmSam,dir) ||
            !WriteRegSzIfMissing(HKEY_LOCAL_MACHINE,L"Software\\GlassPlatform",L"InstallPath",lmSam,dir)) return false;
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
    bool ok=true;
    if (x64 == IsOs64Bit()) {
        ok=DeleteRegistryValue(HKEY_CURRENT_USER, L"Software\\GlassPlatform", L"InstallPath", 0);
    } else if (!x64 && IsOs64Bit()) {
        ok=DeleteRegistryValue(HKEY_CURRENT_USER, L"Software\\Wow6432Node\\GlassPlatform", L"InstallPath", 0);
    }
    // Uninstalling the MSI removes its RegistryValue but leaves the key; drop the
    // empty install-path keys so no stray HKCU\Software\Wow6432Node\GlassPlatform lingers.
    RemoveEmptyInstallPathKeys();
    return ok;
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
        if (!WriteRegSzIfMissing(HKEY_LOCAL_MACHINE,L"Software\\StanislavKa\\GlassPlatform",L"InstallPath",KEY_WOW64_64KEY,root) ||
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

namespace {

// OpMessageId of GlassSettings/Native/ShellExtensionOps.cpp as technical text.
const wchar_t* ShellHelperReasonText(DWORD id) noexcept {
    switch (id) {
    case 7: return L"sparse package artifacts are missing";
    case 8: return L"the msix signer certificate is not trusted on this computer";
    case 9: return L"package deployment failed";
    case 10: return L"registration failed";
    case 11: return L"unregistration failed";
    case 12: return L"external location conflict";
    default: return nullptr;
    }
}

// Technical description of a failed GlassSettings_ShellExtRegisterSetupV1W run.
// Mirrors SetupHelperFailureExitCode() in GlassSettings/Native/ShellExtensionOps.cpp:
// 0x8xxxxxxx = HRESULT of the deployment call, 0x47500000|id = OpMessageId
// without an HRESULT, 1 = generic failure. 0x47504C00|id (classic fallback)
// is a success code and is handled by the caller.
std::wstring DescribeShellHelperExit(DWORD exitCode, bool started, bool completed) {
    if (!started) return L"helper process could not be started";
    if (!completed) return L"helper process did not finish within 45 s";
    if ((exitCode & 0xFFFFFF00u)==0x47500000u) {
        const wchar_t* reason=ShellHelperReasonText(exitCode & 0xFFu);
        wchar_t text[160]{};
        if (reason) swprintf_s(text,L"helper reported: %s (0x%08lX)",reason,exitCode);
        else swprintf_s(text,L"helper exit code 0x%08lX",exitCode);
        return text;
    }
    if (exitCode & 0x80000000u) {
        wchar_t message[512]{};
        const DWORD len=FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,nullptr,exitCode,0,
            message,static_cast<DWORD>(std::size(message)),nullptr);
        std::wstring detail=L"HRESULT 0x";
        wchar_t hex[16]{};
        swprintf_s(hex,L"%08lX",exitCode);
        detail+=hex;
        if (len) {
            std::wstring system(message,len);
            while (!system.empty() && (system.back()==L'\r' || system.back()==L'\n' || system.back()==L' ')) system.pop_back();
            if (!system.empty()) detail+=L" - "+system;
        }
        return detail;
    }
    wchar_t text[64]{};
    swprintf_s(text,L"helper exit code %lu",exitCode);
    return text;
}

} // namespace

std::wstring DescribeShellClassicFallback(DWORD reason) {
    const wchar_t* text=ShellHelperReasonText(reason);
    wchar_t buffer[200]{};
    if (text) swprintf_s(buffer,L"classic menu registered; sparse package: %s (0x%08lX)",text,0x47504C00u|(reason & 0xFFu));
    else swprintf_s(buffer,L"classic menu registered; sparse package failed (0x%08lX)",0x47504C00u|(reason & 0xFFu));
    return buffer;
}

bool InvokeShellExtRegistration(
    bool registerExtension,
    const std::wstring& x64Dir,
    const std::wstring& x86Dir,
    bool* registrationMayHaveChanged,
    bool* registrationConflict,
    std::wstring* failureDetail,
    DWORD* classicFallbackReason) noexcept {
    if (registrationMayHaveChanged) *registrationMayHaveChanged=false;
    if (registrationConflict) *registrationConflict=false;
    if (classicFallbackReason) *classicFallbackReason=0;
    auto fail=[failureDetail](const wchar_t* detail) noexcept {
        if (failureDetail) {
            try { *failureDetail=detail; } catch (...) {}
        }
        return false;
    };
    const bool os64 = IsOs64Bit();
    const auto arch = os64
        ? GlassShared::GlassPlatformTask::PlatformArch::X64
        : GlassShared::GlassPlatformTask::PlatformArch::X86;
    const std::wstring& selectedRoot=os64 ? x64Dir : x86Dir;
    const std::wstring root=TrimTrailingSlashes(selectedRoot);
    if (root.empty() || root.find(L'"')!=std::wstring::npos || !IsSafeInstallPath(root))
        return fail(L"install root is empty or unsafe");
    const std::wstring dll = NativeDllForArch(arch, x64Dir, x86Dir);
    const WORD machine=os64?IMAGE_FILE_MACHINE_AMD64:IMAGE_FILE_MACHINE_I386;
    if (dll.empty() || !PeMatches(dll,machine,kInstallerShellExport)) {
        // Windows 11 requires the sparse package; do not silently substitute legacy regsvr32.
        // On 32-bit Windows 10, legacy registration is the supported fallback.
        if (os64) return fail(L"Installed GlassSettings.Native.dll is missing or failed architecture, export, ACL or publisher verification; see Native helper/trust log entries");
        const std::wstring shellExt = root + L"\\" + GlassShared::GlassShellMenu::kShellExtRelativePath;
        if (!IsSafeInstallPath(shellExt) || !PathFileExistsW(shellExt.c_str())) return fail(L"GlassShellSparse\\GlassShellExt.dll was not found");
        if (HasExistingShellRegistrationKeys()) {
            const bool matches=HasMatchingLegacyShellRegistration(root);
            if (!matches) {
                GlassInstallLog::Line(L"ShellExt: existing legacy registration does not match the selected root");
                if (registrationConflict) *registrationConflict=true;
                return fail(L"existing legacy registration points to another folder");
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
        GlassInstallLog::LineFormat(L"ShellExt %s via regsvr32: started=%d exit=%lu",
            registerExtension ? L"register" : L"unregister", started ? 1 : 0, exitCode);
        if (!result) {
            if (!started) return fail(L"regsvr32 could not be started");
            wchar_t detail[64]{};
            swprintf_s(detail,L"regsvr32 exit code %lu",exitCode);
            return fail(detail);
        }
        return true;
    }
    const std::wstring rundll = os64 ? NativeRundll64() : RundllPathForTaskArch(arch);
    if (rundll.empty()) return fail(L"rundll32.exe was not found");
    constexpr char reportExport[]="GlassSettings_ShellExtUnregisterReportSetupV1W";
    const bool reportsNoop=!registerExtension && PeMatches(dll,machine,reportExport);
    std::wstring cmd = L"\"" + rundll + L"\" \"" + dll + L"\","+
        (reportsNoop?L"GlassSettings_ShellExtUnregisterReportSetupV1W ":L"GlassSettings_ShellExtRegisterSetupV1W ");
    cmd += registerExtension ? L"/register" : L"/unregister";
    cmd += L" /installroot \"" + root + L"\"";
    DWORD exitCode = 1;
    bool started=false,completed=false;
    const bool ran = RunHiddenCommand(cmd, &exitCode, 45000, &started, &completed);
    constexpr DWORD kShellAlreadyRegistered=0x47505245; // Verified existing registration.
    constexpr DWORD kShellRegistrationConflict=0x47504346; // Preserved, no change made.
    constexpr DWORD kShellClassicFallbackBase=0x47504C00; // Registered the classic menu; low byte = reason.
    constexpr DWORD kShellNothingToUnregister=0x47504E4Fu;
    const bool alreadyRegistered=completed && ((registerExtension && exitCode==kShellAlreadyRegistered)||
        (!registerExtension && reportsNoop && exitCode==kShellNothingToUnregister));
    const bool conflict=completed && exitCode==kShellRegistrationConflict;
    const bool classic=registerExtension && completed && (exitCode & 0xFFFFFF00u)==kShellClassicFallbackBase;
    if (registrationMayHaveChanged) *registrationMayHaveChanged=started && !alreadyRegistered && !conflict;
    GlassInstallLog::LineFormat(L"ShellExt %s via Native.dll: ran=%d exit=0x%08lX existing=%d conflict=%d classic=%d",
        registerExtension ? L"register" : L"unregister", ran ? 1 : 0, exitCode, alreadyRegistered?1:0, conflict?1:0, classic?1:0);
    if (registrationConflict) *registrationConflict=conflict;
    if (classic) {
        const DWORD reason=exitCode & 0xFFu;
        if (classicFallbackReason) *classicFallbackReason=reason ? reason : 10u;
        GlassInstallLog::LineFormat(L"ShellExt: %s", DescribeShellClassicFallback(reason).c_str());
        return true;
    }
    if (alreadyRegistered || (ran && exitCode == 0)) return true;
    if (conflict) return fail(L"an existing or unverifiable registration was preserved");
    const std::wstring detail=DescribeShellHelperExit(exitCode,started,completed);
    GlassInstallLog::LineFormat(L"ShellExt %s failed: %s", registerExtension ? L"register" : L"unregister", detail.c_str());
    return fail(detail.c_str());
}

bool InvokeShellSignerTrust(
    const std::wstring& x64Dir,
    const std::wstring& x86Dir,
    std::wstring* failureDetail) noexcept {
    auto fail=[failureDetail](const wchar_t* detail) noexcept {
        if (failureDetail) {
            try { *failureDetail=detail; } catch (...) {}
        }
        return false;
    };
    if (!IsOs64Bit() || !IsWindows11OrGreater()) return fail(L"not applicable: the author certificate is used only on Windows 11 x64");
    const std::wstring root=TrimTrailingSlashes(x64Dir);
    if (root.empty() || root.find(L'"')!=std::wstring::npos || !IsSafeInstallPath(root))
        return fail(L"install root is empty or unsafe");
    const std::wstring dll = NativeDllForArch(GlassShared::GlassPlatformTask::PlatformArch::X64, x64Dir, x86Dir);
    if (dll.empty() || !PeMatches(dll,IMAGE_FILE_MACHINE_AMD64,kInstallerSignerTrustExport))
        return fail(L"GlassSettings.Native.dll with the certificate export was not found");
    const std::wstring rundll = NativeRundll64();
    if (rundll.empty()) return fail(L"rundll32.exe was not found");
    const std::wstring cmd = L"\"" + rundll + L"\" \"" + dll + L"\",GlassSettings_ShellSignerTrustSetupV1W /installroot \"" + root + L"\"";
    DWORD exitCode = 1;
    bool started=false,completed=false;
    const bool ran = RunHiddenCommand(cmd, &exitCode, 45000, &started, &completed);
    GlassInstallLog::LineFormat(L"Certificate: trust msix signer via Native.dll: ran=%d exit=0x%08lX", ran ? 1 : 0, exitCode);
    if (ran && completed && exitCode == 0) return true;
    if (!started) return fail(L"helper process could not be started");
    if (!completed) return fail(L"helper process did not finish within 45 s");
    // 0x47535400 | SparseSignerTrustResult (GlassSettings/Native/ShellSparsePackage.h).
    constexpr DWORD kTrustFailureBase=0x47535400u;
    if ((exitCode & 0xFFFFFF00u)==kTrustFailureBase) {
        const wchar_t* reason=nullptr;
        switch (exitCode & 0xFFu) {
        case 2: reason=L"GlassShellSparse.msix was not found"; break;
        case 3: reason=L"GlassShellSparse.msix is not signed"; break;
        case 4: reason=L"the msix signer is not the author certificate; nothing was trusted"; break;
        case 5: reason=L"LocalMachine\\TrustedPeople could not be opened or written"; break;
        default: reason=L"the signer certificate could not be read"; break;
        }
        wchar_t text[200]{};
        swprintf_s(text,L"helper reported: %s (0x%08lX)",reason,exitCode);
        return fail(text);
    }
    wchar_t text[64]{};
    swprintf_s(text,L"helper exit code 0x%08lX",exitCode);
    return fail(text);
}

bool InvokeSecureUxThemeDeactivate(
    const std::wstring& x64Dir,
    const std::wstring& x86Dir,
    SecureUxThemeDeactivateRun* run,
    std::wstring* failureDetail) noexcept {
    auto fail=[failureDetail](const wchar_t* detail) noexcept {
        if (failureDetail) {
            try { *failureDetail=detail; } catch (...) {}
        }
        return false;
    };
    if (run) *run=SecureUxThemeDeactivateRun{};
    // The deactivation entry refuses WOW64, so the helper must match the OS bitness:
    // the embedded x64 helper on x64, the installed x86 helper on 32-bit Windows.
    const bool os64 = IsOs64Bit();
    const auto arch = os64
        ? GlassShared::GlassPlatformTask::PlatformArch::X64
        : GlassShared::GlassPlatformTask::PlatformArch::X86;
    const std::wstring dll = InstalledSecureUxThemeNativeDll(arch,x64Dir,x86Dir,kInstallerSecureUxThemeExport);
    const WORD machine=os64?IMAGE_FILE_MACHINE_AMD64:IMAGE_FILE_MACHINE_I386;
    if (dll.empty() || !PeMatches(dll,machine,kInstallerSecureUxThemeExport))
        return fail(L"GlassSettings.Native.dll with the SecureUxTheme export was not found");
    const std::wstring rundll = os64 ? NativeRundll64() : RundllPathForTaskArch(arch);
    if (rundll.empty()) return fail(L"rundll32.exe was not found");
    const std::wstring cmd = L"\"" + rundll + L"\" \"" + dll + L"\",GlassSettings_SecureUxThemeSetupV1W";
    DWORD exitCode = 1;
    bool started=false,completed=false;
    const bool ran = RunHiddenCommand(cmd,&exitCode,45000,&started,&completed);
    GlassInstallLog::LineFormat(L"SecureUxTheme: deactivate via Native.dll: ran=%d exit=0x%08lX",
        ran?1:0,exitCode);
    if (!ran) return fail(L"helper process could not be started");
    if (!completed) return fail(L"helper process did not finish within 45 s");
    // Bit 0 (bad arguments) and bit 3 (deactivation failed) are hard failures; the
    // remaining bits are outcomes of a completed deactivation.
    if ((exitCode & (1u|kSecureUxThemeExitDeactivateFailed)) != 0) {
        wchar_t text[64]{};
        swprintf_s(text,L"helper exit code 0x%08lX",exitCode);
        return fail(text);
    }
    if (run) {
        run->ran=true;
        run->rebootRequired=(exitCode & kSecureUxThemeExitRebootRequired)!=0;
        run->themeNotRestored=(exitCode & kSecureUxThemeExitThemeNotRestored)!=0;
        run->skippedExternalInstall=(exitCode & kSecureUxThemeExitExternalInstall)!=0;
    }
    return true;
}

bool QuerySecureUxPendingFileOperations(bool& pending) noexcept {
    pending=true; // Unreadable/invalid data must not suppress an MSI reboot request.
    const auto fail=[](const wchar_t* reason) {
        GlassInstallLog::LineFormat(L"SecureUxTheme pending file operations: unverified (%s)",reason);return false;
    };
    try {
        constexpr auto key=L"SYSTEM\\CurrentControlSet\\Control\\Session Manager";
        constexpr auto value=L"PendingFileRenameOperations";
        DWORD bytes=0;
        LONG rc=RegGetValueW(HKEY_LOCAL_MACHINE,key,value,RRF_RT_REG_MULTI_SZ,nullptr,nullptr,&bytes);
        if(rc==ERROR_FILE_NOT_FOUND){pending=false;GlassInstallLog::Line(L"SecureUxTheme pending file operations: none (value absent)");return true;}
        if(rc!=ERROR_SUCCESS || bytes<2*sizeof(wchar_t) || bytes>1024*1024 || bytes%sizeof(wchar_t))return fail(L"registry size/type/access");
        std::vector<wchar_t> data(bytes/sizeof(wchar_t));
        rc=RegGetValueW(HKEY_LOCAL_MACHINE,key,value,RRF_RT_REG_MULTI_SZ,nullptr,data.data(),&bytes);
        if(rc!=ERROR_SUCCESS || bytes<2*sizeof(wchar_t) || bytes%sizeof(wchar_t) || bytes/sizeof(wchar_t)>data.size())return fail(L"registry read/size changed");
        const auto count=bytes/sizeof(wchar_t);
        if(data[count-1]!=0 || data[count-2]!=0)return fail(L"unterminated queue");
        wchar_t windows[32768]{};const UINT length=GetSystemWindowsDirectoryW(windows,32768);
        if(!length || length>=32768)return fail(L"Windows directory unavailable");
        std::wstring root(windows,length);while(!root.empty()&&root.back()==L'\\')root.pop_back();
        const auto relevant=[&](std::wstring path) {
            // Session Manager uses NT DOS paths and an optional overwrite marker.
            if(!path.empty()&&path.front()==L'!')path.erase(0,1);
            if(path.rfind(L"\\??\\",0)==0 || path.rfind(L"\\?\\",0)==0)path.erase(0,4);
            if(_wcsnicmp(path.c_str(),L"\\SystemRoot\\",12)==0)path=root+L"\\"+path.substr(12);
            for(const wchar_t* folder:{L"\\System32\\",L"\\SysWOW64\\"})
                for(const wchar_t* file:{L"SecureUxTheme4.dll",L"ThemeUiProxy.dll"})
                    if(_wcsicmp(path.c_str(),(root+folder+file).c_str())==0)return true;
            // A non-DOS namespace/short path cannot safely prove unrelatedness
            // for a named SUT file. Keep its reboot request conservatively.
            const auto slash=path.find_last_of(L"\\/");const auto name=path.substr(slash==std::wstring::npos?0:slash+1);
            const bool sutName=_wcsicmp(name.c_str(),L"SecureUxTheme4.dll")==0 || _wcsicmp(name.c_str(),L"ThemeUiProxy.dll")==0;
            return path.find(L'~')!=std::wstring::npos || (sutName && (path.size()<3 || path[1]!=L':' ||
                path.find(L"\\..\\")!=std::wstring::npos || path.find(L"\\.\\")!=std::wstring::npos));
        };
        std::size_t at=0,pairs=0;bool found=false;
        while(at<count && data[at]!=0) {
            const std::wstring source(data.data()+at);at+=source.size()+1;
            if(at>=count)return fail(L"missing destination");
            const std::wstring destination(data.data()+at);at+=destination.size()+1;++pairs;
            if(relevant(source)||relevant(destination)) {
                found=true;
                GlassInstallLog::LineFormat(L"SecureUxTheme pending file operation: %s -> %s",source.c_str(),destination.c_str());
            }
        }
        if(at>=count || std::any_of(data.begin()+at,data.begin()+count,[](wchar_t c){return c!=0;}))return fail(L"invalid queue tail");
        pending=found;
        GlassInstallLog::LineFormat(L"SecureUxTheme pending file operations: read=ok pairs=%u relevant=%d",static_cast<unsigned>(pairs),found?1:0);
        return true;
    }catch(...){return fail(L"exception");}
}

bool QuerySecureUxThemeState(const std::wstring& x64Dir,const std::wstring& x86Dir,
    SecureUxThemeState* state,std::wstring* failureDetail) noexcept {
    auto fail=[failureDetail](const wchar_t* detail) noexcept { if(failureDetail)try{*failureDetail=detail;}catch(...){} return false; };
    if(state)*state=SecureUxThemeState::Conflict;
    const bool os64=IsOs64Bit();
    const auto arch=os64?GlassShared::GlassPlatformTask::PlatformArch::X64:GlassShared::GlassPlatformTask::PlatformArch::X86;
    const std::wstring dll=NativeDllForArch(arch,x64Dir,x86Dir);
    if(dll.empty()||!PeMatches(dll,os64?IMAGE_FILE_MACHINE_AMD64:IMAGE_FILE_MACHINE_I386,kInstallerSecureUxThemeQueryExport))
        return fail(L"embedded GlassSettings.Native.dll query helper was not found");
    const std::wstring rundll=os64?NativeRundll64():RundllPathForTaskArch(arch);
    if(rundll.empty())return fail(L"native rundll32.exe was not found");
    const std::wstring cmd=L"\""+rundll+L"\" \""+dll+L"\",GlassSettings_SecureUxThemeQueryStateV1W";
    DWORD exitCode=MAXDWORD;bool started=false,completed=false;
    const bool ran=RunHiddenCommand(cmd,&exitCode,20000,&started,&completed);
    GlassInstallLog::LineFormat(L"SecureUxTheme: query via Native.dll: ran=%d exit=%lu",ran?1:0,exitCode);
    if(!ran||!started)return fail(L"query helper process could not be started");
    if(!completed)return fail(L"query helper did not finish within 20 s");
    if(exitCode>static_cast<DWORD>(SecureUxThemeState::Conflict))return fail(L"query helper returned an unknown state");
    if(state)*state=static_cast<SecureUxThemeState>(exitCode);
    return true;
}

std::wstring SecureUxThemeInstalledFileRoot(bool x64Product) noexcept {
    wchar_t buffer[MAX_PATH + 1]{};
    if (x64Product) {
        if (GetSystemDirectoryW(buffer, MAX_PATH) == 0)
            return {};
        return buffer;
    }
    if (IsOs64Bit()) {
        if (GetSystemWow64DirectoryW(buffer, MAX_PATH) == 0)
            return {};
        return buffer;
    }
    if (GetSystemDirectoryW(buffer, MAX_PATH) == 0)
        return {};
    return buffer;
}

bool InvokeSecureUxThemeActivate(const std::wstring& x64Dir,const std::wstring& x86Dir,
    SecureUxThemeActivateRun* run,std::wstring* failureDetail) noexcept {
    auto fail=[failureDetail](const wchar_t* detail) noexcept { if(failureDetail)try{*failureDetail=detail;}catch(...){} return false; };
    if(run)*run=SecureUxThemeActivateRun{};
    const bool os64=IsOs64Bit();
    const auto arch=os64?GlassShared::GlassPlatformTask::PlatformArch::X64:GlassShared::GlassPlatformTask::PlatformArch::X86;
    const std::wstring dll=InstalledSecureUxThemeNativeDll(arch,x64Dir,x86Dir,kInstallerSecureUxThemeActivateExport);
    if(dll.empty())return fail(L"installed GlassSettings.Native.dll activation helper was not found");
    const std::wstring rundll=os64?NativeRundll64():RundllPathForTaskArch(arch);
    if(rundll.empty())return fail(L"native rundll32.exe was not found");
    const std::wstring cmd=L"\""+rundll+L"\" \""+dll+L"\",GlassSettings_SecureUxThemeActivateSetupV1W";
    DWORD exitCode=MAXDWORD;bool started=false,completed=false;
    const bool ran=RunHiddenCommand(cmd,&exitCode,45000,&started,&completed);
    GlassInstallLog::LineFormat(L"SecureUxTheme: activate via Native.dll: ran=%d exit=%lu",ran?1:0,exitCode);
    if(!ran||!started)return fail(L"activation helper process could not be started");
    if(!completed)return fail(L"activation helper did not finish within 45 s");
    if(exitCode==kSecureUxThemeExitExternalInstall){if(run){run->ran=true;run->skippedExternalInstall=true;}return true;}
    if(exitCode!=0&&exitCode!=kSecureUxThemeExitRebootRequired){
        wchar_t text[80]{};swprintf_s(text,L"activation helper exit code 0x%08lX",exitCode);return fail(text);
    }
    if(run){run->ran=true;run->rebootRequired=exitCode==kSecureUxThemeExitRebootRequired;}
    return true;
}

} // namespace GlassInstallUtil
