#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <utility>
#include <Helpers/ThemeHelper.h>
#include <uxtheme.h>

// Existing Setup/Unins code only; no new executable or helper DLL.
namespace GlassSavedThemeCleanup {
// Section recognition is deliberately bounded and independent of Win32 INI's
// encoding heuristics. Non-ASCII text cannot manufacture an ASCII Glass header.
inline bool HasGlassSection(const std::vector<unsigned char>& bytes,bool& valid) {
    valid=false;
    if(bytes.size()>4*1024*1024)return false;
    std::wstring text;
    const bool le=bytes.size()>=2 && bytes[0]==255 && bytes[1]==254;
    const bool be=bytes.size()>=2 && bytes[0]==254 && bytes[1]==255;
    std::size_t offset=le||be?2:bytes.size()>=3&&bytes[0]==239&&bytes[1]==187&&bytes[2]==191?3:0;
    if((le||be) && (bytes.size()-offset)%2)return false;
    for(std::size_t i=offset;i<bytes.size();i+=le||be?2:1) {
        unsigned value=le?bytes[i]|(unsigned(bytes[i+1])<<8):be?(unsigned(bytes[i])<<8)|bytes[i+1]:bytes[i];
        if(!value)return false;
        text+=value<128?static_cast<wchar_t>(value):L'?';
    }
    valid=true;
    for(std::size_t start=0;start<text.size();) {
        auto end=text.find_first_of(L"\r\n",start);if(end==std::wstring::npos)end=text.size();
        auto line=text.substr(start,end-start);start=end+1;
        auto first=line.find_first_not_of(L" \t");if(first==std::wstring::npos || line[first]!=L'[')continue;
        auto close=line.find(L']',first+1);if(close==std::wstring::npos)continue;
        auto tail=line.find_first_not_of(L" \t",close+1);
        if(tail!=std::wstring::npos && line[tail]!=L';' && line[tail]!=L'#')continue;
        auto section=line.substr(first+1,close-first-1);
        auto a=section.find_first_not_of(L" \t"),b=section.find_last_not_of(L" \t");
        if(a==std::wstring::npos)continue;
        section=section.substr(a,b-a+1);
        for(auto& c:section)if(c>=L'A'&&c<=L'Z')c+=L'a'-L'A';
        if(section==L"glass" || (section.size()>6 && section.compare(0,6,L"glass.")==0))return true;
    }
    return false;
}
struct File {
    std::wstring path;
    HANDLE handle=INVALID_HANDLE_VALUE,seal=INVALID_HANDLE_VALUE;
    File()=default;
    File(const File&)=delete;File& operator=(const File&)=delete;
    File(File&& other) noexcept:path(std::move(other.path)),handle(other.handle),seal(other.seal){other.handle=other.seal=INVALID_HANDLE_VALUE;}
    ~File(){if(seal!=INVALID_HANDLE_VALUE)CloseHandle(seal);if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}
};
class Batch {
    std::vector<File> files_;
    std::wstring profile_,localAppData_;
    bool prepared_=false;
    bool (*safe_)(const std::wstring&)=nullptr;
    static bool Fail(std::wstring& detail,const std::wstring& path){detail=L"Saved theme cleanup: "+path;return false;}
    static bool SameFile(HANDLE a,HANDLE b,bool& same) {
        BY_HANDLE_FILE_INFORMATION x{},y{};same=false;
        if(!GetFileInformationByHandle(a,&x)||!GetFileInformationByHandle(b,&y))return false;
        same=x.dwVolumeSerialNumber==y.dwVolumeSerialNumber && x.nFileIndexHigh==y.nFileIndexHigh && x.nFileIndexLow==y.nFileIndexLow;
        return true;
    }
    bool ActiveOwned(HKEY hive,bool& owned,std::wstring& detail) const {
        owned=false;if(files_.empty())return true;
        wchar_t raw[32768]{};DWORD size=sizeof(raw);
        LONG result=RegGetValueW(hive,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes",L"CurrentTheme",
            RRF_RT_REG_SZ|RRF_RT_REG_EXPAND_SZ|RRF_NOEXPAND,nullptr,raw,&size);
        if(result==ERROR_FILE_NOT_FOUND)return true;
        if(result!=ERROR_SUCCESS || size<sizeof(wchar_t) || size>sizeof(raw) || size%sizeof(wchar_t) || raw[size/sizeof(wchar_t)-1]!=0)
            return Fail(detail,L"CurrentTheme unavailable");
        for(DWORD i=0;i+1<size/sizeof(wchar_t);++i)if(!raw[i])return Fail(detail,L"CurrentTheme contains embedded NUL");
        std::wstring value=raw;
        for(const auto& variable:{std::make_pair(L"%LOCALAPPDATA%",localAppData_),
            std::make_pair(L"%APPDATA%",profile_+L"\\AppData\\Roaming"),std::make_pair(L"%USERPROFILE%",profile_)}) {
            const std::wstring token=variable.first;
            if(value.size()>=token.size() && _wcsnicmp(value.c_str(),token.c_str(),token.size())==0)
                value=variable.second+value.substr(token.size());
        }
        if(value.find(L'%')!=std::wstring::npos) {
            bool machine=false;
            for(const auto* token:{L"%SystemRoot%",L"%windir%"}) {
                const auto length=wcslen(token);
                if(_wcsnicmp(value.c_str(),token,length)==0) {
                    wchar_t windows[32768]{};const DWORD n=GetWindowsDirectoryW(windows,32768);
                    if(!n||n>=32768)return Fail(detail,L"Windows directory unavailable");
                    value=std::wstring(windows)+value.substr(length);machine=true;break;
                }
            }
            if(!machine || value.find(L'%')!=std::wstring::npos)return Fail(detail,L"CurrentTheme uses unsupported per-user environment variables");
        }
        wchar_t expanded[32768]{};
        const DWORD count=ExpandEnvironmentStringsW(value.c_str(),expanded,32768);
        if(!count || count>32768)return Fail(detail,L"CurrentTheme expansion failed");
        HANDLE active=CreateFileW(expanded,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
        if(active==INVALID_HANDLE_VALUE) {
            const auto error=GetLastError();if(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND)return true;
            return Fail(detail,L"CurrentTheme identity unavailable");
        }
        for(const auto& file:files_) {
            bool same=false;
            if(!SameFile(active,file.handle,same)){CloseHandle(active);return Fail(detail,L"CurrentTheme file identity check failed");}
            if(same){owned=true;break;}
        }
        CloseHandle(active);return true;
    }
    static bool WriteStockLink(HKEY hive,const std::wstring& theme,const std::wstring& style,std::wstring& detail) {
        // User explicitly approved this fallback for every local profile. It is
        // configuration for next activation, NOT proof of a live session reset.
        const auto write=[&](const wchar_t* sub,const wchar_t* name,const std::wstring& value) {
            HKEY key=nullptr;
            LONG rc=RegCreateKeyExW(hive,sub,0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr);
            if(rc!=ERROR_SUCCESS)return false;
            rc=RegSetValueExW(key,name,0,REG_SZ,reinterpret_cast<const BYTE*>(value.c_str()),static_cast<DWORD>((value.size()+1)*sizeof(wchar_t)));
            RegCloseKey(key);return rc==ERROR_SUCCESS;
        };
        if(!write(L"Software\\Microsoft\\Windows\\CurrentVersion\\ThemeManager",L"DllName",style) ||
           !write(L"Software\\Microsoft\\Windows\\CurrentVersion\\ThemeManager",L"ColorName",L"NormalColor") ||
           !write(L"Software\\Microsoft\\Windows\\CurrentVersion\\ThemeManager",L"SizeName",L"NormalSize") ||
           !write(L"Software\\Microsoft\\Windows\\CurrentVersion\\ThemeManager",L"ThemeActive",L"1") ||
           !write(L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes",L"CurrentTheme",theme))return Fail(detail,L"cannot write stock theme link");
        return true;
    }
public:
    bool Prepare(const std::wstring& profile,HKEY hive,bool (*safe)(const std::wstring&),std::wstring& detail) {
        prepared_=false;files_.clear();profile_=profile;safe_=safe;
        localAppData_=profile+L"\\AppData\\Local";
        wchar_t local[32768]{};DWORD localSize=sizeof(local);
        const LONG localStatus=RegGetValueW(hive,L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders",L"Local AppData",
            RRF_RT_REG_SZ|RRF_RT_REG_EXPAND_SZ|RRF_NOEXPAND,nullptr,local,&localSize);
        if(localStatus==ERROR_SUCCESS) {
            if(localSize<sizeof(wchar_t)||localSize>sizeof(local)||localSize%sizeof(wchar_t)||local[localSize/sizeof(wchar_t)-1])return Fail(detail,L"Local AppData value invalid");
            for(DWORD i=0;i+1<localSize/sizeof(wchar_t);++i)if(!local[i])return Fail(detail,L"Local AppData contains embedded NUL");
            std::wstring value=local;
            if(_wcsnicmp(value.c_str(),L"%USERPROFILE%",13)==0)value=profile+value.substr(13);
            if(value.find(L'%')!=std::wstring::npos)return Fail(detail,L"Local AppData uses unsupported environment variables");
            wchar_t expanded[32768]{},full[32768]{};
            const DWORD n=ExpandEnvironmentStringsW(value.c_str(),expanded,32768);
            const DWORD m=n&&n<=32768?GetFullPathNameW(expanded,32768,full,nullptr):0;
            if(!m||m>=32768)return Fail(detail,L"Local AppData path invalid");
            const std::wstring prefix=profile+L"\\";
            if(_wcsnicmp(full,prefix.c_str(),prefix.size())!=0)return Fail(detail,L"redirected Local AppData outside profile requires manual review");
            localAppData_=full;
        } else if(localStatus!=ERROR_FILE_NOT_FOUND)return Fail(detail,L"Local AppData unavailable");
        const auto directory=localAppData_+L"\\Microsoft\\Windows\\Themes";
        const auto attr=GetFileAttributesW(directory.c_str());
        if(attr==INVALID_FILE_ATTRIBUTES) {
            const DWORD error=GetLastError();
            if(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND)return true;
            return Fail(detail,directory);
        }
        if(!(attr&FILE_ATTRIBUTE_DIRECTORY)||!safe(directory))return Fail(detail,directory);
        WIN32_FIND_DATAW found{};HANDLE search=FindFirstFileW((directory+L"\\*.theme").c_str(),&found);
        if(search==INVALID_HANDLE_VALUE) {if(GetLastError()==ERROR_FILE_NOT_FOUND)return true;return Fail(detail,directory);}
        struct CloseSearch {HANDLE h;~CloseSearch(){FindClose(h);}} close{search};
        unsigned count=0;
        do {
            if(++count>4096)return Fail(detail,L"too many saved themes");
            const std::wstring name=found.cFileName;
            if(name==L"."||name==L".."||name.find_first_of(L"\\/:")!=std::wstring::npos)return Fail(detail,L"invalid saved theme filename");
            File file;file.path=directory+L"\\"+name;
            if(found.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))return Fail(detail,file.path);
            file.handle=CreateFileW(file.path.c_str(),GENERIC_READ|DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
            if(file.handle==INVALID_HANDLE_VALUE)return Fail(detail,file.path);
            BY_HANDLE_FILE_INFORMATION info{};
            wchar_t finalPath[32768]{};
            const DWORD chars=GetFinalPathNameByHandleW(file.handle,finalPath,32768,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
            if(!GetFileInformationByHandle(file.handle,&info) || info.nNumberOfLinks!=1 || info.nFileSizeHigh || info.nFileSizeLow>4*1024*1024 ||
               (info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) || !chars || chars>=32768 ||
               _wcsicmp(finalPath,(L"\\\\?\\"+file.path).c_str())!=0)return Fail(detail,file.path);
            std::vector<unsigned char> bytes(info.nFileSizeLow);DWORD read=0;
            if(!ReadFile(file.handle,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr)||read!=bytes.size())return Fail(detail,file.path);
            bool valid=false;const bool glass=HasGlassSection(bytes,valid);
            if(!valid)return Fail(detail,L"unreadable theme encoding: "+file.path);
            if(glass && (info.dwFileAttributes&FILE_ATTRIBUTE_READONLY))return Fail(detail,L"read-only saved Glass theme: "+file.path);
            if(glass)files_.push_back(std::move(file));
        } while(FindNextFileW(search,&found));
        if(GetLastError()!=ERROR_NO_MORE_FILES)return Fail(detail,directory);
        bool active=false;if(!ActiveOwned(hive,active,detail))return false;
        if(active) {
            // Test existing Windows keys for write/create rights without writing
            // or creating a key during preflight.
            HKEY parent=nullptr;
            const LONG rc=RegOpenKeyExW(hive,L"Software\\Microsoft\\Windows\\CurrentVersion",0,KEY_CREATE_SUB_KEY,&parent);
            if(rc!=ERROR_SUCCESS)return Fail(detail,L"stock link access unavailable");
            RegCloseKey(parent);
            for(const auto* sub:{L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes",L"Software\\Microsoft\\Windows\\CurrentVersion\\ThemeManager"}) {
                HKEY key=nullptr;const LONG st=RegOpenKeyExW(hive,sub,0,KEY_SET_VALUE,&key);
                if(st!=ERROR_SUCCESS && st!=ERROR_FILE_NOT_FOUND)return Fail(detail,L"stock link value access unavailable");
                if(key)RegCloseKey(key);
            }
        }
        prepared_=true;
        return true;
    }
    bool Cleanup(HKEY hive,bool liveUser,bool& registryFallback,std::wstring& detail) {
        if(files_.empty())return true;
        if(!prepared_)return Fail(detail,L"successful saved-theme preflight required");
        prepared_=false;
        // Windows may legitimately rewrite Custom.theme during earlier style
        // preparation. Pin identity at preflight, but seal writers only at
        // commit, then re-read the ownership section before any reset/delete.
        std::vector<File> sealed;
        for(auto& file:files_) {
            file.seal=CreateFileW(file.path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
            if(file.seal==INVALID_HANDLE_VALUE)return Fail(detail,L"cannot seal saved theme: "+file.path);
            bool same=false;
            if(!SameFile(file.handle,file.seal,same)||!same)return Fail(detail,L"saved theme identity changed: "+file.path);
            wchar_t finalPath[32768]{};
            const DWORD finalSize=GetFinalPathNameByHandleW(file.seal,finalPath,32768,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
            if(!finalSize||finalSize>=32768||_wcsicmp(finalPath,(L"\\\\?\\"+file.path).c_str())!=0)return Fail(detail,L"saved theme moved outside expected path");
            BY_HANDLE_FILE_INFORMATION info{};
            if(!GetFileInformationByHandle(file.seal,&info)||info.nNumberOfLinks!=1||info.nFileSizeHigh||info.nFileSizeLow>4*1024*1024||
               (info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))return Fail(detail,file.path);
            std::vector<unsigned char> bytes(info.nFileSizeLow);DWORD read=0;
            if(!ReadFile(file.seal,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr)||read!=bytes.size())return Fail(detail,file.path);
            bool valid=false;const bool glass=HasGlassSection(bytes,valid);
            if(!valid)return Fail(detail,L"saved theme encoding changed: "+file.path);
            if(glass && (info.dwFileAttributes&FILE_ATTRIBUTE_READONLY))return Fail(detail,L"saved Glass theme became read-only: "+file.path);
            if(glass)sealed.push_back(std::move(file));
        }
        files_=std::move(sealed);
        if(files_.empty())return true;
        bool active=false;if(!ActiveOwned(hive,active,detail))return false;
        if(active) {
            wchar_t windows[32768]{};
            const DWORD count=GetWindowsDirectoryW(windows,32768);
            if(!count || count>=32768)return Fail(detail,L"Windows directory unavailable");
            const std::wstring theme=std::wstring(windows)+L"\\Resources\\Themes\\aero.theme";
            const std::wstring style=std::wstring(windows)+L"\\Resources\\Themes\\Aero\\aero.msstyles";
            const DWORD themeAttr=GetFileAttributesW(theme.c_str()),styleAttr=GetFileAttributesW(style.c_str());
            if(!safe_ || !safe_(theme) || !safe_(style) || themeAttr==INVALID_FILE_ATTRIBUTES || styleAttr==INVALID_FILE_ATTRIBUTES ||
               ((themeAttr|styleAttr)&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))return Fail(detail,L"stock Windows resources unavailable or unsafe");
            bool live=false;
            if(liveUser && SUCCEEDED(Shared::ThemeHelper::SetSystemVisualStyle(style.c_str(),L"NormalColor",L"NormalSize",0))) {
                wchar_t actual[32768]{},color[256]{},size[256]{};
                live=SUCCEEDED(GetCurrentThemeName(actual,32768,color,256,size,256)) && _wcsicmp(actual,style.c_str())==0;
            }
            if(!WriteStockLink(hive,theme,style,detail))return false;
            if(!live)registryFallback=true;
            if(!ActiveOwned(hive,active,detail)||active)return Fail(detail,L"active saved theme link still present");
        }
        for(auto& file:files_) {
            FILE_DISPOSITION_INFO disposition{};disposition.DeleteFile=TRUE;
            if(!SetFileInformationByHandle(file.handle,FileDispositionInfo,&disposition,sizeof(disposition)))return Fail(detail,file.path);
            CloseHandle(file.handle);file.handle=INVALID_HANDLE_VALUE;
            CloseHandle(file.seal);file.seal=INVALID_HANDLE_VALUE;
        }
        files_.clear();return true;
    }
};
}
