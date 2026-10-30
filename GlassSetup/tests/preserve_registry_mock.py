#!/usr/bin/env python3
"""Exercise the real MSI user-path preservation against a fake registry.

A platform MSI writes an HKCU value as part of its own component. The snapshot
restores a previously existing value byte-for-byte, even when another registry
copy was the selected destination. This is not a real Windows/MSI test.
"""
from pathlib import Path
import os
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[2]
rollback = (repo / 'GlassSetup/src/InstallSupport/GlassInstallRollback.cpp').read_text(encoding='utf-8')
engine = (repo / 'GlassSetup/src/InstallEngine.cpp').read_text(encoding='utf-8')


def function(text, token):
    start = text.index(token)
    brace = text.index('{', start)
    depth = 0
    for index in range(brace, len(text)):
        if text[index] == '{':
            depth += 1
        elif text[index] == '}':
            depth -= 1
            if depth == 0:
                return text[start:index + 1]
    raise ValueError(token)

sequence = function(engine, 'bool InstallEngine::RunMsiSequence(')
assert sequence.index('RunOneMsi(msiPath') < sequence.index('PreservePlatformUserPath(')
assert sequence.index('PreservePlatformUserPath(') < sequence.index('WritePlatformInstallPath(')

prefix = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <map>
#include <string>
#include <tuple>
#include <vector>
using DWORD=uint32_t; using BYTE=unsigned char; using REGSAM=DWORD; using LSTATUS=int32_t; using HKEY=void*;
constexpr DWORD ERROR_SUCCESS=0,ERROR_ACCESS_DENIED=5,ERROR_INVALID_DATA=13,KEY_SET_VALUE=2;
HKEY const HKEY_CURRENT_USER=reinterpret_cast<HKEY>(1);
HKEY const HKEY_LOCAL_MACHINE=reinterpret_cast<HKEY>(2);
DWORD lastError=0; bool denyWrite=false;
void SetLastError(DWORD value){lastError=value;}
int _wcsicmp(const wchar_t* a,const wchar_t* b){
 while(*a&&*b){auto x=std::towlower(*a++),y=std::towlower(*b++);if(x!=y)return x<y?-1:1;}return *a?1:*b?-1:0;
}
struct Value {DWORD type;std::vector<BYTE> data;};
using Key=std::tuple<HKEY,std::wstring,std::wstring>;
std::map<Key,Value> registry;
struct Handle {HKEY hive;std::wstring key;};
LSTATUS RegCreateKeyExW(HKEY hive,const wchar_t* path,DWORD,void*,DWORD,REGSAM,void*,HKEY* key,void*){
 if(denyWrite)return ERROR_ACCESS_DENIED;*key=new Handle{hive,path};return ERROR_SUCCESS;
}
LSTATUS RegSetValueExW(HKEY raw,const wchar_t* name,DWORD,DWORD type,const BYTE* data,DWORD size){
 if(denyWrite)return ERROR_ACCESS_DENIED;auto* h=static_cast<Handle*>(raw);
 std::vector<BYTE> bytes;if(size){assert(data);bytes.assign(data,data+size);}registry[{h->hive,h->key,name}]={type,bytes};return 0;
}
LSTATUS RegCloseKey(HKEY raw){delete static_cast<Handle*>(raw);return 0;}
struct RegistryValueSnapshot {HKEY hive=nullptr;REGSAM view=0;std::wstring key,name;DWORD type=0;std::vector<BYTE> data;bool existed=false;};
struct SessionState {std::vector<RegistryValueSnapshot> registryValues;};
'''
main = r'''
int main(){
 const std::wstring path=L"Software\\GlassPlatform";
 RegistryValueSnapshot prior{HKEY_CURRENT_USER,0,path,L"InstallPathX86",2,{7,0,4,0,0,0},true};
 RegistryValueSnapshot other{HKEY_CURRENT_USER,0,path,L"InstallPathX64",1,{1,2},false};
 SessionState state{{prior,other}};
 const Key x86={HKEY_CURRENT_USER,path,L"InstallPathX86"};
 const Key x64={HKEY_CURRENT_USER,path,L"InstallPathX64"};
 registry[x86]={1,{9,9}};registry[x64]={1,{8,8}}; // what MSI just wrote
 assert(PreservePlatformUserPath(state,false));
 assert(registry[x86].type==2 && registry[x86].data==prior.data);
 assert(PreservePlatformUserPath(state,true));
 assert(registry[x64].type==1 && registry[x64].data==std::vector<BYTE>({8,8}));
 denyWrite=true;assert(!PreservePlatformUserPath(state,false) && lastError==ERROR_ACCESS_DENIED);
 denyWrite=false;SessionState missing;
 assert(!PreservePlatformUserPath(missing,false) && lastError==ERROR_INVALID_DATA);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='glass-preserve-registry-') as directory:
    cpp = Path(directory) / 'test.cpp'
    exe = Path(directory) / 'test'
    cpp.write_text(prefix + '\n' + function(rollback, 'bool PreservePlatformUserPath(') + '\n' + main,
                   encoding='utf-8')
    subprocess.run([os.environ.get('CXX', 'g++'), '-std=c++17', '-g', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-no-pie', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('PASS: preserve existing MSI-owned HKCU values byte-for-byte; missing values and errors')
