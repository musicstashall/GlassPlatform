#!/usr/bin/env python3
"""Exercise the real fixed-root resolver with fake Win32/registry APIs.

Simulates the 32-bit installer on x64 and x86 Windows. It does not replace
running the installer or Windows APIs on a Windows VM.
"""
from pathlib import Path
import os
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[2]
util = (repo / "GlassSetup/src/InstallSupport/GlassInstallUtil.cpp").read_text(encoding="utf-8")
setup = (repo / "GlassSetup/src/main.cpp").read_text(encoding="utf-8")
unins = (repo / "GlassSetup/GlassUnins/main.cpp").read_text(encoding="utf-8")


def function(text, token):
    start = text.index(token)
    brace = text.index("{", start)
    depth = 0
    for pos in range(brace, len(text)):
        if text[pos] == "{":
            depth += 1
        if text[pos] == "}":
            depth -= 1
            if depth == 0:
                return text[start:pos + 1]
    raise ValueError(token)


read_value = function(util, "LSTATUS ReadNativeProgramFilesValue(")
validate = function(util, "DWORD ValidateSystemProgramFiles(")
architecture = function(util, "bool QueryNativeOs64Bit(")
default_root = function(util, "DWORD ResolveDefaultPlatformRoot(")
resolve = function(util, "bool ResolveFixedPlatformRoots(")
assert 'KEY_QUERY_VALUE|KEY_WOW64_64KEY' in read_value
assert 'HKEY_LOCAL_MACHINE' in read_value and 'ProgramFilesDir' in default_root and 'ProgramW6432Dir' in default_root
assert 'GetEnvironmentVariable' not in read_value + default_root and 'ExpandEnvironmentStrings' not in read_value + default_root
assert 'FOLDERID_ProgramFilesX64' not in default_root
for token in ('bool InstalledGlassRoots(', 'void StartupWorker(', 'void StartInstallation('):
    assert 'ResolveInstallRoots(' in function(setup, token), token
start_install = function(setup, 'void StartInstallation(')
assert 'preflightX64Root_' in start_install and 'preflightX86Root_' in start_install and 'preflightOpenGlassRoot_' in start_install
assert start_install.index('SameInstallPath(') < start_install.index('installEngine_.Start(')
for token in ('void ResolveRoots()', 'int RunOperation('):
    assert 'ResolveInstallRoots(' in function(unins, token), token
assert 'rootsKnown_' in function(unins, 'int RunOperation(')

prefix = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <cstring>
using DWORD=uint32_t; using LSTATUS=int32_t; using HRESULT=int32_t; using BOOL=int;
using HKEY=void*; using LPBYTE=unsigned char*; using PWSTR=wchar_t*;
constexpr BOOL FALSE=0; constexpr DWORD ERROR_SUCCESS=0,ERROR_FILE_NOT_FOUND=2,ERROR_PATH_NOT_FOUND=3,
 ERROR_ACCESS_DENIED=5,ERROR_INVALID_DATA=13,ERROR_NOT_ENOUGH_MEMORY=8,ERROR_INVALID_NAME=123,
 ERROR_MORE_DATA=234,ERROR_INVALID_PARAMETER=87,FILE_ATTRIBUTE_DIRECTORY=16,FILE_ATTRIBUTE_REPARSE_POINT=0x400,
 INVALID_FILE_ATTRIBUTES=0xffffffffu,REG_SZ=1,REG_EXPAND_SZ=2,KEY_QUERY_VALUE=1,KEY_WOW64_64KEY=0x100,
 KF_FLAG_DEFAULT=0; constexpr int FOLDERID_ProgramFilesX86=1,FOLDERID_ProgramFiles=2;
HKEY const HKEY_LOCAL_MACHINE=reinterpret_cast<HKEY>(1);
#define FAILED(hr) ((hr)<0)
#define HRESULT_CODE(hr) (static_cast<DWORD>(hr)&0xffff)
struct RegValue { std::wstring text; DWORD type=REG_SZ; LSTATUS status=0; bool terminate=true; };
struct Fake {
 bool wow=true,wowCall=true,known86=true,knownDefault=true; DWORD error=0;
 std::wstring folder86=L"C:\\Program Files (x86)",folderDefault=L"C:\\Program Files";
 std::map<std::wstring,RegValue> reg;
 std::set<std::wstring> existing,files,unsafe;
 int registryOpens=0,folder86Calls=0,defaultCalls=0;
} fake;
void reset() {
 fake={};fake.reg[L"ProgramFilesDir"]={L"C:\\Program Files"};
 fake.reg[L"ProgramW6432Dir"]={L"C:\\Program Files"};
 fake.existing={L"C:\\Program Files",L"C:\\Program Files (x86)"};
}
DWORD GetLastError(){return fake.error;}
void SetLastError(DWORD error){fake.error=error;}
void* GetCurrentProcess(){return nullptr;}
BOOL IsWow64Process(void*,BOOL* wow){if(!fake.wowCall){fake.error=ERROR_ACCESS_DENIED;return 0;}*wow=fake.wow;return 1;}
HRESULT SHGetKnownFolderPath(int id,DWORD,void*,PWSTR* path){
 bool ok=id==FOLDERID_ProgramFilesX86?fake.known86:fake.knownDefault;
 if(id==FOLDERID_ProgramFilesX86)++fake.folder86Calls;else {assert(id==FOLDERID_ProgramFiles);++fake.defaultCalls;}
 if(!ok){*path=nullptr;return static_cast<HRESULT>(0x80070002u);}
 const auto& source=id==FOLDERID_ProgramFilesX86?fake.folder86:fake.folderDefault;
 *path=new wchar_t[source.size()+1];std::wmemcpy(*path,source.c_str(),source.size()+1);return 0;
}
void CoTaskMemFree(PWSTR p){delete[] p;}
LSTATUS RegOpenKeyExW(HKEY hive,const wchar_t* sub,DWORD,DWORD access,HKEY* key){
 assert(hive==HKEY_LOCAL_MACHINE);assert(std::wstring(sub)==L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion");
 assert(access==(KEY_QUERY_VALUE|KEY_WOW64_64KEY));++fake.registryOpens;*key=reinterpret_cast<HKEY>(2);return ERROR_SUCCESS;
}
LSTATUS RegQueryValueExW(HKEY key,const wchar_t* name,void*,DWORD* type,LPBYTE data,DWORD* bytes){
 assert(key==reinterpret_cast<HKEY>(2));auto it=fake.reg.find(name);if(it==fake.reg.end())return ERROR_FILE_NOT_FOUND;
 const RegValue& value=it->second;if(value.status)return value.status;
 *type=value.type;size_t chars=value.text.size()+(value.terminate?1:0);DWORD size=static_cast<DWORD>(chars*sizeof(wchar_t));
 if(size>*bytes){*bytes=size;return ERROR_MORE_DATA;}if(chars)std::memcpy(data,value.text.c_str(),size);*bytes=size;return ERROR_SUCCESS;
}
LSTATUS RegCloseKey(HKEY key){assert(key==reinterpret_cast<HKEY>(2));return 0;}
DWORD GetFileAttributesW(const wchar_t* p){if(fake.existing.count(p))return FILE_ATTRIBUTE_DIRECTORY;
 if(fake.files.count(p))return 0;
 fake.error=ERROR_PATH_NOT_FOUND;return INVALID_FILE_ATTRIBUTES;}
std::wstring TrimTrailingSlashes(std::wstring path){while(!path.empty()&&(path.back()==L'\\'||path.back()==L'/'))path.pop_back();return path;}
bool NormalProductDirectory(const std::wstring& raw,std::wstring& normalized){
 if(raw.size()<3||raw[1]!=L':'||(raw[2]!=L'\\'&&raw[2]!=L'/'))return false;
 std::vector<std::wstring> parts;size_t pos=3;
 while(pos<raw.size()){size_t end=raw.find_first_of(L"\\/",pos);auto s=raw.substr(pos,end==std::wstring::npos?end:end-pos);
 if(s==L"..") {if(!parts.empty())parts.pop_back();}else if(!s.empty()&&s!=L".")parts.push_back(s);
 if(end==std::wstring::npos)break;pos=end+1;}
 if(parts.empty())return false;normalized=raw.substr(0,2);for(const auto& s:parts)normalized+=L"\\"+s;return true;
}
int _wcsicmp(const wchar_t* a,const wchar_t* b){while(*a&&*b){wchar_t x=std::towlower(*a++),y=std::towlower(*b++);if(x!=y)return x<y?-1:1;}return *a?1:*b?-1:0;}
bool SameInstallPath(const std::wstring& a,const std::wstring& b){std::wstring x,y;return NormalProductDirectory(a,x)&&NormalProductDirectory(b,y)&&_wcsicmp(x.c_str(),y.c_str())==0;}
bool IsWithinInstallRoot(const std::wstring& root,const std::wstring& path){std::wstring r,p;
 if(!NormalProductDirectory(root,r)||!NormalProductDirectory(path,p)||p.size()<r.size())return false;
 for(size_t i=0;i<r.size();++i)if(std::towlower(r[i])!=std::towlower(p[i]))return false;
 return p.size()==r.size()||p[r.size()]==L'\\';}
bool IsSafeInstallPath(const std::wstring& path){return !fake.unsafe.count(path);}
'''

main = r'''
int main(){std::wstring x64=L"old",x86=L"old";
 // A 32-bit installer running under WOW64 must use the 64-bit machine view,
 // not FOLDERID_ProgramFilesX64 or a caller-modified environment variable.
 reset();setenv("ProgramW6432","D:\\VS_PROJECT\\Build\\Debug",1);
 assert(ResolveFixedPlatformRoots(x64,x86));
 assert(x64==L"C:\\Program Files\\GlassPlatform" && x86==L"C:\\Program Files (x86)\\GlassPlatform");
 assert(fake.registryOpens==2 && fake.defaultCalls==0 && fake.folder86Calls==1);
 // Non-C system drives are supported.
 reset();fake.folder86=L"D:\\Apps32";fake.reg[L"ProgramFilesDir"].text=L"D:\\Apps64";
 fake.reg[L"ProgramW6432Dir"].text=L"D:\\Apps64";fake.existing={L"D:\\Apps32",L"D:\\Apps64"};
 assert(ResolveFixedPlatformRoots(x64,x86));assert(x64==L"D:\\Apps64\\GlassPlatform" && x86==L"D:\\Apps32\\GlassPlatform");
 // Native x86 Windows uses its own Program Files and never opens the 64-bit view.
 reset();fake.wow=false;fake.folder86=L"C:\\Program Files";assert(ResolveFixedPlatformRoots(x64,x86));
 assert(x64.empty() && x86==L"C:\\Program Files\\GlassPlatform" && fake.registryOpens==0);
 reset();fake.wow=false;fake.known86=false;assert(ResolveFixedPlatformRoots(x64,x86));assert(fake.defaultCalls==1);
 // On x64 a missing x86 known folder must not silently fall back to x64.
 reset();fake.known86=false;assert(!ResolveFixedPlatformRoots(x64,x86) && x64.empty() && x86.empty() && fake.defaultCalls==0);
 reset();fake.wowCall=false;assert(!ResolveFixedPlatformRoots(x64,x86) && GetLastError()==ERROR_ACCESS_DENIED);
 reset();fake.reg.erase(L"ProgramFilesDir");assert(!ResolveFixedPlatformRoots(x64,x86) && GetLastError()==ERROR_FILE_NOT_FOUND);
 reset();fake.reg.erase(L"ProgramW6432Dir");assert(!ResolveFixedPlatformRoots(x64,x86));
 reset();fake.reg[L"ProgramFilesDir"].type=REG_EXPAND_SZ;assert(!ResolveFixedPlatformRoots(x64,x86));
 reset();fake.reg[L"ProgramFilesDir"].terminate=false;assert(!ResolveFixedPlatformRoots(x64,x86));
 reset();fake.reg[L"ProgramW6432Dir"].text=L"D:\\Other";assert(!ResolveFixedPlatformRoots(x64,x86));
 reset();fake.reg[L"ProgramFilesDir"].text=fake.reg[L"ProgramW6432Dir"].text=L"C:\\Program Files (x86)";
 assert(!ResolveFixedPlatformRoots(x64,x86));
 reset();fake.folder86=L"C:\\Program Files\\GlassPlatform\\nested";fake.existing.insert(fake.folder86);
 assert(!ResolveFixedPlatformRoots(x64,x86));
 for(const std::wstring& bad:{L"relative",L"\\\\server\\share",L"C:\\Program Files\\..\\Other",L"C:\\foo%bar",L"C:\\"}){
   reset();fake.reg[L"ProgramFilesDir"].text=fake.reg[L"ProgramW6432Dir"].text=bad;
   assert(!ResolveFixedPlatformRoots(x64,x86) && x64.empty() && x86.empty());
 }
 reset();fake.unsafe.insert(L"C:\\Program Files\\GlassPlatform");assert(!ResolveFixedPlatformRoots(x64,x86));
 reset();fake.files.insert(L"C:\\Program Files\\GlassPlatform");assert(!ResolveFixedPlatformRoots(x64,x86));
 reset();fake.existing.erase(L"C:\\Program Files");assert(!ResolveFixedPlatformRoots(x64,x86));
 std::cout<<"PASS: real fixed roots: x86-on-x64, x86 Windows, malformed/missing registry, paths and aliases\n";
}
'''
with tempfile.TemporaryDirectory(prefix="glass-roots-") as directory:
    cpp = Path(directory) / "test.cpp"
    exe = Path(directory) / "test"
    cpp.write_text(prefix + read_value + "\n" + validate + "\n" + architecture + "\n" + default_root + "\n" + resolve + "\n" + main, encoding="utf-8")
    subprocess.run([os.environ.get("CXX", "g++"), "-std=c++17", "-g", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", "-no-pie", str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
