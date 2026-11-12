#pragma once
#include "GlassOpenGlassInstall.h"
#include "GlassMaintenanceCache.h"
#include "GlassOpenGlassFiles.h"

// A shared kernel file lease spans UI, MSI execution and handoff in every new
// Setup/Unins process. Retirement needs an exclusive lease across all sessions.
namespace GlassMaintenanceLifetime {
struct Lease {
    HANDLE handle=INVALID_HANDLE_VALUE;
    Lease()=default;
    Lease(const Lease&)=delete;
    Lease& operator=(const Lease&)=delete;
    ~Lease(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}
    bool Acquire(bool exclusive=false) {
        using namespace GlassMaintenanceCache;
        if(!EnsureRoot())return false;
        const auto path=Root()+L"\\Maintenance.lease";
        handle=CreateFileW(path.c_str(),GENERIC_READ|(exclusive?GENERIC_WRITE:0),exclusive?0:FILE_SHARE_READ,
            nullptr,OPEN_ALWAYS,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        const DWORD opened=GetLastError();
        if(handle==INVALID_HANDLE_VALUE)return false;
        // OPEN_ALWAYS reports ALREADY_EXISTS for an existing file. Never bless
        // its ACL merely because its name matches; harden only our new object.
        return (opened==ERROR_ALREADY_EXISTS||HardenFile(path))&&TrustedPath(path,false);
    }
};
inline bool HasProducts() {
    const wchar_t* codes[]={GlassInstallMsi::kProductPlatformX64,GlassInstallMsi::kProductPlatformX86,
        GlassInstallMsi::kProductSettingsX64,GlassInstallMsi::kProductSettingsX86,GlassInstallMsi::kProductOpenGlass,
        GlassInstallMsi::kProductSecureUxThemeX64,GlassInstallMsi::kProductSecureUxThemeX86,
        GlassInstallMsi::kLegacyPlatformX64,GlassInstallMsi::kLegacyPlatformX86,GlassInstallMsi::kLegacyOpenGlass};
    for(const auto* code:codes) {
        wchar_t product[39]{};
        const UINT result=MsiEnumProductsExW(code,L"S-1-1-0",MSIINSTALLCONTEXT_ALL,0,product,nullptr,nullptr,nullptr);
        if(result!=ERROR_NO_MORE_ITEMS&&result!=ERROR_UNKNOWN_PRODUCT)return true;
    }
    GlassInstallUtil::InstallRoots roots{};
    if(!GlassInstallUtil::ResolveInstallRoots(roots))return true;
    if(GlassOpenGlassFiles::HasReceipt(roots.openGlass)||GlassOpenGlassInstall::HasProduct())return true;
    return false; // Unknown enumeration is a reference, never absence.
}
// Explicit command, or successful last-product removal. No product/configuration
// deletion, network access, reboot-delete scheduling, or process termination here.
inline bool RetireUnused() {
    using namespace GlassMaintenanceCache;
    Lease exclusive;if(!exclusive.Acquire(true))return false;
    CacheLock cacheLock;if(!cacheLock.acquired||HasProducts()||GlassMaintenanceReboot::Pinned())return false;
    const auto store=Root()+L"\\Installer";
    if(GetFileAttributesW(store.c_str())==INVALID_FILE_ATTRIBUTES)return true;
    if(!TrustedDirectory(store))return false;
    // Revoke entrypoints before best-effort deletion. Locked code is now inert
    // CleanupPending, not an Active record pointing at a partly deleted pair.
    for(const auto* name:{L"Active",L"Previous",L"Pending",L"Confirmed",L"Rejected"}) {
        const auto path=store+L"\\"+name;
        if(GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES&&
           (!TrustedPath(path,false)||!DeleteFileW(path.c_str())))return false;
    }
    bool ok=true;
    WIN32_FIND_DATAW data{};HANDLE find=FindFirstFileW((store+L"\\*").c_str(),&data);
    if(find!=INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name=data.cFileName;
            if(name==L"."||name==L"..")continue;
            const std::string ascii(name.begin(),name.end());
            if((data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)&&!(data.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)&&
               (ControllerRecordValid(ascii+ascii)||name.rfind(L"Stage-",0)==0)) {
                if(!RemoveFlat(store+L"\\"+name))ok=false;
            } else if(!(data.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&
                      (name.rfind(L"Catalog-",0)==0||name.rfind(L"Record-",0)==0)) {
                const auto file=store+L"\\"+name;
                if(!TrustedPath(file,false)||!DeleteFileW(file.c_str()))ok=false;
            } else ok=false; // Unknown objects are not ours to delete.
        }while(FindNextFileW(find,&data));
        if(GetLastError()!=ERROR_NO_MORE_FILES)ok=false;
        FindClose(find);
    } else if(GetLastError()!=ERROR_FILE_NOT_FOUND)ok=false;
    for(const auto* name:{L"Current",L"Retained",L"Previous",L"Staging"}) {
        const auto folder=Root()+L"\\"+name;
        if(GetFileAttributesW(folder.c_str())!=INVALID_FILE_ATTRIBUTES&&!RemoveFlat(folder))ok=false;
    }
    GlassInstallLog::Line(ok?L"Unused maintenance controller retired":L"Unused maintenance controller CleanupPending; retry after closing old launchers");
    return ok;
}
}
