#pragma once
#include <windows.h>
#include <shlobj.h>
#include <aclapi.h>
#include <sddl.h>
#include <msi.h>
#include <msiquery.h>
#include <string>
#include <vector>
#include "GlassInstallUtil.h"
#include "GlassInstallMsi.h"
#include <cwctype>

// One current bundle, not a version archive. All executable cache paths are
// fixed, administrator-owned and checked for writable/untrusted ACLs/reparse.
namespace GlassMaintenanceCache {
inline std::wstring Root() {
    PWSTR path=nullptr;if(FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData,0,nullptr,&path)))return {};
    std::wstring root=std::wstring(path)+L"\\Packages\\GlassPlatform";CoTaskMemFree(path);return root;
}
inline bool TrustedPath(const std::wstring& path, bool directory) {
    const DWORD attr=GetFileAttributesW(path.c_str());
    if(attr==INVALID_FILE_ATTRIBUTES||((attr&FILE_ATTRIBUTE_DIRECTORY)!=0)!=directory||(attr&FILE_ATTRIBUTE_REPARSE_POINT))return false;
    PSID owner=nullptr;PACL acl=nullptr;PSECURITY_DESCRIPTOR sd=nullptr;
    if(GetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()),SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,&owner,nullptr,&acl,nullptr,&sd)!=ERROR_SUCCESS)return false;
    BYTE admins[SECURITY_MAX_SID_SIZE]{},system[SECURITY_MAX_SID_SIZE]{};DWORD a=sizeof(admins),b=sizeof(system);
    const bool sids=CreateWellKnownSid(WinBuiltinAdministratorsSid,nullptr,admins,&a)&&CreateWellKnownSid(WinLocalSystemSid,nullptr,system,&b);
    bool ok=sids&&owner&&acl&&(EqualSid(owner,admins)||EqualSid(owner,system));
    if(ok)for(DWORD i=0;i<acl->AceCount;++i) {
        void* raw=nullptr;if(!GetAce(acl,i,&raw)){ok=false;break;}
        const auto* header=static_cast<ACE_HEADER*>(raw);
        if(header->AceType==ACCESS_DENIED_ACE_TYPE)continue;
        if(header->AceType!=ACCESS_ALLOWED_ACE_TYPE){ok=false;break;}
        auto* ace=static_cast<ACCESS_ALLOWED_ACE*>(raw);PSID sid=&ace->SidStart;
        const DWORD writes=GENERIC_ALL|GENERIC_WRITE|DELETE|WRITE_DAC|WRITE_OWNER|FILE_WRITE_DATA|FILE_APPEND_DATA|FILE_DELETE_CHILD|FILE_WRITE_EA|FILE_WRITE_ATTRIBUTES;
        if((ace->Mask&writes)&&!EqualSid(sid,admins)&&!EqualSid(sid,system)){ok=false;break;}
    }
    LocalFree(sd);return ok;
}
inline bool TrustedDirectory(const std::wstring& path){return TrustedPath(path,true);}
inline bool HardenFile(const std::wstring& path) {
    PSECURITY_DESCRIPTOR sd=nullptr;
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;GRGX;;;BU)",SDDL_REVISION_1,&sd,nullptr))return false;
    PSID owner=nullptr;PACL acl=nullptr;BOOL present=FALSE,defaulted=FALSE;
    const bool got=GetSecurityDescriptorOwner(sd,&owner,&defaulted)&&GetSecurityDescriptorDacl(sd,&present,&acl,&defaulted);
    const DWORD result=got?SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()),SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,owner,nullptr,acl,nullptr):ERROR_INVALID_SECURITY_DESCR;
    LocalFree(sd);return result==ERROR_SUCCESS&&TrustedPath(path,false);
}
inline bool SecureDirectory(const std::wstring& path) {
    PSECURITY_DESCRIPTOR sd=nullptr;
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;BU)",SDDL_REVISION_1,&sd,nullptr))return false;
    SECURITY_ATTRIBUTES sa{sizeof(sa),sd,FALSE};
    const bool created=CreateDirectoryW(path.c_str(),&sa)!=FALSE;
    const DWORD error=created?ERROR_SUCCESS:GetLastError();LocalFree(sd);
    return (created||error==ERROR_ALREADY_EXISTS)&&TrustedDirectory(path);
}
inline bool EnsureRoot() {
    const std::wstring root=Root();if(root.empty())return false;
    const auto slash=root.find_last_of(L'\\');const std::wstring parent=root.substr(0,slash);
    if(!SecureDirectory(parent))return false;
    for(std::size_t i=3;i<=parent.size();++i)if(i==parent.size()||parent[i]==L'\\') {
        const DWORD a=GetFileAttributesW(parent.substr(0,i).c_str());
        if(a==INVALID_FILE_ATTRIBUTES||(a&FILE_ATTRIBUTE_REPARSE_POINT))return false;
    }
    return SecureDirectory(root);
}
inline std::wstring CurrentFile(const wchar_t* name) {
    const auto root=Root();const auto current=root+L"\\Current";
    if(root.empty()||!TrustedDirectory(root)||!TrustedDirectory(current))return {};
    const auto path=current+L"\\"+name;
    const DWORD a=GetFileAttributesW(path.c_str());
    if(a==INVALID_FILE_ATTRIBUTES||(a&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||!TrustedPath(path,false))return {};
    return path;
}
inline bool RemoveFlat(const std::wstring& folder) {
    if(GetFileAttributesW(folder.c_str())==INVALID_FILE_ATTRIBUTES)return GetLastError()==ERROR_FILE_NOT_FOUND||GetLastError()==ERROR_PATH_NOT_FOUND;
    if(!TrustedDirectory(folder))return false;
    WIN32_FIND_DATAW data{};HANDLE find=FindFirstFileW((folder+L"\\*").c_str(),&data);
    if(find==INVALID_HANDLE_VALUE)return false;
    bool ok=true;
    do {
        if(wcscmp(data.cFileName,L".")==0||wcscmp(data.cFileName,L"..")==0)continue;
        if(data.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)){ok=false;break;}
        if(!DeleteFileW((folder+L"\\"+data.cFileName).c_str())){ok=false;break;}
    }while(FindNextFileW(find,&data));FindClose(find);
    return ok&&RemoveDirectoryW(folder.c_str());
}
inline std::wstring Property(MSIHANDLE db,const wchar_t* name) {
    const std::wstring sql=L"SELECT `Value` FROM `Property` WHERE `Property`='"+std::wstring(name)+L"'";
    MSIHANDLE view=0,record=0;std::wstring value;
    if(MsiDatabaseOpenViewW(db,sql.c_str(),&view)==ERROR_SUCCESS&&MsiViewExecute(view,0)==ERROR_SUCCESS&&MsiViewFetch(view,&record)==ERROR_SUCCESS) {
        wchar_t buffer[512]{};DWORD size=512;if(MsiRecordGetStringW(record,1,buffer,&size)==ERROR_SUCCESS)value=buffer;
    }
    if(record)MsiCloseHandle(record);if(view)MsiCloseHandle(view);return value;
}
inline bool Identity(const std::wstring& path,std::wstring& product,std::wstring& version) {
    MSIHANDLE db=0;if(MsiOpenDatabaseW(path.c_str(),MSIDBOPEN_READONLY,&db)!=ERROR_SUCCESS)return false;
    product=Property(db,L"ProductCode");version=Property(db,L"ProductVersion");MsiCloseHandle(db);
    if(product.size()!=38||version.empty())return false;
    for(wchar_t c:product)if(!iswxdigit(c)&&c!=L'{'&&c!=L'}'&&c!=L'-')return false;
    for(wchar_t c:version)if((c<L'0'||c>L'9')&&c!=L'.')return false;
    return true;
}
inline std::wstring InstalledVersion(const std::wstring& product) {
    wchar_t value[256]{};DWORD size=256;
    return MsiGetProductInfoW(product.c_str(),INSTALLPROPERTY_VERSIONSTRING,value,&size)==ERROR_SUCCESS?value:std::wstring();
}
inline std::wstring PackageCode(const std::wstring& file) {
    MSIHANDLE summary=0;if(MsiGetSummaryInformationW(0,file.c_str(),0,&summary)!=ERROR_SUCCESS)return {};
    wchar_t value[256]{};DWORD size=256;UINT type=0;
    const UINT status=MsiSummaryInfoGetPropertyW(summary,9,&type,nullptr,nullptr,value,&size);MsiCloseHandle(summary);
    if(status!=ERROR_SUCCESS||type!=VT_LPSTR)return {};
    return value;
}
struct Registration {std::wstring sid,version,local;MSIINSTALLCONTEXT context=MSIINSTALLCONTEXT_NONE;};
inline bool Registrations(const std::wstring& code,std::vector<Registration>& out) {
    out.clear();
    for(DWORD index=0;;++index) {
        wchar_t actual[39]{},sid[256]{};DWORD sidSize=256;MSIINSTALLCONTEXT context=MSIINSTALLCONTEXT_NONE;
        const UINT status=MsiEnumProductsExW(code.c_str(),L"S-1-1-0",MSIINSTALLCONTEXT_ALL,index,actual,&context,sid,&sidSize);
        if(status==ERROR_NO_MORE_ITEMS)return true;
        if(status!=ERROR_SUCCESS)return false;
        const wchar_t* user=context==MSIINSTALLCONTEXT_MACHINE?nullptr:sid;
        wchar_t local[32768]{},version[256]{};DWORD localSize=32768,versionSize=256;
        if(MsiGetProductInfoExW(code.c_str(),user,context,INSTALLPROPERTY_VERSIONSTRING,version,&versionSize)!=ERROR_SUCCESS||
           MsiGetProductInfoExW(code.c_str(),user,context,INSTALLPROPERTY_LOCALPACKAGE,local,&localSize)!=ERROR_SUCCESS)return false;
        out.push_back({sid,version,local,context});
    }
}
// -1 = unknown: never release a repair source on incomplete discovery.
inline int RegistrationState(const std::wstring& file,const std::wstring& code,const std::wstring& version) {
    std::vector<Registration> all;if(!Registrations(code,all))return -1;
    const auto package=PackageCode(file);if(package.empty())return -1;
    bool match=false;
    for(const auto& reg:all) {
        const auto registered=PackageCode(reg.local);if(registered.empty())return -1;
        if(version==reg.version&&_wcsicmp(package.c_str(),registered.c_str())==0)match=true;
    }
    return match?1:0;
}
inline bool MatchesInstalled(const std::wstring& file,const std::wstring& code,const std::wstring& version) {
    return RegistrationState(file,code,version)==1;
}
inline bool RegisterRepairSource(const std::wstring& file) {
    std::wstring code,version;if(!TrustedPath(file,false)||!Identity(file,code,version))return false;
    std::vector<Registration> all;if(!Registrations(code,all))return false;
    const auto package=PackageCode(file);if(package.empty())return false;
    for(const auto& reg:all) {
        const auto registered=PackageCode(reg.local);if(registered.empty())return false;
        if(version!=reg.version||_wcsicmp(package.c_str(),registered.c_str())!=0)continue;
        const wchar_t* user=reg.context==MSIINSTALLCONTEXT_MACHINE?nullptr:reg.sid.c_str();
        const auto slash=file.find_last_of(L'\\');const auto folder=file.substr(0,slash+1),name=file.substr(slash+1);
        if(MsiSourceListAddSourceExW(code.c_str(),user,reg.context,MSICODE_PRODUCT|MSISOURCETYPE_NETWORK,folder.c_str(),0)!=ERROR_SUCCESS)return false;
        if(MsiSourceListSetInfoW(code.c_str(),user,reg.context,MSICODE_PRODUCT,INSTALLPROPERTY_PACKAGENAME,name.c_str())!=ERROR_SUCCESS)return false;
        if(MsiSourceListSetInfoW(code.c_str(),user,reg.context,MSICODE_PRODUCT|MSISOURCETYPE_NETWORK,INSTALLPROPERTY_LASTUSEDSOURCE,folder.c_str())!=ERROR_SUCCESS)return false;
    }
    return true;
}
inline bool GarbageCollect() {
    const auto root=Root(),retained=root+L"\\Retained";
    if(!TrustedDirectory(root)||!TrustedDirectory(retained))return false;
    WIN32_FIND_DATAW data{};HANDLE find=FindFirstFileW((retained+L"\\*.msi").c_str(),&data);
    if(find==INVALID_HANDLE_VALUE)return GetLastError()==ERROR_FILE_NOT_FOUND;
    bool ok=true;
    do {
        if(data.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)){ok=false;continue;}
        const auto file=retained+L"\\"+data.cFileName;std::wstring code,version;
        if(!Identity(file,code,version)){ok=false;continue;}
        const int state=RegistrationState(file,code,version);
        if(state<0){ok=false;continue;}
        if(state==0&&!DeleteFileW(file.c_str()))ok=false;
    }while(FindNextFileW(find,&data));FindClose(find);return ok;
}
struct Blob {UINT id;const wchar_t* file;};
// Invoked only after successful MSI commit. Failed publication leaves Previous
// intact; recovery restores it on the next attempt. Never prunes system MSI cache.
inline bool Publish(HINSTANCE instance,const Blob* blobs,std::size_t count,UINT uninstallerResource) {
    struct CacheLock {
        HANDLE handle=nullptr;bool acquired=false;
        CacheLock(){
            PSECURITY_DESCRIPTOR sd=nullptr;
            if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAG:BAD:P(A;;GA;;;SY)(A;;GA;;;BA)",SDDL_REVISION_1,&sd,nullptr))return;
            SECURITY_ATTRIBUTES sa{sizeof(sa),sd,FALSE};
            handle=CreateMutexW(&sa,FALSE,L"Global\\GlassPlatform.MaintenanceCache.V1");LocalFree(sd);
            if(handle){const DWORD wait=WaitForSingleObject(handle,0);acquired=wait==WAIT_OBJECT_0||wait==WAIT_ABANDONED;}
        }
        ~CacheLock(){if(acquired)ReleaseMutex(handle);if(handle)CloseHandle(handle);}
    } lock;
    if(!lock.acquired||!EnsureRoot())return false;
    const auto root=Root(),current=root+L"\\Current",previous=root+L"\\Previous",stage=root+L"\\Staging",retained=root+L"\\Retained";
    if(!SecureDirectory(retained))return false;
    const bool hasCurrent=GetFileAttributesW(current.c_str())!=INVALID_FILE_ATTRIBUTES;
    if(!hasCurrent&&GetFileAttributesW(previous.c_str())!=INVALID_FILE_ATTRIBUTES) {
        if(!TrustedDirectory(previous)||!MoveFileExW(previous.c_str(),current.c_str(),MOVEFILE_WRITE_THROUGH))return false;
    }
    if(!RemoveFlat(stage)||!SecureDirectory(stage))return false;
    wchar_t exe[32768]{};if(!GetModuleFileNameW(nullptr,exe,32768)||!CopyFileW(exe,(stage+L"\\GlassSetup.exe").c_str(),TRUE))return false;
    if(!GlassInstallUtil::ExtractResourceToFile(instance,uninstallerResource,RT_RCDATA,stage+L"\\GlassUnins.exe"))return false;
    for(std::size_t i=0;i<count;++i)
        if(!GlassInstallUtil::ExtractResourceToFile(instance,blobs[i].id,RT_RCDATA,stage+L"\\"+blobs[i].file))return false;
    WIN32_FIND_DATAW staged{};HANDLE stagedFind=FindFirstFileW((stage+L"\\*").c_str(),&staged);
    if(stagedFind==INVALID_HANDLE_VALUE)return false;
    bool secured=true;
    do {
        if(wcscmp(staged.cFileName,L".")==0||wcscmp(staged.cFileName,L"..")==0)continue;
        if((staged.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||!HardenFile(stage+L"\\"+staged.cFileName)){secured=false;break;}
    }while(FindNextFileW(stagedFind,&staged));FindClose(stagedFind);if(!secured)return false;
    // Retain only the source of an actually installed, different old version.
    if(GetFileAttributesW(current.c_str())!=INVALID_FILE_ATTRIBUTES) {
        if(!TrustedDirectory(current))return false;
        for(std::size_t i=0;i<count;++i) {
            const auto old=current+L"\\"+blobs[i].file,newer=stage+L"\\"+blobs[i].file;
            if(GetFileAttributesW(old.c_str())==INVALID_FILE_ATTRIBUTES)continue;
            std::wstring code,version,newCode,newVersion;
            if(!TrustedPath(old,false)||!Identity(old,code,version)||!Identity(newer,newCode,newVersion))return false;
            if(RegistrationState(old,code,version)<0||RegistrationState(newer,newCode,newVersion)<0)return false;
            if(code==newCode&&MatchesInstalled(old,code,version)&&
               (version!=newVersion||_wcsicmp(PackageCode(old).c_str(),PackageCode(newer).c_str())!=0)) {
                const auto package=PackageCode(old);
                if(package.size()!=38)return false;
                for(wchar_t c:package)if(!iswxdigit(c)&&c!=L'{'&&c!=L'}'&&c!=L'-')return false;
                const auto dest=retained+L"\\"+code+L"-"+version+L"-"+package+L".msi";
                if(!CopyFileW(old.c_str(),dest.c_str(),TRUE)&&GetLastError()!=ERROR_FILE_EXISTS)return false;
                if(!TrustedPath(dest,false))return false;
            }
        }
        if(!RemoveFlat(previous)||!MoveFileExW(current.c_str(),previous.c_str(),MOVEFILE_WRITE_THROUGH))return false;
    }
    if(!MoveFileExW(stage.c_str(),current.c_str(),MOVEFILE_WRITE_THROUGH)) {
        MoveFileExW(previous.c_str(),current.c_str(),MOVEFILE_WRITE_THROUGH);return false;
    }
    bool registered=true;
    for(std::size_t i=0;i<count;++i)if(!RegisterRepairSource(current+L"\\"+blobs[i].file))registered=false;
    WIN32_FIND_DATAW source{};HANDLE sources=FindFirstFileW((retained+L"\\*.msi").c_str(),&source);
    if(sources!=INVALID_HANDLE_VALUE) {
        do {if(!RegisterRepairSource(retained+L"\\"+source.cFileName))registered=false;}while(FindNextFileW(sources,&source));FindClose(sources);
    }
    // Do not release the old bundle until all installed sources have registered.
    if(!registered)return false;
    const bool cleaned=RemoveFlat(previous);const bool gc=GarbageCollect();return cleaned&&gc;
}
}
