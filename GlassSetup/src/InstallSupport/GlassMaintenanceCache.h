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
#include "GlassCacheDiagnostics.h"
#include "GlassRegisteredMsi.h"

// One current bundle, not a version archive. All executable cache paths are
// fixed, administrator-owned and checked for writable/untrusted ACLs/reparse.
namespace GlassMaintenanceCache {
namespace Diag=Diagnostics;
inline std::wstring Root() {
    PWSTR path=nullptr;const HRESULT hr=SHGetKnownFolderPath(FOLDERID_ProgramData,0,nullptr,&path);
    if(FAILED(hr)){Diag::Fail(L"SHGetKnownFolderPath",L"FOLDERID_ProgramData",static_cast<DWORD>(hr),{},L"HRESULT");return {};}
    std::wstring root=std::wstring(path)+L"\\Packages\\GlassPlatform";CoTaskMemFree(path);return root;
}
inline bool TrustedPath(const std::wstring& path, bool directory) {
    const DWORD attr=GetFileAttributesW(path.c_str());
    if(attr==INVALID_FILE_ATTRIBUTES)return Diag::Fail(L"GetFileAttributesW",path,GetLastError());
    if(((attr&FILE_ATTRIBUTE_DIRECTORY)!=0)!=directory)
        return Diag::Fail(L"ValidateObjectType",path,ERROR_INVALID_DATA,L"Expected "+std::wstring(directory?L"directory":L"file"),L"Policy");
    if(attr&FILE_ATTRIBUTE_REPARSE_POINT)
        return Diag::Fail(L"ValidateReparsePoint",path,ERROR_ACCESS_DENIED,L"Reparse point refused",L"Policy");
    PSID owner=nullptr;PACL acl=nullptr;PSECURITY_DESCRIPTOR sd=nullptr;
    const DWORD security=GetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()),SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,&owner,nullptr,&acl,nullptr,&sd);
    if(security!=ERROR_SUCCESS)return Diag::Fail(L"GetNamedSecurityInfoW",path,security);
    BYTE admins[SECURITY_MAX_SID_SIZE]{},system[SECURITY_MAX_SID_SIZE]{};DWORD a=sizeof(admins),b=sizeof(system);
    if(!CreateWellKnownSid(WinBuiltinAdministratorsSid,nullptr,admins,&a)||!CreateWellKnownSid(WinLocalSystemSid,nullptr,system,&b)) {
        const DWORD error=GetLastError();LocalFree(sd);return Diag::Fail(L"CreateWellKnownSid",path,error);
    }
    std::wstring refusal;
    if(!owner||!acl)refusal=L"Missing owner or DACL";
    else if(!EqualSid(owner,admins)&&!EqualSid(owner,system))refusal=L"Owner is not Administrators or SYSTEM; owner="+Diag::SidText(owner);
    bool ok=refusal.empty();
    if(ok)for(DWORD i=0;i<acl->AceCount;++i) {
        void* raw=nullptr;if(!GetAce(acl,i,&raw)) {
            const DWORD error=GetLastError();LocalFree(sd);return Diag::Fail(L"GetAce",path,error,L"ACE index="+std::to_wstring(i));
        }
        const auto* header=static_cast<ACE_HEADER*>(raw);
        if(header->AceType==ACCESS_DENIED_ACE_TYPE)continue;
        if(header->AceType!=ACCESS_ALLOWED_ACE_TYPE){refusal=L"Unsupported ACE type="+std::to_wstring(header->AceType)+L" index="+std::to_wstring(i);ok=false;break;}
        auto* ace=static_cast<ACCESS_ALLOWED_ACE*>(raw);PSID sid=&ace->SidStart;
        const DWORD writes=GENERIC_ALL|GENERIC_WRITE|DELETE|WRITE_DAC|WRITE_OWNER|FILE_WRITE_DATA|FILE_APPEND_DATA|FILE_DELETE_CHILD|FILE_WRITE_EA|FILE_WRITE_ATTRIBUTES;
        if((ace->Mask&writes)&&!EqualSid(sid,admins)&&!EqualSid(sid,system)) {
            refusal=L"Write ACE for non-admin SID="+Diag::SidText(sid)+L" mask="+Diag::CodeText(ace->Mask)+
                L" flags="+std::to_wstring(header->AceFlags)+L" index="+std::to_wstring(i)+L" owner="+Diag::SidText(owner);
            ok=false;break;
        }
    }
    LocalFree(sd);
    return ok?true:Diag::Fail(L"ValidateOwnerAndDacl",path,ERROR_ACCESS_DENIED,refusal,L"Policy");
}
inline bool TrustedDirectory(const std::wstring& path){return TrustedPath(path,true);}
inline bool HardenFile(const std::wstring& path) {
    PSECURITY_DESCRIPTOR sd=nullptr;
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;GRGX;;;BU)",SDDL_REVISION_1,&sd,nullptr))
        return Diag::Fail(L"ConvertStringSecurityDescriptorToSecurityDescriptorW",path,GetLastError());
    PSID owner=nullptr;PACL acl=nullptr;BOOL present=FALSE,defaulted=FALSE;
    if(!GetSecurityDescriptorOwner(sd,&owner,&defaulted)) {
        const DWORD error=GetLastError();LocalFree(sd);return Diag::Fail(L"GetSecurityDescriptorOwner",path,error);
    }
    if(!GetSecurityDescriptorDacl(sd,&present,&acl,&defaulted)) {
        const DWORD error=GetLastError();LocalFree(sd);return Diag::Fail(L"GetSecurityDescriptorDacl",path,error);
    }
    const DWORD result=SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()),SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,owner,nullptr,acl,nullptr);
    LocalFree(sd);
    if(result!=ERROR_SUCCESS)return Diag::Fail(L"SetNamedSecurityInfoW",path,result);
    return TrustedPath(path,false);
}
inline bool SecureDirectory(const std::wstring& path) {
    PSECURITY_DESCRIPTOR sd=nullptr;
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;BU)",SDDL_REVISION_1,&sd,nullptr))
        return Diag::Fail(L"ConvertStringSecurityDescriptorToSecurityDescriptorW",path,GetLastError());
    SECURITY_ATTRIBUTES sa{sizeof(sa),sd,FALSE};
    const bool created=CreateDirectoryW(path.c_str(),&sa)!=FALSE;
    const DWORD error=created?ERROR_SUCCESS:GetLastError();LocalFree(sd);
    if(!created&&error!=ERROR_ALREADY_EXISTS)return Diag::Fail(L"CreateDirectoryW",path,error);
    if(Diag::active)GlassInstallLog::LineFormat(L"Maintenance cache: directory %s path=%s",created?L"created":L"already exists",path.c_str());
    return TrustedDirectory(path);
}
// Shared ProgramData\Packages may carry foreign ACEs. Never harden or reject it;
// only ensure the path is a real directory without reparse tricks.
inline bool ValidateSharedParentDirectory(const std::wstring& path) {
    const DWORD attr=GetFileAttributesW(path.c_str());
    if(attr==INVALID_FILE_ATTRIBUTES) {
        Diag::Step(L"create-shared-parent",path);
        return SecureDirectory(path);
    }
    if(!(attr&FILE_ATTRIBUTE_DIRECTORY))
        return Diag::Fail(L"ValidateObjectType",path,ERROR_INVALID_DATA,L"Expected directory",L"Policy");
    if(attr&FILE_ATTRIBUTE_REPARSE_POINT)
        return Diag::Fail(L"ValidateReparsePoint",path,ERROR_ACCESS_DENIED,L"Reparse point refused",L"Policy");
    return true;
}
inline bool EnsureRoot() {
    Diag::Step(L"resolve-root",L"FOLDERID_ProgramData");
    const std::wstring root=Root();if(root.empty())return false;
    const auto slash=root.find_last_of(L'\\');const std::wstring parent=root.substr(0,slash);
    // Do not rewrite ACLs on this shared Windows folder. Diagnose policy
    // refusals separately from CreateDirectory failures before touching our root.
    Diag::Step(L"validate-packages-parent",parent);
    if(!ValidateSharedParentDirectory(parent))return false;
    Diag::Step(L"validate-ancestors",parent);
    for(std::size_t i=3;i<=parent.size();++i)if(i==parent.size()||parent[i]==L'\\') {
        const auto ancestor=parent.substr(0,i);const DWORD a=GetFileAttributesW(ancestor.c_str());
        if(a==INVALID_FILE_ATTRIBUTES)return Diag::Fail(L"GetFileAttributesW",ancestor,GetLastError());
        if(a&FILE_ATTRIBUTE_REPARSE_POINT)return Diag::Fail(L"ValidateReparsePoint",ancestor,ERROR_ACCESS_DENIED,L"Reparse ancestor refused",L"Policy");
    }
    Diag::Step(L"create-owned-root",root);
    return SecureDirectory(root);
}
// Independent controller store. The record contains Setup SHA256 + Unins SHA256,
// not paths. Immutable code folders are addressed by the Setup digest.
inline bool ControllerRecordValid(const std::string& record) {
    if(record.size()!=128)return false;
    for(char c:record)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;
    return true;
}
inline std::string ReadControllerRecord(const wchar_t* name=L"Active") {
    const auto root=Root(),store=root+L"\\Installer";
    if(root.empty()||!TrustedDirectory(root)||!TrustedDirectory(store))return {};
    const auto path=store+L"\\"+name;
    if(!GlassInstallUtil::IsSafeInstallPath(path)||!TrustedPath(path,false))return {};
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(file==INVALID_HANDLE_VALUE)return {};
    char bytes[129]{};DWORD count=0;
    const bool ok=ReadFile(file,bytes,sizeof(bytes),&count,nullptr)!=FALSE;
    CloseHandle(file);
    const std::string record(bytes,count);
    return ok&&ControllerRecordValid(record)?record:std::string{};
}
inline std::string ControllerDigest(const std::wstring& path) {
    std::array<BYTE,32> hash{};if(!GlassRegisteredMsi::Hash(path,hash))return {};
    constexpr char hex[]="0123456789abcdef";std::string out;
    for(BYTE b:hash){out+=hex[b>>4];out+=hex[b&15];}return out;
}
inline std::wstring ControllerFileForRecord(const std::string& record,const wchar_t* name) {
    if(!ControllerRecordValid(record)||(wcscmp(name,L"GlassSetup.exe")&&wcscmp(name,L"GlassUnins.exe")))return {};
    const auto root=Root(),store=root+L"\\Installer";
    const auto hash=record.substr(0,64);
    const auto folder=store+L"\\"+std::wstring(hash.begin(),hash.end());
    if(!TrustedDirectory(root)||!TrustedDirectory(store)||!TrustedDirectory(folder))return {};
    const auto file=folder+L"\\"+name;
    if(!GlassInstallUtil::IsSafeInstallPath(file)||!TrustedPath(file,false))return {};
    const auto expected=record.substr(wcscmp(name,L"GlassSetup.exe")?64:0,64);
    return ControllerDigest(file)==expected?file:std::wstring{};
}
inline std::wstring ControllerFile(const wchar_t* name) {
    const auto root=Root();
    if(root.empty()||GetFileAttributesW((root+L"\\Installer\\Active").c_str())==INVALID_FILE_ATTRIBUTES)return {};
    return ControllerFileForRecord(ReadControllerRecord(),name);
}
inline std::wstring CurrentFile(const wchar_t* name) {
    if(!wcscmp(name,L"GlassSetup.exe")||!wcscmp(name,L"GlassUnins.exe")) {
        const auto root=Root();
        if(!root.empty()&&GetFileAttributesW((root+L"\\Installer\\Active").c_str())!=INVALID_FILE_ATTRIBUTES)
            return ControllerFile(name); // Do not downgrade to legacy on a corrupt active record.
    }
    const auto root=Root();const auto current=root+L"\\Current";
    if(root.empty())return {};
    const DWORD currentAttributes=GetFileAttributesW(current.c_str());
    if(currentAttributes==INVALID_FILE_ATTRIBUTES)return {}; // Normal before first installation.
    if(!TrustedDirectory(root)||!TrustedDirectory(current))return {};
    const auto path=current+L"\\"+name;
    const DWORD a=GetFileAttributesW(path.c_str());
    if(a==INVALID_FILE_ATTRIBUTES||(a&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||!TrustedPath(path,false))return {};
    return path;
}
inline bool RemoveFlat(const std::wstring& folder) {
    if(GetFileAttributesW(folder.c_str())==INVALID_FILE_ATTRIBUTES) {
        const DWORD error=GetLastError();
        return error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND?true:Diag::Fail(L"GetFileAttributesW",folder,error);
    }
    if(!TrustedDirectory(folder))return false;
    WIN32_FIND_DATAW data{};HANDLE find=FindFirstFileW((folder+L"\\*").c_str(),&data);
    if(find==INVALID_HANDLE_VALUE)return Diag::Fail(L"FindFirstFileW",folder,GetLastError());
    bool ok=true;
    for(;;) {
        if(wcscmp(data.cFileName,L".")!=0&&wcscmp(data.cFileName,L"..")!=0) {
            const auto file=folder+L"\\"+data.cFileName;
            if(data.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) {
                ok=Diag::Fail(L"RemoveFlat",file,ERROR_ACCESS_DENIED,L"Directory or reparse child refused",L"Policy");break;
            }
            if(!DeleteFileW(file.c_str())){ok=Diag::Fail(L"DeleteFileW",file,GetLastError());break;}
        }
        if(!FindNextFileW(find,&data)) {
            const DWORD error=GetLastError();if(error!=ERROR_NO_MORE_FILES)ok=Diag::Fail(L"FindNextFileW",folder,error);break;
        }
    }
    FindClose(find);
    if(!ok)return false;
    return RemoveDirectoryW(folder.c_str())?true:Diag::Fail(L"RemoveDirectoryW",folder,GetLastError());
}
inline std::wstring Property(MSIHANDLE db,const wchar_t* name,const std::wstring& file) {
    const std::wstring sql=L"SELECT `Value` FROM `Property` WHERE `Property`='"+std::wstring(name)+L"'";
    MSIHANDLE view=0,record=0;std::wstring value;
    UINT status=MsiDatabaseOpenViewW(db,sql.c_str(),&view);
    const wchar_t* operation=L"MsiDatabaseOpenViewW";
    if(status==ERROR_SUCCESS){operation=L"MsiViewExecute";status=MsiViewExecute(view,0);}
    if(status==ERROR_SUCCESS){operation=L"MsiViewFetch";status=MsiViewFetch(view,&record);}
    if(status==ERROR_SUCCESS) {
        wchar_t buffer[512]{};DWORD size=512;operation=L"MsiRecordGetStringW";status=MsiRecordGetStringW(record,1,buffer,&size);
        if(status==ERROR_SUCCESS)value=buffer;
    }
    if(record)MsiCloseHandle(record);if(view)MsiCloseHandle(view);
    if(status!=ERROR_SUCCESS)Diag::Fail(operation,file,status,L"Property="+std::wstring(name),L"MSI");
    return value;
}
inline bool Identity(const std::wstring& path,std::wstring& product,std::wstring& version) {
    MSIHANDLE db=0;const UINT status=MsiOpenDatabaseW(path.c_str(),MSIDBOPEN_READONLY,&db);
    if(status!=ERROR_SUCCESS)return Diag::Fail(L"MsiOpenDatabaseW",path,status,{},L"MSI");
    product=Property(db,L"ProductCode",path);version=Property(db,L"ProductVersion",path);MsiCloseHandle(db);
    if(product.size()!=38||version.empty())return Diag::Fail(L"ValidateMsiIdentity",path,ERROR_INVALID_DATA,L"Missing/invalid ProductCode or ProductVersion",L"Policy");
    for(wchar_t c:product)if(!iswxdigit(c)&&c!=L'{'&&c!=L'}'&&c!=L'-')return Diag::Fail(L"ValidateProductCode",path,ERROR_INVALID_DATA,product,L"Policy");
    for(wchar_t c:version)if((c<L'0'||c>L'9')&&c!=L'.')return Diag::Fail(L"ValidateProductVersion",path,ERROR_INVALID_DATA,version,L"Policy");
    return true;
}
inline std::wstring PackageCode(const std::wstring& file) {
    MSIHANDLE summary=0;const UINT open=MsiGetSummaryInformationW(0,file.c_str(),0,&summary);
    if(open!=ERROR_SUCCESS){Diag::Fail(L"MsiGetSummaryInformationW",file,open,{},L"MSI");return {};}
    wchar_t value[256]{};DWORD size=256;UINT type=0;
    const UINT status=MsiSummaryInfoGetPropertyW(summary,9,&type,nullptr,nullptr,value,&size);MsiCloseHandle(summary);
    if(status!=ERROR_SUCCESS){Diag::Fail(L"MsiSummaryInfoGetPropertyW",file,status,L"PID_REVNUMBER",L"MSI");return {};}
    if(type!=VT_LPSTR){Diag::Fail(L"ValidatePackageCode",file,ERROR_INVALID_DATA,L"Unexpected summary type="+std::to_wstring(type),L"Policy");return {};}
    if(!value[0]){Diag::Fail(L"ValidatePackageCode",file,ERROR_INVALID_DATA,L"Empty summary PackageCode",L"Policy");return {};}
    return value;
}
struct Registration {std::wstring sid,version,local;MSIINSTALLCONTEXT context=MSIINSTALLCONTEXT_NONE;};
inline bool Registrations(const std::wstring& code,std::vector<Registration>& out) {
    out.clear();
    for(DWORD index=0;;++index) {
        wchar_t actual[39]{},sid[256]{};DWORD sidSize=256;MSIINSTALLCONTEXT context=MSIINSTALLCONTEXT_NONE;
        const UINT status=MsiEnumProductsExW(code.c_str(),L"S-1-1-0",MSIINSTALLCONTEXT_ALL,index,actual,&context,sid,&sidSize);
        if(status==ERROR_NO_MORE_ITEMS)return true;
        if(status!=ERROR_SUCCESS)return Diag::Fail(L"MsiEnumProductsExW",code,status,{},L"MSI");
        const wchar_t* user=context==MSIINSTALLCONTEXT_MACHINE?nullptr:sid;
        wchar_t local[32768]{},version[256]{};DWORD localSize=32768,versionSize=256;
        UINT query=MsiGetProductInfoExW(code.c_str(),user,context,INSTALLPROPERTY_VERSIONSTRING,version,&versionSize);
        if(query!=ERROR_SUCCESS)return Diag::Fail(L"MsiGetProductInfoExW",code,query,L"VersionString SID="+std::wstring(sid)+L" context="+std::to_wstring(context),L"MSI");
        query=MsiGetProductInfoExW(code.c_str(),user,context,INSTALLPROPERTY_LOCALPACKAGE,local,&localSize);
        if(query!=ERROR_SUCCESS)return Diag::Fail(L"MsiGetProductInfoExW",code,query,L"LocalPackage SID="+std::wstring(sid)+L" context="+std::to_wstring(context),L"MSI");
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
// Bind a published source to the exact installed per-machine cache baseline.
// Additional per-user/ambiguous registrations are not implicit proof of ownership.
inline bool InstalledCacheDigest(const std::wstring& file,std::array<BYTE,32>& digest) {
    std::wstring code,version;
    if(!Identity(file,code,version))return false;
    std::vector<Registration> all;
    if(!Registrations(code,all) || all.size()!=1 || all[0].context!=MSIINSTALLCONTEXT_MACHINE)return false;
    const auto package=PackageCode(file),cached=PackageCode(all[0].local);
    return !package.empty() && !cached.empty() && version==all[0].version &&
        _wcsicmp(package.c_str(),cached.c_str())==0 && GlassRegisteredMsi::Hash(all[0].local,digest);
}
// Distribution MSI bytes are NOT the bytes of Windows Installer's LocalPackage:
// Windows may strip embedded cabinets from its cache. Bind distribution bytes
// to our protected publication record, and compare MSI identity to LocalPackage
// separately. Never hash the stripped cache and call it a distribution digest.
inline bool PublishedDigestMatches(const std::wstring& file) {
    const auto slash=file.find_last_of(L"\\/");
    const auto name=file.substr(slash==std::wstring::npos?0:slash+1)+L".sha256";
    const auto record=CurrentFile(name.c_str());
    if(record.empty())return false; // Legacy ACL/identity alone is not provenance.
    HANDLE handle=CreateFileW(record.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(handle==INVALID_HANDLE_VALUE)return false;
    BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER length{};
    char text[128]{};DWORD read=0;
    const bool loaded=GetFileInformationByHandle(handle,&info) && GetFileSizeEx(handle,&length) &&
        !(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) &&
        info.nNumberOfLinks==1 && length.QuadPart==sizeof(text) &&
        ReadFile(handle,text,sizeof(text),&read,nullptr) && read==sizeof(text);
    CloseHandle(handle);
    if(!loaded)return false;
    const auto hex=[](char c)->int{return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;};
    std::array<BYTE,64> expected{};
    std::array<BYTE,32> actual{},installed{};
    for(std::size_t i=0;i<expected.size();++i) {
        const int high=hex(text[i*2]),low=hex(text[i*2+1]);
        if(high<0||low<0)return false;
        expected[i]=static_cast<BYTE>((high<<4)|low);
    }
    if(!GlassRegisteredMsi::Hash(file,actual) || !InstalledCacheDigest(file,installed))return false;
    for(std::size_t i=0;i<actual.size();++i)
        if(actual[i]!=expected[i] || installed[i]!=expected[i+32])return false;
    return true;
}
// Record format: 128 lowercase ASCII hex bytes: source SHA256, then LocalPackage SHA256.
inline bool WritePublishedDigest(const std::wstring& file) {
    std::array<BYTE,32> digest{},installed{};
    if(!GlassRegisteredMsi::Hash(file,digest) || !InstalledCacheDigest(file,installed))return Diag::Fail(L"HashPublishedPackage",file,ERROR_INVALID_DATA);
    constexpr char hex[]="0123456789abcdef";
    char text[128]{};
    for(std::size_t i=0;i<digest.size();++i){
        text[i*2]=hex[digest[i]>>4];text[i*2+1]=hex[digest[i]&15];
        text[64+i*2]=hex[installed[i]>>4];text[64+i*2+1]=hex[installed[i]&15];
    }
    const auto record=file+L".sha256";
    HANDLE handle=CreateFileW(record.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(handle==INVALID_HANDLE_VALUE)return Diag::Fail(L"CreatePackageDigest",record,GetLastError());
    DWORD written=0;
    bool ok=WriteFile(handle,text,sizeof(text),&written,nullptr) && written==sizeof(text) && FlushFileBuffers(handle);
    DWORD error=ok?ERROR_SUCCESS:GetLastError();
    if(!CloseHandle(handle)){if(ok)error=GetLastError();ok=false;}
    if(!ok){DeleteFileW(record.c_str());return Diag::Fail(L"WritePackageDigest",record,error?error:ERROR_WRITE_FAULT);}
    return true; // The publication hardening pass protects MSI and record together.
}
inline bool MatchesInstalled(const std::wstring& file,const std::wstring& code,const std::wstring& version) {
    return RegistrationState(file,code,version)==1 && PublishedDigestMatches(file);
}
// Coverage is independent of publication. An unbound legacy source is retained,
// but never registered as trusted and never labelled an I/O/publication failure.
inline bool RegisterRepairSource(const std::wstring& file,std::vector<std::wstring>* needsOriginal=nullptr) {
    std::wstring code,version;if(!TrustedPath(file,false)||!Identity(file,code,version))return false;
    std::vector<Registration> all;if(!Registrations(code,all))return false;
    const auto package=PackageCode(file);if(package.empty())return false;
    for(const auto& reg:all) {
        const auto registered=PackageCode(reg.local);if(registered.empty())return false;
        if(version!=reg.version||_wcsicmp(package.c_str(),registered.c_str())!=0)continue;
        if(!PublishedDigestMatches(file)) {
            if(needsOriginal)needsOriginal->push_back(file);
            GlassInstallLog::LineFormat(L"Maintenance source coverage: NeedsOriginalSource product=%s context=%u SID=%s source=%s; no verified distribution/installed-cache binding; source retained, not registered",
                code.c_str(),static_cast<unsigned>(reg.context),reg.sid.c_str(),file.c_str());
            continue;
        }
        const wchar_t* user=reg.context==MSIINSTALLCONTEXT_MACHINE?nullptr:reg.sid.c_str();
        const auto slash=file.find_last_of(L'\\');const auto folder=file.substr(0,slash+1),name=file.substr(slash+1);
        const auto registration=L"product="+code+L" SID="+reg.sid+L" context="+std::to_wstring(reg.context);
        UINT result=MsiSourceListAddSourceExW(code.c_str(),user,reg.context,MSICODE_PRODUCT|MSISOURCETYPE_NETWORK,folder.c_str(),0);
        if(result!=ERROR_SUCCESS)return Diag::Fail(L"MsiSourceListAddSourceExW",file,result,registration,L"MSI");
        result=MsiSourceListSetInfoW(code.c_str(),user,reg.context,MSICODE_PRODUCT,INSTALLPROPERTY_PACKAGENAME,name.c_str());
        if(result!=ERROR_SUCCESS)return Diag::Fail(L"MsiSourceListSetInfoW",file,result,registration+L" PackageName",L"MSI");
        result=MsiSourceListSetInfoW(code.c_str(),user,reg.context,MSICODE_PRODUCT|MSISOURCETYPE_NETWORK,INSTALLPROPERTY_LASTUSEDSOURCE,folder.c_str());
        if(result!=ERROR_SUCCESS)return Diag::Fail(L"MsiSourceListSetInfoW",file,result,registration+L" LastUsedSource",L"MSI");
    }
    return true;
}
inline bool GarbageCollect() {
    const auto root=Root(),retained=root+L"\\Retained";
    if(!TrustedDirectory(root)||!TrustedDirectory(retained))return false;
    WIN32_FIND_DATAW data{};HANDLE find=FindFirstFileW((retained+L"\\*.msi").c_str(),&data);
    if(find==INVALID_HANDLE_VALUE) {
        const DWORD error=GetLastError();return error==ERROR_FILE_NOT_FOUND?true:Diag::Fail(L"FindFirstFileW",retained,error);
    }
    bool ok=true;
    for(;;) {
        const auto file=retained+L"\\"+data.cFileName;
        if(data.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))
            ok=Diag::Fail(L"GarbageCollect",file,ERROR_ACCESS_DENIED,L"Directory/reparse source refused",L"Policy");
        else {
            std::wstring code,version;
            if(!Identity(file,code,version))ok=false;
            else {
                const int state=RegistrationState(file,code,version);
                if(state<0)ok=false;
                else if(state==0&&!DeleteFileW(file.c_str()))ok=Diag::Fail(L"DeleteFileW",file,GetLastError());
            }
        }
        if(!FindNextFileW(find,&data)) {
            const DWORD error=GetLastError();if(error!=ERROR_NO_MORE_FILES)ok=Diag::Fail(L"FindNextFileW",retained,error);break;
        }
    }
    FindClose(find);return ok;
}
struct Blob {
    UINT id;const wchar_t* file;const wchar_t* source=nullptr;
    // Only the acquisition adapter can assert this after signature/manifest
    // validation of its private copy. A legacy Current file is never blessed.
    bool verifiedSource=false;
};
// Invoked only after successful MSI commit. Failed publication leaves Previous
// intact; recovery restores it on the next attempt. Never prunes system MSI cache.
inline bool Publish(HINSTANCE instance,const Blob* blobs,std::size_t count,UINT uninstallerResource,
    std::wstring* failureDetail=nullptr,std::vector<std::wstring>* needsOriginal=nullptr) {
    if(needsOriginal)needsOriginal->clear();
    Diag::Publication publication(failureDetail);
    Diag::Step(L"acquire-lock",L"Global\\GlassPlatform.MaintenanceCache.V1");
    struct CacheLock {
        HANDLE handle=nullptr;bool acquired=false;
        CacheLock(){
            constexpr auto name=L"Global\\GlassPlatform.MaintenanceCache.V1";
            PSECURITY_DESCRIPTOR sd=nullptr;
            if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAG:BAD:P(A;;GA;;;SY)(A;;GA;;;BA)",SDDL_REVISION_1,&sd,nullptr)) {
                Diag::Fail(L"ConvertStringSecurityDescriptorToSecurityDescriptorW",name,GetLastError());return;
            }
            SECURITY_ATTRIBUTES sa{sizeof(sa),sd,FALSE};
            handle=CreateMutexW(&sa,FALSE,name);const DWORD error=handle?ERROR_SUCCESS:GetLastError();LocalFree(sd);
            if(!handle){Diag::Fail(L"CreateMutexW",name,error);return;}
            const DWORD wait=WaitForSingleObject(handle,0);acquired=wait==WAIT_OBJECT_0||wait==WAIT_ABANDONED;
            if(!acquired) {
                if(wait==WAIT_FAILED)Diag::Fail(L"WaitForSingleObject",name,GetLastError());
                else Diag::Fail(L"WaitForSingleObject",name,ERROR_BUSY,L"Cache lock is busy; wait="+std::to_wstring(wait),L"Policy");
            }
        }
        ~CacheLock(){if(acquired)ReleaseMutex(handle);if(handle)CloseHandle(handle);}
    } lock;
    if(!lock.acquired||!EnsureRoot())return false;
    const auto root=Root(),current=root+L"\\Current",previous=root+L"\\Previous",stage=root+L"\\Staging",retained=root+L"\\Retained";
    Diag::Step(L"prepare-retained",retained);
    if(!SecureDirectory(retained))return false;
    const auto exists=[](const std::wstring& path,bool& found) {
        found=GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES;
        if(found)return true;
        const DWORD error=GetLastError();
        return error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND?true:Diag::Fail(L"GetFileAttributesW",path,error);
    };
    Diag::Step(L"recover-previous",previous);
    bool hasCurrent=false,hasPrevious=false;
    if(!exists(current,hasCurrent)||!exists(previous,hasPrevious))return false;
    if(!hasCurrent&&hasPrevious) {
        if(!TrustedDirectory(previous))return false;
        if(!MoveFileExW(previous.c_str(),current.c_str(),MOVEFILE_WRITE_THROUGH))return Diag::Fail(L"MoveFileExW",previous,GetLastError(),L"Recovery target="+current);
        hasCurrent=true;
    }
    Diag::Step(L"prepare-staging",stage);
    if(!RemoveFlat(stage)||!SecureDirectory(stage))return false;
    // The immutable controller is committed before MSI work. Keep executable
    // publication here only for compatibility with older callers/installations.
    if(ControllerFile(L"GlassSetup.exe").empty()) {
        Diag::Step(L"copy-setup",stage+L"\\GlassSetup.exe");
        wchar_t exe[32768]{};const DWORD length=GetModuleFileNameW(nullptr,exe,32768);
        if(!length)return Diag::Fail(L"GetModuleFileNameW",L"current Setup",GetLastError());
        if(length>=32768)return Diag::Fail(L"GetModuleFileNameW",L"current Setup",ERROR_INSUFFICIENT_BUFFER);
        if(!CopyFileW(exe,(stage+L"\\GlassSetup.exe").c_str(),TRUE))return Diag::Fail(L"CopyFileW",stage+L"\\GlassSetup.exe",GetLastError(),L"source="+std::wstring(exe));
        const auto extract=[&](UINT id,const wchar_t* file) {
            const auto path=stage+L"\\"+file;
            Diag::Step(L"extract-resource",path);
            return GlassInstallUtil::ExtractResourceToFile(instance,id,RT_RCDATA,path)?true:
                Diag::Fail(L"ExtractResourceToFile",path,GetLastError(),L"resourceId="+std::to_wstring(id));
        };
        if(!extract(uninstallerResource,L"GlassUnins.exe"))return false;
    }
    for(std::size_t i=0;i<count;++i) {
        if(blobs[i].source) {
            const std::wstring source=blobs[i].source,destination=stage+L"\\"+blobs[i].file;
            if(!TrustedPath(source,false)||!CopyFileW(source.c_str(),destination.c_str(),TRUE))
                return Diag::Fail(L"CopyVerifiedSource",destination,GetLastError(),L"source="+source);
            if(blobs[i].verifiedSource || PublishedDigestMatches(source)) {
                if(!WritePublishedDigest(destination))return false;
            }
        } else if(!extract(blobs[i].id,blobs[i].file))return false;
    }
    Diag::Step(L"harden-staging",stage);
    WIN32_FIND_DATAW staged{};HANDLE stagedFind=FindFirstFileW((stage+L"\\*").c_str(),&staged);
    if(stagedFind==INVALID_HANDLE_VALUE)return Diag::Fail(L"FindFirstFileW",stage,GetLastError());
    bool secured=true;
    for(;;) {
        if(wcscmp(staged.cFileName,L".")!=0&&wcscmp(staged.cFileName,L"..")!=0) {
            const auto path=stage+L"\\"+staged.cFileName;
            if(staged.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) {
                secured=Diag::Fail(L"ValidateStagingChild",path,ERROR_ACCESS_DENIED,L"Directory/reparse child refused",L"Policy");break;
            }
            if(!HardenFile(path)){secured=false;break;}
        }
        if(!FindNextFileW(stagedFind,&staged)) {
            const DWORD error=GetLastError();if(error!=ERROR_NO_MORE_FILES)secured=Diag::Fail(L"FindNextFileW",stage,error);break;
        }
    }
    FindClose(stagedFind);if(!secured)return false;
    Diag::Step(L"retain-installed-sources",current);
    // Retain only the source of an actually installed, different old package.
    if(hasCurrent) {
        if(!TrustedDirectory(current))return false;
        for(std::size_t i=0;i<count;++i) {
            const auto old=current+L"\\"+blobs[i].file,newer=stage+L"\\"+blobs[i].file;
            bool found=false;if(!exists(old,found))return false;if(!found)continue;
            std::wstring code,version,newCode,newVersion;
            if(!TrustedPath(old,false)||!Identity(old,code,version)||!Identity(newer,newCode,newVersion))return false;
            if(RegistrationState(old,code,version)<0||RegistrationState(newer,newCode,newVersion)<0)return false;
            if(code==newCode&&RegistrationState(old,code,version)==1&&
               (version!=newVersion||_wcsicmp(PackageCode(old).c_str(),PackageCode(newer).c_str())!=0)) {
                const auto package=PackageCode(old);
                if(package.size()!=38)return Diag::Fail(L"ValidatePackageCode",old,ERROR_INVALID_DATA,package,L"Policy");
                for(wchar_t c:package)if(!iswxdigit(c)&&c!=L'{'&&c!=L'}'&&c!=L'-')return Diag::Fail(L"ValidatePackageCode",old,ERROR_INVALID_DATA,package,L"Policy");
                const auto dest=retained+L"\\"+code+L"-"+version+L"-"+package+L".msi";
                if(!CopyFileW(old.c_str(),dest.c_str(),TRUE)) {
                    const DWORD error=GetLastError();if(error!=ERROR_FILE_EXISTS)return Diag::Fail(L"CopyFileW",dest,error,L"source="+old);
                }
                if(!TrustedPath(dest,false))return false;
            }
        }
        Diag::Step(L"rotate-current-to-previous",current);
        if(!RemoveFlat(previous))return false;
        if(!MoveFileExW(current.c_str(),previous.c_str(),MOVEFILE_WRITE_THROUGH))return Diag::Fail(L"MoveFileExW",current,GetLastError(),L"target="+previous);
    }
    Diag::Step(L"activate-current",current);
    if(!MoveFileExW(stage.c_str(),current.c_str(),MOVEFILE_WRITE_THROUGH)) {
        const DWORD error=GetLastError();Diag::Fail(L"MoveFileExW",stage,error,L"target="+current);
        if(hasCurrent&&!MoveFileExW(previous.c_str(),current.c_str(),MOVEFILE_WRITE_THROUGH))
            Diag::Fail(L"MoveFileExW",previous,GetLastError(),L"Failed recovery target="+current);
        return false;
    }
    Diag::Step(L"register-repair-sources",current);
    bool registered=true;
    for(std::size_t i=0;i<count;++i)if(!RegisterRepairSource(current+L"\\"+blobs[i].file,needsOriginal))registered=false;
    WIN32_FIND_DATAW source{};HANDLE sources=FindFirstFileW((retained+L"\\*.msi").c_str(),&source);
    if(sources==INVALID_HANDLE_VALUE) {
        const DWORD error=GetLastError();if(error!=ERROR_FILE_NOT_FOUND)registered=Diag::Fail(L"FindFirstFileW",retained,error);
    } else {
        for(;;) {
            if(!RegisterRepairSource(retained+L"\\"+source.cFileName,needsOriginal))registered=false;
            if(!FindNextFileW(sources,&source)) {
                const DWORD error=GetLastError();if(error!=ERROR_NO_MORE_FILES)registered=Diag::Fail(L"FindNextFileW",retained,error);break;
            }
        }
        FindClose(sources);
    }
    // Do not release the old bundle on a real registration failure. Unbound
    // legacy candidates have already been copied/retained; missing coverage alone
    // does not pin an entire historical controller bundle indefinitely.
    if(!registered)return false;
    Diag::Step(L"release-previous",previous);
    const bool cleaned=RemoveFlat(previous);
    Diag::Step(L"garbage-collect-retained",retained);
    const bool gc=GarbageCollect();
    publication.succeeded=cleaned&&gc&&!publication.context.failed;
    return publication.succeeded;
}
}
