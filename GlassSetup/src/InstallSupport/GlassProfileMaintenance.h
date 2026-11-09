#pragma once
#include <windows.h>
#include <wtsapi32.h>
#include <sddl.h>
#include <ntsecapi.h>
#include <cstring>
#include <string>
#include <vector>
#include <utility>
#include <RegistrySanity.h>
#include <SystemVisualStyle.h>
#include "GlassSavedThemeCleanup.h"
#include "GlassOfflineUserHive.h"
#pragma comment(lib,"Wtsapi32.lib")

namespace GlassProfileMaintenance {
// Only a confirmed missing account from this machine's SAM may be ignored.
// Domain lookup failures and cloud identities are never proof of deletion.
inline bool DeletedLocalProfile(const std::wstring& text) {
    PSID sid=nullptr;if(!ConvertStringSidToSidW(text.c_str(),&sid))return false;
    struct FreeSid {PSID p;~FreeSid(){LocalFree(p);}} free{sid};
    LSA_OBJECT_ATTRIBUTES attributes{};attributes.Length=sizeof(attributes);
    LSA_HANDLE policy=nullptr;
    if(LsaOpenPolicy(nullptr,&attributes,POLICY_VIEW_LOCAL_INFORMATION,&policy)!=0)return false;
    PPOLICY_ACCOUNT_DOMAIN_INFO info=nullptr;
    const auto status=LsaQueryInformationPolicy(policy,PolicyAccountDomainInformation,reinterpret_cast<void**>(&info));
    LsaClose(policy);
    if(status!=0||!info)return false;
    bool local=false;
    if(info->DomainSid&&IsValidSid(info->DomainSid)&&IsValidSid(sid)) {
        const BYTE count=*GetSidSubAuthorityCount(info->DomainSid);
        local=*GetSidSubAuthorityCount(sid)==count+1 &&
            std::memcmp(GetSidIdentifierAuthority(sid),GetSidIdentifierAuthority(info->DomainSid),sizeof(SID_IDENTIFIER_AUTHORITY))==0;
        for(BYTE i=0;local&&i<count;++i)local=*GetSidSubAuthority(sid,i)==*GetSidSubAuthority(info->DomainSid,i);
    }
    LsaFreeMemory(info);
    if(!local){GlassInstallLog::LineFormat(L"Profile account: %s is not confirmed machine-local SAM",text.c_str());return false;}
    DWORD nameSize=0,domainSize=0;SID_NAME_USE use{};
    SetLastError(ERROR_SUCCESS);
    const bool mapped=LookupAccountSidW(nullptr,sid,nullptr,&nameSize,nullptr,&domainSize,&use)!=FALSE;
    const DWORD lookup=GetLastError();
    GlassInstallLog::LineFormat(L"Profile account: %s machine-local=1 lookup=%lu deleted=%d",text.c_str(),lookup,!mapped&&lookup==ERROR_NONE_MAPPED?1:0);
    return !mapped&&lookup==ERROR_NONE_MAPPED;
}
struct Profile {
    std::wstring sid, path, ownedMount;
    HKEY hive=nullptr;
    GlassSavedThemeCleanup::Batch savedThemes;
    Profile()=default;
    Profile(const Profile&)=delete;
    Profile& operator=(const Profile&)=delete;
    Profile(Profile&& other) noexcept : sid(std::move(other.sid)),path(std::move(other.path)),ownedMount(std::move(other.ownedMount)),hive(other.hive),savedThemes(std::move(other.savedThemes)) {other.hive=nullptr;other.ownedMount.clear();}
    ~Profile(){if(hive)RegCloseKey(hive);if(!ownedMount.empty())GlassOfflineUserHive::Unmount(ownedMount);}
};
inline bool ReadString(HKEY key,const wchar_t* name,std::wstring& out) {
    out.clear();
    DWORD type=0,size=0;
    if(RegQueryValueExW(key,name,nullptr,&type,nullptr,&size)!=ERROR_SUCCESS ||
       (type!=REG_SZ&&type!=REG_EXPAND_SZ)||size>65536||size<sizeof(wchar_t)||size%sizeof(wchar_t))return false;
    std::vector<wchar_t> data(size/sizeof(wchar_t)+1,0);
    if(RegQueryValueExW(key,name,nullptr,&type,reinterpret_cast<BYTE*>(data.data()),&size)!=ERROR_SUCCESS ||
       (type!=REG_SZ&&type!=REG_EXPAND_SZ)||size<sizeof(wchar_t)||size%sizeof(wchar_t))return false;
    const std::size_t chars=size/sizeof(wchar_t);
    if(chars>=data.size() || data[chars-1]!=0)return false;
    // Embedded NUL/trailing payload must not truncate a profile path silently.
    for(std::size_t i=0;i+1<chars;++i)if(data[i]==0)return false;
    out.assign(data.data(),chars-1);return !out.empty();
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
inline bool IsDesktopProfile(const std::wstring& sid) {
    DWORD session=0;
    if(!ProcessIdToSessionId(GetCurrentProcessId(),&session)||!session)return false;
    LPWSTR user=nullptr,domain=nullptr;DWORD userSize=0,domainSize=0;
    const bool gotUser=WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE,session,WTSUserName,&user,&userSize)!=FALSE;
    const bool gotDomain=WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE,session,WTSDomainName,&domain,&domainSize)!=FALSE;
    std::wstring account;
    if(gotUser&&gotDomain&&user&&*user)account=(domain&&*domain?std::wstring(domain)+L"\\":L"")+user;
    if(user)WTSFreeMemory(user);if(domain)WTSFreeMemory(domain);
    if(account.empty())return false;
    DWORD bytes=0,chars=0;SID_NAME_USE use{};
    LookupAccountNameW(nullptr,account.c_str(),nullptr,&bytes,nullptr,&chars,&use);
    std::vector<BYTE> desktop(bytes);std::vector<wchar_t> desktopDomain(chars);
    if(!bytes||!LookupAccountNameW(nullptr,account.c_str(),desktop.data(),&bytes,desktopDomain.data(),&chars,&use))return false;
    LPWSTR text=nullptr;
    if(!ConvertSidToStringSidW(desktop.data(),&text))return false;
    const bool matching=sid==text;LocalFree(text);if(!matching)return false;
    HANDLE token=nullptr;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return false;
    DWORD size=0;GetTokenInformation(token,TokenUser,nullptr,0,&size);
    std::vector<BYTE> process(size);
    const bool got=size&&GetTokenInformation(token,TokenUser,process.data(),size,&size);CloseHandle(token);
    return got&&EqualSid(desktop.data(),reinterpret_cast<TOKEN_USER*>(process.data())->User.Sid);
}
class Inventory {
    std::vector<Profile> profiles_;
    bool skipDeletedLocal_=false;
    bool cleanupPrepared_=false;
    bool removalStylePrepared_=false;
    bool removalStyleCommitted_=false;
    bool savedThemeRegistryFallback_=false;
public:
    bool SavedThemeRegistryFallback() const noexcept{return savedThemeRegistryFallback_;}
    const std::vector<Profile>& Profiles() const noexcept{return profiles_;}
    // Hives stay open between preflight and commit. Existing SID mounts are
    // never unloaded; private app hives/owned fallback mounts have scoped cleanup.
    bool OpenAll(std::wstring& detail,bool skipDeletedLocal=false) {
        skipDeletedLocal_=skipDeletedLocal;
        cleanupPrepared_=false;
        removalStylePrepared_=false;removalStyleCommitted_=false;
        savedThemeRegistryFallback_=false;
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
            if(skipDeletedLocal_&&DeletedLocalProfile(sid)){GlassInstallLog::LineFormat(L"SecureUxTheme profile: deleted local-account residue ignored: %s",sid.c_str());continue;}
            HKEY entry=nullptr;
            if(RegOpenKeyExW(list,name,0,KEY_READ,&entry)!=ERROR_SUCCESS){detail=sid+L": profile entry unavailable";ok=false;break;}
            std::wstring raw;const bool got=ReadString(entry,L"ProfileImagePath",raw);RegCloseKey(entry);
            wchar_t expanded[32768]{};
            const DWORD chars=got?ExpandEnvironmentStringsW(raw.c_str(),expanded,32768):0;
            if(!chars||chars>32768){detail=sid+L": invalid profile path";ok=false;break;}
            // ProfileList is the inventory, not account-name lookup: deleted,
            // disconnected-domain and temporarily unresolvable users can still
            // own local data. Never silently exclude their hives.
            const DWORD pathAttr=GetFileAttributesW(expanded);
            if(pathAttr==INVALID_FILE_ATTRIBUTES || !(pathAttr&FILE_ATTRIBUTE_DIRECTORY) ||
               (pathAttr&FILE_ATTRIBUTE_REPARSE_POINT) || !SafePath(expanded)) {
                detail=sid+L": profile directory unavailable or unsafe";ok=false;break;
            }
            const std::wstring ntuser=std::wstring(expanded)+L"\\NTUSER.DAT";
            const DWORD hiveAttr=GetFileAttributesW(ntuser.c_str());
            if(hiveAttr==INVALID_FILE_ATTRIBUTES || (hiveAttr&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) ||
               !SafePath(ntuser)) {
                detail=sid+L": NTUSER.DAT unavailable or unsafe";ok=false;break;
            }
            Profile profile;profile.sid=sid;profile.path=expanded;
            st=RegOpenKeyExW(HKEY_USERS,name,0,KEY_READ|KEY_WRITE,&profile.hive);
            if(st!=ERROR_SUCCESS&&st!=ERROR_FILE_NOT_FOUND){detail=sid+L": loaded hive unavailable: "+std::to_wstring(st);ok=false;break;}
            if(!profile.hive) {
                const std::wstring file=profile.path+L"\\NTUSER.DAT";
                DWORD attr=GetFileAttributesW(file.c_str());
                if(attr==INVALID_FILE_ATTRIBUTES){detail=sid+L": NTUSER.DAT unavailable";ok=false;break;}
                if(!SafePath(file)){detail=sid+L": unsafe or unavailable NTUSER.DAT";ok=false;break;}
                st=RegLoadAppKeyW(file.c_str(),&profile.hive,KEY_ALL_ACCESS,REG_PROCESS_APPKEY,0);
                if(st!=ERROR_SUCCESS){
                    GlassInstallLog::LineFormat(L"Offline user hive: SID=%s path=%s RegLoadAppKey=%ld",sid.c_str(),file.c_str(),st);
                    std::wstring fallback;
                    if(st!=ERROR_BADDB||!skipDeletedLocal_||!GlassOfflineUserHive::LoadMounted(file,profile.hive,profile.ownedMount,fallback)) {
                        detail=sid+L": cannot open offline hive: RegLoadAppKey="+std::to_wstring(st);
                        if(!fallback.empty())detail+=L"; "+fallback;
                        ok=false;break;
                    }
                }
            }
            profiles_.push_back(std::move(profile));
        }
        RegCloseKey(list);
        if(!ok) profiles_.clear(); // A partial inventory must never become a cleanup target.
        return ok;
    }
    // Re-enumerate metadata without reopening NTUSER.DAT: existing private
    // hive handles remain pinned. Enumeration order is not an identity.
    bool VerifyInventory(std::wstring& detail) const {
        HKEY list=nullptr;
        const LONG opened=RegOpenKeyExW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList",0,KEY_READ|KEY_WOW64_64KEY,&list);
        if(opened!=ERROR_SUCCESS){detail=L"Cannot recheck ProfileList: "+std::to_wstring(opened);return false;}
        struct CloseKey { HKEY key; ~CloseKey(){RegCloseKey(key);} } close{list};
        std::vector<bool> seen(profiles_.size(),false);
        for(DWORD index=0;;++index) {
            wchar_t name[256]{};DWORD length=256;
            const LONG status=RegEnumKeyExW(list,index,name,&length,nullptr,nullptr,nullptr,nullptr);
            if(status==ERROR_NO_MORE_ITEMS)break;
            if(status!=ERROR_SUCCESS){detail=L"Cannot re-enumerate profiles";return false;}
            const std::wstring sid=name;
            if(sid.rfind(L"S-1-5-21-",0)!=0&&sid.rfind(L"S-1-12-1-",0)!=0)continue;
            if(skipDeletedLocal_&&DeletedLocalProfile(sid))continue;
            std::size_t found=0;
            while(found<profiles_.size() && profiles_[found].sid!=sid)++found;
            if(found==profiles_.size() || seen[found]) {
                detail=sid+L": profile inventory changed; a fresh preflight is required";return false;
            }
            HKEY entry=nullptr;
            if(RegOpenKeyExW(list,name,0,KEY_READ,&entry)!=ERROR_SUCCESS){detail=sid+L": profile entry unavailable during recheck";return false;}
            std::wstring raw;
            const bool got=ReadString(entry,L"ProfileImagePath",raw);RegCloseKey(entry);
            wchar_t expanded[32768]{};
            const DWORD chars=got?ExpandEnvironmentStringsW(raw.c_str(),expanded,32768):0;
            if(!chars || chars>32768 || profiles_[found].path!=expanded) {
                detail=sid+L": profile path changed or unavailable; a fresh preflight is required";return false;
            }
            const DWORD pathAttr=GetFileAttributesW(expanded);
            const std::wstring hivePath=profiles_[found].path+L"\\NTUSER.DAT";
            const DWORD hiveAttr=GetFileAttributesW(hivePath.c_str());
            if(pathAttr==INVALID_FILE_ATTRIBUTES || !(pathAttr&FILE_ATTRIBUTE_DIRECTORY) ||
               (pathAttr&FILE_ATTRIBUTE_REPARSE_POINT) || !SafePath(expanded) ||
               hiveAttr==INVALID_FILE_ATTRIBUTES || (hiveAttr&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) ||
               !SafePath(hivePath)) {
                detail=sid+L": profile paths became unavailable or unsafe";return false;
            }
            seen[found]=true;
        }
        for(bool present:seen)if(!present){detail=L"A profile disappeared; a fresh preflight is required";return false;}
        return true;
    }
    // Persist stock selectors for every inventoried profile, including offline
    // hives. Other live sessions finish switching at restart; no logoff is forced.
    bool PreflightLoadedRemoval(std::wstring& detail) {
        removalStylePrepared_=false;removalStyleCommitted_=false;
        if(!OpenAll(detail,true))return false;
        if(profiles_.empty()){detail=L"No user profiles could be inventoried.";return false;}
        for(const auto& profile:profiles_) {
            for(const auto* path:{L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes",L"Software\\Microsoft\\Windows\\CurrentVersion\\ThemeManager"}) {
                HKEY key=nullptr;
                const LONG status=RegOpenKeyExW(profile.hive,path,0,KEY_READ|KEY_WRITE,&key);
                if(key)RegCloseKey(key);
                if(status!=ERROR_SUCCESS&&status!=ERROR_FILE_NOT_FOUND){detail=profile.sid+L": theme selector is not writable ("+std::to_wstring(status)+L")";return false;}
            }
        }
        removalStylePrepared_=VerifyInventory(detail);return removalStylePrepared_;
    }
    bool StockRemovalPaths(std::wstring& theme,std::wstring& style,std::wstring& detail) const {
        wchar_t windows[32768]{};const UINT n=GetWindowsDirectoryW(windows,32768);
        if(!n||n>=32768){detail=L"Cannot resolve the Windows directory.";return false;}
        theme=std::wstring(windows)+L"\\Resources\\Themes\\aero.theme";
        style=std::wstring(windows)+L"\\Resources\\Themes\\Aero\\aero.msstyles";
        if(!SafePath(theme)||!SafePath(style)){detail=L"Windows stock theme/style files are missing or inaccessible.";return false;}
        return true;
    }
    bool VerifyLoadedRemovalStock(std::wstring& detail) const {
        std::wstring theme,style;if(!StockRemovalPaths(theme,style,detail))return false;
        wchar_t actual[32768]{},color[256]{},size[256]{};
        const auto lite=style.substr(0,style.find_last_of(L'\\')+1)+L"aerolite.msstyles";
        if(FAILED(GetCurrentThemeName(actual,32768,color,256,size,256))||
            (_wcsicmp(actual,style.c_str())&&_wcsicmp(actual,lite.c_str()))){detail=L"Current session did not switch to the Windows stock visual style.";return false;}
        for(const auto& profile:profiles_) {
            const struct {const wchar_t* path;const wchar_t* name;const std::wstring* expected;} values[]={
                {L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes",L"CurrentTheme",&theme},
                {L"Software\\Microsoft\\Windows\\CurrentVersion\\ThemeManager",L"DllName",&style}};
            for(const auto& value:values) {
                HKEY key=nullptr;
                const LONG status=RegOpenKeyExW(profile.hive,value.path,0,KEY_READ,&key);
                std::wstring current;const bool ok=status==ERROR_SUCCESS&&ReadString(key,value.name,current)&&!_wcsicmp(current.c_str(),value.expected->c_str());
                if(key)RegCloseKey(key);
                if(!ok){detail=profile.sid+L": stock selector verification failed: "+value.name;return false;}
            }
        }
        return true;
    }
    bool RecheckLoadedRemoval(std::wstring& detail) const {
        if(!removalStylePrepared_){detail=L"Loaded-removal profile preflight is required.";return false;}
        if(!VerifyInventory(detail))return false;
        return !removalStyleCommitted_||VerifyLoadedRemovalStock(detail);
    }
    // Native helper switches the current session only. These writes deliberately
    // leave wallpaper, sounds, colors and custom .theme/.msstyles files untouched.
    // Partial failure is not rolled back into an unsafe custom style.
    bool CommitLoadedRemovalStyle(std::wstring& detail) {
        if(removalStyleCommitted_){detail=L"Style preparation already committed; a fresh preflight is required.";return false;}
        if(!RecheckLoadedRemoval(detail))return false;
        removalStylePrepared_=false; // One-shot even after a partial write.
        std::wstring theme,style;if(!StockRemovalPaths(theme,style,detail))return false;
        for(const auto& profile:profiles_) {
            const struct {const wchar_t* path;const wchar_t* name;const std::wstring* target;} values[]={
                {L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes",L"CurrentTheme",&theme},
                {L"Software\\Microsoft\\Windows\\CurrentVersion\\ThemeManager",L"DllName",&style}};
            for(const auto& value:values) {
                HKEY key=nullptr;
                LONG status=RegCreateKeyExW(profile.hive,value.path,0,nullptr,0,KEY_READ|KEY_WRITE,nullptr,&key,nullptr);
                if(status==ERROR_SUCCESS)status=RegSetValueExW(key,value.name,0,REG_SZ,reinterpret_cast<const BYTE*>(value.target->c_str()),static_cast<DWORD>((value.target->size()+1)*sizeof(wchar_t)));
                std::wstring current;
                const bool verified=status==ERROR_SUCCESS&&ReadString(key,value.name,current)&&!_wcsicmp(current.c_str(),value.target->c_str());
                if(key)RegCloseKey(key);
                if(!verified){detail=profile.sid+L": cannot write/verify "+value.name+L" ("+std::to_wstring(status)+L"). Some profile selectors or the live style may already be reset.";return false;}
            }
        }
        if(!VerifyInventory(detail)||!VerifyLoadedRemovalStock(detail))return false;
        removalStylePrepared_=true;removalStyleCommitted_=true;return true;
    }
    // Conservative safety barrier until a per-session style broker and verified
    // offline theme writer are available. Unknown/custom offline styles do not
    // authorize hook removal. No logout or registry reset is performed here.
    bool PreflightStyles(std::wstring& detail) {
        if(OtherUserSessions(detail)){detail=L"Cannot verify the actual active style of another session: "+detail;return false;}
        if(Shared::SystemVisualStyle::CurrentForMaintenance()==Shared::SystemVisualStyle::State::Unknown){detail=L"Actual current visual style is unknown; no reset is authorized";return false;}
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
            if(!got||!Shared::SystemVisualStyle::IsStockMsStylesFileName(path.c_str())) {
                detail=profile.sid+L": custom/unknown offline style; verified offline reset is required before SecureUxTheme removal";return false;
            }
        }
        return true;
    }
    bool PreflightCleanup(std::wstring& detail) {
        cleanupPrepared_=false;
        if(OtherUserSessions(detail)||!OpenAll(detail))return false;
        for(auto& profile:profiles_) {
            if(!profile.savedThemes.Prepare(profile.path,profile.hive,SafePath,detail) ||
               !Shared::RegistrySanity::CleanOwnedSettings(profile.hive,false,false,detail)){detail=profile.sid+L": "+detail;return false;}
        }
        cleanupPrepared_=Shared::RegistrySanity::CleanOwnedSettings(HKEY_LOCAL_MACHINE,true,false,detail) && VerifyInventory(detail);
        return cleanupPrepared_;
    }
    bool Cleanup(std::wstring& detail) {
        if(!cleanupPrepared_) {detail=L"Successful all-profile cleanup preflight is required";return false;}
        cleanupPrepared_=false; // One-shot commit; partial errors require a fresh inventory/preflight.
        if(OtherUserSessions(detail)||!VerifyInventory(detail))return false;
        savedThemeRegistryFallback_=false;
        for(auto& profile:profiles_) {
            if(!profile.savedThemes.Cleanup(profile.hive,IsDesktopProfile(profile.sid),savedThemeRegistryFallback_,detail)) {
                detail=profile.sid+L": partial saved-theme cleanup: "+detail;return false;
            }
            if(!Shared::RegistrySanity::CleanOwnedSettings(profile.hive,false,true,detail)){detail=profile.sid+L": partial cleanup: "+detail;return false;}
        }
        return Shared::RegistrySanity::CleanOwnedSettings(HKEY_LOCAL_MACHINE,true,true,detail);
    }
};
}
