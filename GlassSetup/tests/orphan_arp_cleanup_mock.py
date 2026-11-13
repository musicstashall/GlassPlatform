"""Actual known-product orphan removal in both views, with fail-closed MSI inventory."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassInstallMsi.cpp').read_text()
fn=s[s.index('void RemoveOrphanUninstallEntry('):s.index('// Read-only post-removal')]
h=r'''
#include <string>
#include <cwchar>
#include <cassert>
#include <initializer_list>
using HKEY=int;using DWORD=unsigned;
constexpr int HKEY_LOCAL_MACHINE=1,HKEY_CURRENT_USER=2,KEY_WOW64_64KEY=256,KEY_WOW64_32KEY=512,KEY_READ=1,KEY_WRITE=2,DELETE=4;
constexpr unsigned ERROR_SUCCESS=0,ERROR_NO_MORE_ITEMS=259,ERROR_INSTALL_ALREADY_RUNNING=1618,MSIINSTALLCONTEXT_ALL=7;
constexpr const wchar_t *kProductPlatformX64=L"p64",*kProductPlatformX86=L"p32",*kProductSettingsX64=L"s64",*kProductSettingsX86=L"s32",*kProductOpenGlassResourcesX64=L"res",*kProductOpenGlass=L"og",*kLegacyPlatformX64=L"old64",*kLegacyPlatformX86=L"old32",*kLegacyOpenGlass=L"oldog";
int _wcsicmp(const wchar_t* a,const wchar_t* b){return wcscasecmp(a,b);}
unsigned inventory=259,calls=0,removed=0,opened=0,closed=0;bool raced=false,denied=false;unsigned mask=0;
unsigned MsiEnumProductsExW(const wchar_t*,const wchar_t* sid,unsigned mode,unsigned,wchar_t*,void*,void*,void*){
 assert(std::wstring(sid)==L"S-1-1-0"&&mode==7);++calls;return raced&&calls>1?0:inventory;}
long RegOpenKeyExW(HKEY root,const wchar_t* path,unsigned,unsigned access,HKEY* h){
 assert(std::wstring(path)==L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall");
 ++opened;if(denied)return 5;*h=root*1000+(access&768);return 0;}
long RegDeleteTreeW(HKEY h,const wchar_t* code){assert(std::wstring(code)==L"s64");++removed;
 unsigned bit=h==1256?1:h==1512?2:h==2256?4:8;mask|=bit;return 0;}
void RegCloseKey(HKEY){++closed;}
namespace GlassInstallLog{template<class... T>void LineFormat(const wchar_t*,T...) {}}
// FN
void reset(){inventory=259;calls=removed=opened=closed=mask=0;raced=denied=false;}
int main(){
 reset();RemoveOrphanUninstallEntry(L"s64");assert(removed==4&&mask==15&&closed==4);
 for(unsigned status:{0u,5u,87u,1605u}){reset();inventory=status;RemoveOrphanUninstallEntry(L"s64");assert(removed==0&&opened==0);}
 reset();raced=true;RemoveOrphanUninstallEntry(L"s64");assert(removed==0&&closed==4);
 reset();denied=true;RemoveOrphanUninstallEntry(L"s64");assert(removed==0&&closed==0);
 for(const auto* code:{L"foreign",L"sut",L"{official}_is1",L""}){reset();RemoveOrphanUninstallEntry(code);assert(calls==0&&removed==0);}
}
'''.replace('// FN',fn)
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';p.write_text(h);exe=Path(d)/'test'
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: actual orphan cleanup: owned GUID only, both HKLM/HKCU views, all-context absence/recheck, inventory errors, re-registration, access denied and foreign entries')
