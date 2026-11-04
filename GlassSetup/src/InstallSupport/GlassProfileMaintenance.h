#pragma once
#include <windows.h>
#include <wtsapi32.h>
#include <sddl.h>
#include <string>
#include <vector>
#include <utility>
#include <RegistrySanity.h>
#include <SystemVisualStyle.h>
#pragma comment(lib,"Wtsapi32.lib")

namespace GlassProfileMaintenance {
struct Profile {
    std::wstring sid, path;
    HKEY hive=nullptr;
    Profile()=default;
    Profile(const Profile&)=delete;
    Profile& operator=(const Profile&)=delete;
    Profile(Profile&& other) noexcept : sid(std::move(other.sid)),path(std::move(other.path)),hive(other.hive) {other.hive=nullptr;}
    ~Profile(){if(hive)RegCloseKey(hive);}
};
inline bool ReadString(HKEY key,const wchar_t* name,std::wstring& out) {
    DWORD type=0,size=0;
    if(RegQueryValueExW(key,name,nullptr,&type,nullptr,&size)!=ERROR_SUCCESS ||
       (type!=REG_SZ&&type!=REG_EXPAND_SZ)||size>65536||size<sizeof(wchar_t))return false;
    std::vector<wchar_t> data(size/sizeof(wchar_t)+1,0);
    if(RegQueryValueExW(key,name,nullptr,&type,reinterpret_cast<BYTE*>(data.data()),&size)!=ERROR_SUCCESS)return false;
    out=data.data();return true;
}
inline bool SafePath(const std::wstring& path) {
    if(path.size()<3||path[1]!=L':'||path[2]!=L'\\')return false;
    for(std::size_t i=3;i<=path.size();++i) {
        if(i!=path.size()&&path[i]!=L'\\')continue;
        const DWORD attr=GetFileAttributesW(path.substr(0,i).c_str());
        if(attr==INVALID_FILE_ATTRIBUTES||(attr&FILE_ATTRIBUTE_REPARSE_POINT))return false;
    }
    return true;
}
inline bool OtherUserSessions(std::wstring& detail) {
    DWORD own=0; if(!ProcessIdToSessionId(GetCurrentProcessId(),&own)){detail=L"Cannot inspect current session";return true;}
    PWTS_SESSION_INFOW sessions=nullptr;DWORD count=0;
    if(!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE,0,1,&sessions,&count)){detail=L"Cannot enumerate user sessions";return true;}
    bool blocked=false;
    for(DWORD i=0;i<count;++i) {
        if(sessions[i].SessionId==own||sessions[i].SessionId==0)continue;
        LPWSTR user=nullptr;DWORD bytes=0;
        const bool ok=WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE,sessions[i].SessionId,WTSUserName,&user,&bytes)!=FALSE;
        if(!ok||(user&&*user)){detail=L"Close other user sessions and retry (session "+std::to_wstring(sessions[i].SessionId)+L")";blocked=true;}
        if(user)WTSFreeMemory(user);
        if(blocked)break;
    }
    WTSFreeMemory(sessions);return blocked;
}
class Inventory {
    std::vector<Profile> profiles_;
public:
    const std::vector<Profile>& Profiles() const noexcept{return profiles_;}
    // All hives stay open between preflight and commit. We never unload a system
    // mount; RegLoadAppKey handles unload only our private offline mapping.
    bool OpenAll(std::wstring& detail) {
        profiles_.clear();
        HKEY list=nullptr;
        LONG st=RegOpenKeyExW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList",0,KEY_READ|KEY_WOW64_64KEY,&list);
        if(st!=ERROR_SUCCESS){detail=L"Cannot read ProfileList: "+std::to_wstring(st);return false;}
        bool ok=true;
        for(DWORD i=0;;++i) {
            wchar_t name[256]{};DWORD length=256;
            st=RegEnumKeyExW(list,i,name,&length,nullptr,nullptr,nullptr,nullptr);
            if(st==ERROR_NO_MORE_ITEMS)break;
            if(st!=ERROR_SUCCESS){detail=L"Cannot enumerate profiles";ok=false;break;}
            const std::wstring sid=name;
            if(sid.rfind(L"S-1-5-21-",0)!=0&&sid.rfind(L"S-1-12-1-",0)!=0)continue;
            PSID binary=nullptr;
            if(!ConvertStringSidToSidW(name,&binary)){detail=L"Invalid profile SID";ok=false;break;}
            LocalFree(binary);
            HKEY entry=nullptr;
            if(RegOpenKeyExW(list,name,0,KEY_READ,&entry)!=ERROR_SUCCESS){detail=sid+L": profile entry unavailable";ok=false;break;}
            std::wstring raw;const bool got=ReadString(entry,L"ProfileImagePath",raw);RegCloseKey(entry);
            wchar_t expanded[32768]{};
            const DWORD chars=got?ExpandEnvironmentStringsW(raw.c_str(),expanded,32768):0;
            if(!chars||chars>32768){detail=sid+L": invalid profile path";ok=false;break;}
            Profile profile;profile.sid=sid;profile.path=expanded;
            st=RegOpenKeyExW(HKEY_USERS,name,0,KEY_READ|KEY_WRITE,&profile.hive);
            if(st!=ERROR_SUCCESS&&st!=ERROR_FILE_NOT_FOUND){detail=sid+L": loaded hive unavailable: "+std::to_wstring(st);ok=false;break;}
            if(!profile.hive) {
                const std::wstring file=profile.path+L"\\NTUSER.DAT";
                DWORD attr=GetFileAttributesW(file.c_str());
                if(attr==INVALID_FILE_ATTRIBUTES){detail=sid+L": NTUSER.DAT unavailable";ok=false;break;}
                if(!SafePath(file)){detail=sid+L": unsafe or unavailable NTUSER.DAT";ok=false;break;}
                st=RegLoadAppKeyW(file.c_str(),&profile.hive,KEY_ALL_ACCESS,REG_PROCESS_APPKEY,0);
                if(st!=ERROR_SUCCESS){detail=sid+L": cannot open offline hive: "+std::to_wstring(st);ok=false;break;}
            }
            profiles_.push_back(std::move(profile));
        }
        RegCloseKey(list);return ok;
    }
    // Conservative safety barrier until a per-session style broker and verified
    // offline theme writer are available. Unknown/custom offline styles do not
    // authorize hook removal. No logout or registry reset is performed here.
    bool PreflightStyles(std::wstring& detail) {
        if(OtherUserSessions(detail)){detail=L"Cannot verify the actual active style of another session: "+detail;return false;}
        if(Shared::SystemVisualStyle::Current()==Shared::SystemVisualStyle::State::Unknown){detail=L"Actual current visual style is unknown; no reset is authorized";return false;}
        if(!OpenAll(detail))return false;
        HANDLE token=nullptr;
        if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)){detail=L"Cannot identify current user";return false;}
        DWORD bytes=0;GetTokenInformation(token,TokenUser,nullptr,0,&bytes);
        std::vector<BYTE> data(bytes);const bool read=bytes&&GetTokenInformation(token,TokenUser,data.data(),bytes,&bytes);CloseHandle(token);
        if(!read){detail=L"Cannot read current user SID";return false;}
        LPWSTR sid=nullptr;
        if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid,&sid)){detail=L"Cannot decode current user SID";return false;}
        const std::wstring own=sid;LocalFree(sid);
        DWORD session=0;
        if(!ProcessIdToSessionId(GetCurrentProcessId(),&session)){detail=L"Cannot identify interactive session";return false;}
        if(session!=0) {
            LPWSTR user=nullptr,domain=nullptr;DWORD userBytes=0,domainBytes=0;
            const bool gotUser=WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE,session,WTSUserName,&user,&userBytes)!=FALSE;
            const bool gotDomain=WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE,session,WTSDomainName,&domain,&domainBytes)!=FALSE;
            std::wstring account;
            if(gotUser&&gotDomain&&user&&*user)account=(domain&&*domain?std::wstring(domain)+L"\\":L"")+user;
            if(user)WTSFreeMemory(user);if(domain)WTSFreeMemory(domain);
            if(account.empty()){detail=L"Cannot identify the desktop user";return false;}
            DWORD sidSize=0,domainSize=0;SID_NAME_USE use{};
            LookupAccountNameW(nullptr,account.c_str(),nullptr,&sidSize,nullptr,&domainSize,&use);
            std::vector<BYTE> sessionSid(sidSize);std::vector<wchar_t> sessionDomain(domainSize);
            if(!sidSize||!LookupAccountNameW(nullptr,account.c_str(),sessionSid.data(),&sidSize,sessionDomain.data(),&domainSize,&use)||
               !EqualSid(sessionSid.data(),reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid)) {
                detail=L"Elevated account differs from desktop user; visual-style reset requires that user's maintenance session";return false;
            }
        }
        for(const auto& profile:profiles_) {
            if(profile.sid==own)continue; // Native helper assesses the actual current style before any reset.
            HKEY theme=nullptr;
            const LONG status=RegOpenKeyExW(profile.hive,L"Software\\Microsoft\\Windows\\CurrentVersion\\ThemeManager",0,KEY_READ,&theme);
            if(status!=ERROR_SUCCESS){detail=profile.sid+L": offline visual style is unknown";return false;}
            std::wstring path;const bool got=ReadString(theme,L"DllName",path);RegCloseKey(theme);
            if(!got||Shared::SystemVisualStyle::ClassifyPath(path.c_str())!=Shared::SystemVisualStyle::State::Stock) {
                detail=profile.sid+L": custom/unknown offline style; verified offline reset is required before SecureUxTheme removal";return false;
            }
        }
        return true;
    }
    bool PreflightCleanup(std::wstring& detail) {
        if(OtherUserSessions(detail)||!OpenAll(detail))return false;
        for(const auto& profile:profiles_)
            if(!Shared::RegistrySanity::CleanOwnedSettings(profile.hive,false,false,detail)){detail=profile.sid+L": "+detail;return false;}
        return Shared::RegistrySanity::CleanOwnedSettings(HKEY_LOCAL_MACHINE,true,false,detail);
    }
    bool Cleanup(std::wstring& detail) {
        if(OtherUserSessions(detail))return false;
        for(const auto& profile:profiles_) {
            if(!Shared::RegistrySanity::CleanOwnedSettings(profile.hive,false,true,detail)){detail=profile.sid+L": partial cleanup: "+detail;return false;}
        }
        return Shared::RegistrySanity::CleanOwnedSettings(HKEY_LOCAL_MACHINE,true,true,detail);
    }
};
}
