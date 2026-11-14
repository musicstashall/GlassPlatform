#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <utility>

// Elevated deletion is confined to profile\AppData\Local\GlassPlatform.
// Pin every ancestor against rename, reject links, delete by verified handle.
namespace GlassUserDataCleanup {
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    Handle()=default;Handle(const Handle&)=delete;
    Handle(Handle&& other) noexcept:value(other.value){other.value=INVALID_HANDLE_VALUE;}
    ~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
};
inline bool Fail(std::wstring& detail,const std::wstring& path) {
    detail=L"User data cleanup refused/incomplete: "+path+L" ("+std::to_wstring(GetLastError())+L")";return false;
}
inline bool Tree(const std::wstring& path,bool erase,unsigned depth,unsigned& count,std::wstring& detail) {
    if(depth>64||++count>100000||path.size()>32000)return Fail(detail,path);
    Handle node;node.value=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES|DELETE,
        FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(node.value==INVALID_HANDLE_VALUE)return Fail(detail,path);
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(node.value,&info)||
       (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)||
       (!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)&&info.nNumberOfLinks!=1))return Fail(detail,path);
    if(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) {
        WIN32_FIND_DATAW data{};HANDLE find=FindFirstFileW((path+L"\\*").c_str(),&data);
        if(find==INVALID_HANDLE_VALUE) {
            if(GetLastError()!=ERROR_FILE_NOT_FOUND)return Fail(detail,path);
        }else {
            bool ok=true;
            do {
                const std::wstring name=data.cFileName;
                if(name==L"."||name==L"..")continue;
                if(name.find_first_of(L"\\/:")!=std::wstring::npos||!Tree(path+L"\\"+name,erase,depth+1,count,detail)){ok=false;break;}
            }while(FindNextFileW(find,&data));
            const DWORD error=GetLastError();FindClose(find);
            if(!ok)return false;
            if(error!=ERROR_NO_MORE_FILES){SetLastError(error);return Fail(detail,path);}
        }
    }
    if(erase) {
        FILE_DISPOSITION_INFO disposition{};disposition.DeleteFile=TRUE;
        if(!SetFileInformationByHandle(node.value,FileDispositionInfo,&disposition,sizeof(disposition)))return Fail(detail,path);
    }
    return true;
}
inline bool Run(const std::wstring& profile,bool erase,std::wstring& detail) {
    if(profile.size()<3||profile[1]!=L':'||profile[2]!=L'\\'||profile.find(L"..")!=std::wstring::npos)
        return Fail(detail,profile);
    const auto path=profile+L"\\AppData\\Local\\GlassPlatform";
    std::vector<Handle> parents;
    // Hold root and each component through commit; no path can be swapped for
    // a junction between validation, enumeration and handle disposition.
    for(std::size_t end=3;end<path.size();) {
        Handle parent;parent.value=CreateFileW(path.substr(0,end).c_str(),FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(parent.value==INVALID_HANDLE_VALUE) {
            const auto error=GetLastError();
            if(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND)return true;
            return Fail(detail,path.substr(0,end));
        }
        BY_HANDLE_FILE_INFORMATION info{};
        if(!GetFileInformationByHandle(parent.value,&info)||!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||
           (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))return Fail(detail,path.substr(0,end));
        parents.push_back(std::move(parent));
        const auto next=path.find(L'\\',end==3?3:end+1);
        if(next==std::wstring::npos)break;
        end=next;
    }
    const auto attr=GetFileAttributesW(path.c_str());
    if(attr==INVALID_FILE_ATTRIBUTES) {
        const auto error=GetLastError();if(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND)return true;
        return Fail(detail,path);
    }
    if(!(attr&FILE_ATTRIBUTE_DIRECTORY)||(attr&FILE_ATTRIBUTE_REPARSE_POINT))return Fail(detail,path);
    unsigned count=0;return Tree(path,erase,0,count,detail);
}
}
