#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <utility>
#include "GlassInstallUtil.h"
#include "GlassPackageRules.h"

// Read-only inventory, not ownership or execution authorization. Callers must
// separately verify protected paths and publisher before running vendor code.
// Exact official Inno identity; never enumerate/adopt another vendor's key.
namespace GlassOpenGlassInstall {
inline constexpr auto Key=L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{D3D1BC7D-5E24-4B33-9383-7934271A3B05}_is1";
struct Record {bool installed=false;std::wstring directory,version,uninstaller;};
inline bool Query(Record& record,std::wstring& error) noexcept {
    record={};error.clear();HKEY key=nullptr;
    try {
        const auto opened=RegOpenKeyExW(HKEY_LOCAL_MACHINE,Key,0,KEY_QUERY_VALUE|KEY_WOW64_64KEY,&key);
        if(opened==ERROR_FILE_NOT_FOUND||opened==ERROR_PATH_NOT_FOUND)return true;
        if(opened!=ERROR_SUCCESS){error=L"Cannot query official OpenGlass registration ("+std::to_wstring(opened)+L")";return false;}
        const auto read=[&](const wchar_t* name,std::wstring& value){
            DWORD type=0,size=0;
            if(RegQueryValueExW(key,name,nullptr,&type,nullptr,&size)!=ERROR_SUCCESS||type!=REG_SZ||size<sizeof(wchar_t)||size>65536||size%sizeof(wchar_t))return false;
            std::vector<wchar_t> text(size/sizeof(wchar_t));
            if(RegQueryValueExW(key,name,nullptr,&type,reinterpret_cast<BYTE*>(text.data()),&size)!=ERROR_SUCCESS||type!=REG_SZ||size!=text.size()*sizeof(wchar_t)||text.back()!=L'\0')return false;
            value.assign(text.data(),text.size()-1);return value.find(L'\0')==std::wstring::npos;
        };
        Record found;std::wstring command;
        bool valid=read(L"InstallLocation",found.directory)&&read(L"DisplayVersion",found.version)&&read(L"UninstallString",command);
        RegCloseKey(key);key=nullptr;
        found.directory=GlassInstallUtil::TrimTrailingSlashes(found.directory);
        found.uninstaller=found.directory+L"\\unins000.exe";
        GlassPackageRules::Version version;
        valid=valid&&GlassInstallUtil::IsSafeInstallPath(found.directory)&&found.directory.size()>3&&
            found.directory[1]==L':'&&found.directory[2]==L'\\'&&
            found.directory.find_first_of(L"\"\r\n")==std::wstring::npos&&
            (command==L"\""+found.uninstaller+L"\""||command==found.uninstaller)&&
            GlassPackageRules::ParseVersion(std::string(found.version.begin(),found.version.end()),version);
        if(!valid){error=L"Invalid official OpenGlass directory, version or uninstaller registration";return false;}
        found.installed=true;record=std::move(found);return true;
    }catch(...){if(key)RegCloseKey(key);error=L"Official OpenGlass inventory failed";return false;}
}
inline bool HasProduct() noexcept {Record record;std::wstring error;return !Query(record,error)||record.installed;}
inline bool Same(const Record& a,const Record& b) noexcept {
    return a.installed==b.installed&&(!a.installed||
        (GlassInstallUtil::SameInstallPath(a.directory,b.directory)&&a.version==b.version&&a.uninstaller==b.uninstaller));
}
}
