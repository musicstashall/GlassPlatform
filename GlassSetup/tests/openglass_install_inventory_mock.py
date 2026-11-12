#!/usr/bin/env python3
"""Compile the actual read-only Inno inventory with fake registry APIs."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
header = root / 'GlassSetup/src/InstallSupport/GlassOpenGlassInstall.h'
source = '\n'.join(line for line in header.read_text().splitlines()
                   if not line.startswith('#include') and line != '#pragma once')
harness = r'''
#include <string>
#include <vector>
#include <utility>
#include <map>
#include <cassert>
#include <cwchar>
#include <cstring>
#include "GlassSetup/src/InstallSupport/GlassPackageRules.h"
using DWORD=unsigned long;using BYTE=unsigned char;using HKEY=void*;
constexpr int HKEY_LOCAL_MACHINE=1,KEY_QUERY_VALUE=1,KEY_WOW64_64KEY=256;
constexpr int ERROR_SUCCESS=0,ERROR_FILE_NOT_FOUND=2,ERROR_PATH_NOT_FOUND=3,REG_SZ=1;
int opened=0,live=0;bool badType=false,badTerminator=false;std::map<std::wstring,std::wstring> values;
int RegOpenKeyExW(int,const wchar_t* key,int,int access,HKEY* out){
 assert(access==(KEY_QUERY_VALUE|KEY_WOW64_64KEY));
 assert(std::wstring(key).find(L"{D3D1BC7D-5E24-4B33-9383-7934271A3B05}_is1")!=std::wstring::npos);
 if(opened)return opened;*out=reinterpret_cast<void*>(1);++live;return 0;
}
int RegQueryValueExW(HKEY,const wchar_t* name,void*,DWORD* type,BYTE* data,DWORD* size){
 auto it=values.find(name);if(it==values.end())return 2;
 *type=badType?99:REG_SZ;auto n=(it->second.size()+1)*sizeof(wchar_t);
 if(data){assert(*size>=n);memcpy(data,it->second.c_str(),n);if(badTerminator)reinterpret_cast<wchar_t*>(data)[it->second.size()]=L'x';}
 *size=n;return 0;
}
void RegCloseKey(HKEY){--live;}
namespace GlassInstallUtil {
std::wstring TrimTrailingSlashes(std::wstring p){while(!p.empty()&&p.back()==L'\\')p.pop_back();return p;}
bool IsSafeInstallPath(const std::wstring& p){return p.find(L"..") == std::wstring::npos;}
bool SameInstallPath(const std::wstring& a,const std::wstring& b){return a==b;}
}
'''
tests = r'''
int main(){
 using namespace GlassOpenGlassInstall;Record r;std::wstring error;
 values={{L"InstallLocation",L"D:\\Custom OG\\"},{L"DisplayVersion",L"3.0.2.3749"},{L"UninstallString",L"\"D:\\Custom OG\\unins000.exe\""}};
 assert(Query(r,error)&&r.installed&&r.directory==L"D:\\Custom OG"&&live==0);
 auto saved=r;assert(Same(saved,r));r.version=L"3.0.2.3750";assert(!Same(saved,r));
 opened=2;assert(Query(r,error)&&!r.installed&&error.empty()&&!HasProduct());
 opened=5;assert(!Query(r,error)&&!r.installed&&!error.empty()&&HasProduct());opened=0;
 badType=true;assert(!Query(r,error)&&live==0);badType=false;
 badTerminator=true;assert(!Query(r,error)&&live==0);badTerminator=false;
 auto good=values;
 for(auto command:{L"cmd.exe /c bad",L"\"D:\\Custom OG\\unins000.exe\" /extra",L"D:\\Else\\unins000.exe"}){
 values[L"UninstallString"]=command;assert(!Query(r,error)&&live==0);}
 values=good;values[L"DisplayVersion"]=L"unknown";assert(!Query(r,error));
 values=good;values[L"InstallLocation"]=L"relative";assert(!Query(r,error));
 values=good;values.erase(L"InstallLocation");assert(!Query(r,error)&&live==0);
 values=good;values[L"InstallLocation"]=std::wstring(L"D:\\Custom OG\0hidden",19);assert(!Query(r,error)&&live==0);
 values=good;assert(Query(r,error)&&r.installed&&live==0);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    cpp=Path(tmp)/'inventory.cpp';exe=Path(tmp)/'inventory'
    cpp.write_text(harness+'\n'+source+'\n'+tests)
    subprocess.run(['g++','-std=c++17','-I',str(root),str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('OpenGlass Inno read-only inventory mock: PASS')
