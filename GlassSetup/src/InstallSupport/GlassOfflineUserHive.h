#pragma once
#include <windows.h>
#include <objbase.h>
#include <string>
#include "GlassInstallLog.h"

namespace GlassOfflineUserHive {
class Privilege {
    HANDLE token_=nullptr;TOKEN_PRIVILEGES previous_{};bool adjusted_=false,enabled_=false;
public:
    explicit Privilege(const wchar_t* name) noexcept {
        if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY|TOKEN_ADJUST_PRIVILEGES,&token_))return;
        TOKEN_PRIVILEGES request{};request.PrivilegeCount=1;
        if(!LookupPrivilegeValueW(nullptr,name,&request.Privileges[0].Luid))return;
        request.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;
        DWORD bytes=sizeof(previous_);SetLastError(ERROR_SUCCESS);
        adjusted_=AdjustTokenPrivileges(token_,FALSE,&request,sizeof(previous_),&previous_,&bytes)!=FALSE;
        enabled_=adjusted_&&GetLastError()==ERROR_SUCCESS;
    }
    ~Privilege(){if(token_){if(adjusted_)AdjustTokenPrivileges(token_,FALSE,&previous_,0,nullptr,nullptr);CloseHandle(token_);}}
    bool Enabled() const noexcept{return enabled_;}
    Privilege(const Privilege&)=delete;Privilege& operator=(const Privilege&)=delete;
};
inline bool Unmount(const std::wstring& mount) noexcept {
    if(mount.empty())return true;
    Privilege restore(SE_RESTORE_NAME);
    const LONG rc=restore.Enabled()?RegUnLoadKeyW(HKEY_USERS,mount.c_str()):ERROR_PRIVILEGE_NOT_HELD;
    GlassInstallLog::LineFormat(L"Offline user hive: release owned mount %s: %ld",mount.c_str(),rc);
    return rc==ERROR_SUCCESS;
}
// RegLoadAppKey can reject a normal NTUSER hive. Use the standard registry
// loader, with its normal transaction-log recovery, under a unique owned name.
// Never unload an existing SID mount and never repair/copy/delete profile files.
inline bool LoadMounted(const std::wstring& path,HKEY& hive,std::wstring& mount,std::wstring& detail) {
    hive=nullptr;mount.clear();
    Privilege backup(SE_BACKUP_NAME),restore(SE_RESTORE_NAME);
    if(!backup.Enabled()||!restore.Enabled()){detail=L"offline hive fallback requires Backup/Restore privileges";return false;}
    GUID guid{};wchar_t text[64]{};
    if(FAILED(CoCreateGuid(&guid))||!StringFromGUID2(guid,text,64)){detail=L"cannot allocate private offline-hive mount name";return false;}
    const std::wstring name=std::wstring(L"GlassSetup-Offline-")+text;
    mount=name; // Allocate before acquiring the mount.
    const LONG loaded=RegLoadKeyW(HKEY_USERS,name.c_str(),path.c_str());
    GlassInstallLog::LineFormat(L"Offline user hive: RegLoadKey path=%s owned mount=%s rc=%ld",path.c_str(),name.c_str(),loaded);
    if(loaded!=ERROR_SUCCESS){mount.clear();detail=L"RegLoadKey="+std::to_wstring(loaded);return false;}
    // Ownership is acquired only by a successful load.
    const LONG opened=RegOpenKeyExW(HKEY_USERS,name.c_str(),0,KEY_READ|KEY_WRITE,&hive);
    if(opened!=ERROR_SUCCESS){
        detail=L"opening owned offline-hive mount="+std::to_wstring(opened);
        if(Unmount(mount))mount.clear(); // Otherwise owner will retry cleanup.
        return false;
    }
    return true;
}
}
