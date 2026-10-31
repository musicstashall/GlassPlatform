#!/usr/bin/env python3
"""Test the actual registry/default path resolver with simulated Windows APIs.

Exercises read precedence, architecture views, default paths, OpenGlass and
checked writes. These fakes do not replace an elevated Windows MSI/VM test.
"""
from pathlib import Path
import os
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[2]
util = (repo / "GlassSetup/src/InstallSupport/GlassInstallUtil.cpp").read_text(encoding="utf-8")
header = (repo / "GlassSetup/src/InstallSupport/GlassInstallUtil.h").read_text(encoding="utf-8")
engine = (repo / "GlassSetup/src/InstallEngine.cpp").read_text(encoding="utf-8")
rollback = (repo / "GlassSetup/src/InstallSupport/GlassInstallRollback.cpp").read_text(encoding="utf-8")


def function(text, token):
    start = text.index(token)
    brace = text.index("{", start)
    depth = 0
    for index in range(brace, len(text)):
        if text[index] == "{":
            depth += 1
        if text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[start:index + 1]
    raise ValueError(token)


def structure(text, token):
    start = text.index(token)
    return text[start:text.index("\n};", start) + 3]


names = ["RegistryPathRead ReadInstallPathValue(", "bool WriteRegSzIfMissing(",
         "LSTATUS ReadNativeProgramFilesValue(", "DWORD ValidateSystemProgramFiles(",
         "bool QueryNativeOs64Bit(", "DWORD ResolveDefaultPlatformRoot(",
         "RegistryPathRead ReadPlatformInstallPath(", "RegistryPathRead ReadOpenGlassInstallPath(",
         "DWORD ValidateRegisteredInstallRoot(", "bool ResolveInstallRoots(",
         "bool WritePlatformInstallPath(", "bool WriteOpenGlassInstallPath("]
pieces = [function(util, token) for token in names]
assert "enum class RegistryPathRead { Missing, Found, Error };" in util
resolved = function(util, "bool ResolveInstallRoots(")
assert "ReadPlatformInstallPath(" in resolved and "ResolveDefaultPlatformRoot(" in resolved
assert "TryGetAutorunGlassCompatInstallRoot(" not in resolved
sequence = function(engine, "bool InstallEngine::RunMsiSequence(")
assert "GlassInstallUtil::WritePlatformInstallPath(" in sequence
assert "PreservePlatformUserPath(rollbackSession_,true)" in sequence
assert "PreservePlatformUserPath(rollbackSession_,false)" in sequence
assert "pathWritten=GlassInstallUtil::WriteOpenGlassInstallPath(" in sequence
assert "if (!pathWritten)" in sequence and "SetFailure(" in sequence.split("if (!pathWritten)", 1)[1]

prefix = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
using DWORD=uint32_t; using LSTATUS=int32_t; using HRESULT=int32_t; using BOOL=int;
using HKEY=void*; using REGSAM=DWORD; using BYTE=unsigned char; using LPBYTE=unsigned char*; using PWSTR=wchar_t*;
constexpr BOOL FALSE=0;
constexpr DWORD ERROR_SUCCESS=0,ERROR_FILE_NOT_FOUND=2,ERROR_PATH_NOT_FOUND=3,ERROR_ACCESS_DENIED=5,
 ERROR_NOT_ENOUGH_MEMORY=8,ERROR_INVALID_DATA=13,ERROR_INVALID_PARAMETER=87,ERROR_INVALID_NAME=123,
 ERROR_MORE_DATA=234,FILE_ATTRIBUTE_DIRECTORY=16,FILE_ATTRIBUTE_REPARSE_POINT=0x400,
 INVALID_FILE_ATTRIBUTES=0xffffffffu,REG_SZ=1,REG_EXPAND_SZ=2,KEY_QUERY_VALUE=1,KEY_SET_VALUE=2,
 KEY_WOW64_64KEY=0x100,KEY_WOW64_32KEY=0x200,KF_FLAG_DEFAULT=0;
constexpr int FOLDERID_ProgramFilesX86=1,FOLDERID_ProgramFiles=2;
HKEY const HKEY_LOCAL_MACHINE=reinterpret_cast<HKEY>(1);
HKEY const HKEY_CURRENT_USER=reinterpret_cast<HKEY>(2);
#define FAILED(hr) ((hr)<0)
#define HRESULT_CODE(hr) (static_cast<DWORD>(hr)&0xffff)
using Key=std::tuple<int,std::wstring,DWORD,std::wstring>;
using OpenKey=std::tuple<int,std::wstring,DWORD>;
struct Value {std::wstring data;DWORD type=REG_SZ;LSTATUS status=0;bool terminated=true;};
struct Handle {int hive;std::wstring sub;DWORD view;};
struct Fake {
 bool wow=true,wowCall=true,folder86=true,folderDefault=true;
 DWORD error=0;int known86Calls=0,knownDefaultCalls=0,systemOpens=0,writes=0;
 std::map<Key,Value> values;std::set<OpenKey> denied;std::set<Key> failSet;
 std::set<std::wstring> directories,files,unsafe;
 std::wstring pf86=L"C:\\Program Files (x86)",pf=L"C:\\Program Files";
} fake;
int hiveId(HKEY h){assert(h==HKEY_LOCAL_MACHINE||h==HKEY_CURRENT_USER);return h==HKEY_LOCAL_MACHINE?1:2;}
void put(int hive,const wchar_t* sub,DWORD view,const wchar_t* name,const wchar_t* data){fake.values[{hive,sub,view,name}]={data};}
void reset(){fake={};
 put(1,L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion",KEY_WOW64_64KEY,L"ProgramFilesDir",L"C:\\Program Files");
 put(1,L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion",KEY_WOW64_64KEY,L"ProgramW6432Dir",L"C:\\Program Files");
 fake.directories={L"C:\\Program Files",L"C:\\Program Files (x86)"};
}
DWORD GetLastError(){return fake.error;} void SetLastError(DWORD e){fake.error=e;}
void* GetCurrentProcess(){return nullptr;}
BOOL IsWow64Process(void*,BOOL* wow){if(!fake.wowCall){fake.error=ERROR_ACCESS_DENIED;return FALSE;}*wow=fake.wow;return 1;}
HRESULT SHGetKnownFolderPath(int id,DWORD,void*,PWSTR* p){
 bool available=id==FOLDERID_ProgramFilesX86?fake.folder86:fake.folderDefault;
 if(id==FOLDERID_ProgramFilesX86)++fake.known86Calls;else {assert(id==FOLDERID_ProgramFiles);++fake.knownDefaultCalls;}
 if(!available){*p=nullptr;return static_cast<HRESULT>(0x80070002u);}
 const auto& v=id==FOLDERID_ProgramFilesX86?fake.pf86:fake.pf;
 *p=new wchar_t[v.size()+1];std::wmemcpy(*p,v.c_str(),v.size()+1);return 0;
}
void CoTaskMemFree(PWSTR p){delete[] p;}
LSTATUS RegOpenKeyExW(HKEY root,const wchar_t* sub,DWORD,DWORD access,HKEY* result){
 const int hive=hiveId(root);const DWORD view=access&(KEY_WOW64_64KEY|KEY_WOW64_32KEY);
 if(fake.denied.count({hive,sub,view}))return ERROR_ACCESS_DENIED;
 bool present=false;for(const auto& item:fake.values)if(std::get<0>(item.first)==hive &&
  std::get<1>(item.first)==sub && std::get<2>(item.first)==view){present=true;break;}
 if(!present)return ERROR_FILE_NOT_FOUND;
 if(std::wstring(sub)==L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion")++fake.systemOpens;
 *result=new Handle{hive,sub,view};return ERROR_SUCCESS;
}
LSTATUS RegCreateKeyExW(HKEY root,const wchar_t* sub,DWORD,void*,DWORD,DWORD access,void*,HKEY* result,void*){
 const int hive=hiveId(root);const DWORD view=access&(KEY_WOW64_64KEY|KEY_WOW64_32KEY);
 if(fake.denied.count({hive,sub,view}))return ERROR_ACCESS_DENIED;
 *result=new Handle{hive,sub,view};return ERROR_SUCCESS;
}
LSTATUS RegQueryValueExW(HKEY raw,const wchar_t* name,void*,DWORD* type,LPBYTE data,DWORD* bytes){
 auto* h=static_cast<Handle*>(raw);auto it=fake.values.find({h->hive,h->sub,h->view,name});
 if(it==fake.values.end())return ERROR_FILE_NOT_FOUND;
 const auto& v=it->second;if(v.status)return v.status;if(type)*type=v.type;
 const size_t chars=v.data.size()+(v.terminated?1:0);const DWORD size=static_cast<DWORD>(chars*sizeof(wchar_t));
 if(!data){*bytes=size;return 0;}
 if(size>*bytes){*bytes=size;return ERROR_MORE_DATA;}if(size)std::memcpy(data,v.data.c_str(),size);*bytes=size;return 0;
}
LSTATUS RegSetValueExW(HKEY raw,const wchar_t* name,DWORD,DWORD type,const BYTE* data,DWORD bytes){
 auto* h=static_cast<Handle*>(raw);Key k={h->hive,h->sub,h->view,name};
 if(fake.failSet.count(k))return ERROR_ACCESS_DENIED;
 assert(data && bytes>=sizeof(wchar_t) && bytes%sizeof(wchar_t)==0);
 const size_t count=bytes/sizeof(wchar_t);
 std::wstring value(reinterpret_cast<const wchar_t*>(data),count);if(!value.empty()&&value.back()==0)value.pop_back();
 fake.values[k]={value,type};++fake.writes;return 0;
}
LSTATUS RegCloseKey(HKEY raw){delete static_cast<Handle*>(raw);return 0;}
DWORD GetFileAttributesW(const wchar_t* p){if(fake.directories.count(p))return FILE_ATTRIBUTE_DIRECTORY;
 if(fake.files.count(p))return 0;fake.error=ERROR_PATH_NOT_FOUND;return INVALID_FILE_ATTRIBUTES;}
std::wstring TrimTrailingSlashes(std::wstring path){while(!path.empty()&&(path.back()==L'\\'||path.back()==L'/'))path.pop_back();return path;}
bool NormalProductDirectory(const std::wstring& raw,std::wstring& normalized){
 if(raw.size()<3||raw[1]!=L':'||(raw[2]!=L'\\'&&raw[2]!=L'/'))return false;
 std::vector<std::wstring> parts;size_t pos=3;
 while(pos<raw.size()){size_t end=raw.find_first_of(L"\\/",pos);auto s=raw.substr(pos,end==std::wstring::npos?end:end-pos);
  if(s==L"..") {if(!parts.empty())parts.pop_back();}else if(!s.empty()&&s!=L".")parts.push_back(s);
  if(end==std::wstring::npos)break;pos=end+1;}
 if(parts.empty())return false;normalized=raw.substr(0,2);for(const auto& s:parts)normalized+=L"\\"+s;return true;
}
int _wcsicmp(const wchar_t* a,const wchar_t* b){while(*a&&*b){auto x=std::towlower(*a++),y=std::towlower(*b++);if(x!=y)return x<y?-1:1;}return *a?1:*b?-1:0;}
bool SameInstallPath(const std::wstring& a,const std::wstring& b){std::wstring x,y;return NormalProductDirectory(a,x)&&NormalProductDirectory(b,y)&&_wcsicmp(x.c_str(),y.c_str())==0;}
bool IsSafeInstallPath(const std::wstring& path){return !fake.unsafe.count(path);}
'''

main = r'''
int main(){InstallRoots r;
 // No product paths: standard Windows locations, including x64 from a WOW64 process.
 reset();assert(ResolveInstallRoots(r));
 assert(r.x64==L"C:\\Program Files\\GlassPlatform" && r.x86==L"C:\\Program Files (x86)\\GlassPlatform");
 assert(r.openGlass==r.x64+L"\\OpenGlass" && fake.systemOpens==2 && fake.known86Calls==1);
 // Registered locations win, including non-default locations and a separate OpenGlass path.
 reset();put(1,L"Software\\GlassPlatform",KEY_WOW64_32KEY,L"InstallPathX86",L"D:\\Programs\\Glass32");
 put(1,L"Software\\GlassPlatform",KEY_WOW64_64KEY,L"InstallPathX64",L"E:\\Programs\\Glass64");
 put(1,L"Software\\StanislavKa\\GlassPlatform",KEY_WOW64_64KEY,L"OpenGlassInstallPath",L"F:\\Glass\\OpenGlass");
 assert(ResolveInstallRoots(r));assert(r.x86==L"D:\\Programs\\Glass32" && r.x64==L"E:\\Programs\\Glass64");
 assert(r.openGlass==L"F:\\Glass\\OpenGlass" && fake.known86Calls==0 && fake.systemOpens==0);
 assert(r.sourceX86==L"HKLM(32) InstallPathX86" && r.sourceX64==L"HKLM(64) InstallPathX64");
 // An absent architecture alone gets a default; do not query the other's default.
 reset();put(1,L"Software\\GlassPlatform",KEY_WOW64_32KEY,L"InstallPathX86",L"D:\\Programs\\Glass32");
 assert(ResolveInstallRoots(r));assert(r.x86==L"D:\\Programs\\Glass32" && r.x64==L"C:\\Program Files\\GlassPlatform");
 assert(fake.known86Calls==0 && fake.systemOpens==2);
 reset();put(1,L"Software\\GlassPlatform",KEY_WOW64_64KEY,L"InstallPathX64",L"E:\\Programs\\Glass64");
 assert(ResolveInstallRoots(r));assert(r.openGlass==L"E:\\Programs\\Glass64\\OpenGlass");
 assert(fake.known86Calls==1 && fake.systemOpens==0);
 // Established machine-before-user precedence: do not silently reconcile copies.
 reset();put(1,L"Software\\GlassPlatform",KEY_WOW64_32KEY,L"InstallPathX86",L"D:\\Machine\\Glass");
 put(2,L"Software\\GlassPlatform",0,L"InstallPathX86",L"E:\\User\\Glass");
 assert(ResolveInstallRoots(r) && r.x86==L"D:\\Machine\\Glass" && r.sourceX86==L"HKLM(32) InstallPathX86");
 reset();put(1,L"Software\\GlassPlatform",KEY_WOW64_32KEY,L"InstallPath",L"D:\\Legacy\\Glass");
 put(2,L"Software\\GlassPlatform",0,L"InstallPathX86",L"E:\\User\\Glass");
 assert(ResolveInstallRoots(r) && r.x86==L"D:\\Legacy\\Glass" && r.sourceX86==L"HKLM(32) InstallPath");
 reset();put(2,L"Software\\GlassPlatform",0,L"InstallPathX86",L"E:\\User\\Glass");
 assert(ResolveInstallRoots(r) && r.x86==L"E:\\User\\Glass" && r.sourceX86==L"HKCU InstallPathX86");
 reset();put(2,L"Software\\Wow6432Node\\GlassPlatform",0,L"InstallPath",L"D:\\LegacyUser\\Glass");
 assert(ResolveInstallRoots(r) && r.x86==L"D:\\LegacyUser\\Glass");
 // Native 32-bit Windows does not resolve x64/OpenGlass.
 reset();fake.wow=false;fake.pf86=L"C:\\Program Files";assert(ResolveInstallRoots(r));
 assert(r.x64.empty() && r.openGlass.empty() && r.x86==L"C:\\Program Files\\GlassPlatform");
 // A present but unreadable/malformed value must not be mistaken for no value.
 reset();fake.denied.insert({1,L"Software\\GlassPlatform",KEY_WOW64_32KEY});
 assert(!ResolveInstallRoots(r) && GetLastError()==ERROR_ACCESS_DENIED && r.x86.empty());
 for(const auto* value:{L"relative",L"C:\\",L"D:\\Apps\\..\\Other",L"D:\\Apps\\bad\"name",L"D:\\Apps\\file:stream"}){
  reset();put(1,L"Software\\GlassPlatform",KEY_WOW64_32KEY,L"InstallPathX86",value);
  assert(!ResolveInstallRoots(r) && r.x86.empty());
 }
 reset();put(1,L"Software\\GlassPlatform",KEY_WOW64_32KEY,L"InstallPathX86",L"D:\\Apps\\Glass");
 fake.values[{1,L"Software\\GlassPlatform",KEY_WOW64_32KEY,L"InstallPathX86"}].type=REG_EXPAND_SZ;
 assert(!ResolveInstallRoots(r) && GetLastError()==ERROR_INVALID_DATA);
 reset();put(1,L"Software\\GlassPlatform",KEY_WOW64_32KEY,L"InstallPathX86",L"D:\\Apps\\Glass");
 fake.values[{1,L"Software\\GlassPlatform",KEY_WOW64_32KEY,L"InstallPathX86"}].terminated=false;
 assert(!ResolveInstallRoots(r));
 reset();put(1,L"Software\\GlassPlatform",KEY_WOW64_32KEY,L"InstallPathX86",L"D:\\Apps\\Glass");
 fake.unsafe.insert(L"D:\\Apps\\Glass");assert(!ResolveInstallRoots(r));
 reset();put(1,L"Software\\GlassPlatform",KEY_WOW64_32KEY,L"InstallPathX86",L"D:\\Apps\\Glass");
 fake.files.insert(L"D:\\Apps\\Glass");assert(!ResolveInstallRoots(r));
 // Writing is checked: failure is propagated for the engine's rollback path.
 reset();assert(WritePlatformInstallPath(false,L"D:\\Programs\\Glass32"));
 assert(fake.values.at({1,L"Software\\GlassPlatform",KEY_WOW64_32KEY,L"InstallPathX86"}).data==L"D:\\Programs\\Glass32");
 assert(ResolveInstallRoots(r) && r.x86==L"D:\\Programs\\Glass32");
 reset();fake.failSet.insert({1,L"Software\\GlassPlatform",KEY_WOW64_32KEY,L"InstallPathX86"});
 assert(!WritePlatformInstallPath(false,L"D:\\Programs\\Glass32") && GetLastError()==ERROR_ACCESS_DENIED);
 // Existing conflicting copies are not rewritten or reconciled.
 reset();put(1,L"Software\\GlassPlatform",KEY_WOW64_32KEY,L"InstallPathX86",L"D:\\Machine\\Glass");
 put(2,L"Software\\GlassPlatform",0,L"InstallPathX86",L"E:\\User\\Glass");
 put(1,L"Software\\GlassPlatform",KEY_WOW64_32KEY,L"InstallPath",L"F:\\Legacy\\Glass");
 assert(WritePlatformInstallPath(false,L"D:\\Machine\\Glass"));
 assert(fake.values.at({2,L"Software\\GlassPlatform",0,L"InstallPathX86"}).data==L"E:\\User\\Glass");
 assert(fake.values.at({1,L"Software\\GlassPlatform",KEY_WOW64_32KEY,L"InstallPath"}).data==L"F:\\Legacy\\Glass");
 assert(!WritePlatformInstallPath(false,L"G:\\Changed\\Glass") && GetLastError()==ERROR_INVALID_DATA);
 reset();assert(WriteOpenGlassInstallPath(L"E:\\Glass64",L"F:\\OpenGlass"));
 assert(fake.values.at({1,L"Software\\StanislavKa\\GlassPlatform",KEY_WOW64_64KEY,L"OpenGlassInstallPath"}).data==L"F:\\OpenGlass");
 reset();fake.failSet.insert({1,L"Software\\StanislavKa\\GlassPlatform",KEY_WOW64_64KEY,L"OpenGlassInstallPath"});
 assert(!WriteOpenGlassInstallPath(L"E:\\Glass64",L"F:\\OpenGlass") && GetLastError()==ERROR_ACCESS_DENIED);
 std::cout<<"PASS: registered/default roots, registry precedence, x86/x64, OpenGlass and checked writes\n";
}
'''

with tempfile.TemporaryDirectory(prefix="glass-registered-paths-") as directory:
    cpp = Path(directory) / "test.cpp"
    exe = Path(directory) / "test"
    source = (prefix + "\nenum class RegistryPathRead { Missing, Found, Error };\n" +
              structure(header, "struct InstallRoots") + "\n" + "\n".join(pieces) + "\n" + main)
    cpp.write_text(source, encoding="utf-8")
    subprocess.run([os.environ.get("CXX", "g++"), "-std=c++17", "-g", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", "-no-pie", str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
