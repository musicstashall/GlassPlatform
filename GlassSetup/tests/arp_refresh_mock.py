"""Compile actual managed ARP writer/refresh with fake read-only MSI + registry APIs.
No real Windows registry, ownership/context security or MSI transaction exercised.
"""
from pathlib import Path
import re, shutil, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassInstallMsi.cpp').read_text()
# Test the real managed writer/refresh; vendor/file/Resources adapters have their
# own compiled harnesses, so isolate those independent dependencies here.
code=s[s.index('bool PatchArpUninstallString('):s.index('// A separate, non-MSI registration.')]
code+='\nbool RefreshOpenGlassFileArp(const std::wstring&){return true;}\nbool PatchOfficialOpenGlassMaintenance(){return true;}\nbool PatchResourcesMaintenance(){return true;}\nnamespace GlassAppsNotification{void Changed(){}}\n'
code+=s[s.index('bool RefreshManagedArp('):s.index('DWORD CALLBACK SnapshotCopyProgress')]
h=(root/'GlassSetup/src/InstallSupport/GlassInstallMsi.h').read_text()
constants='\n'.join(re.findall(r'inline constexpr wchar_t k(?:Product|Legacy)\w+\[\]\s*=\s*L"[^"\n]+";',h))
# New package metadata and repair of old ARP entries must agree.
import xml.etree.ElementTree as ET
for arch,bits in [('x64','64'),('x86','32')]:
    project=ET.parse(root/f'GlassSetup/msi/GlassSettings-{arch}.wixproj')
    assert project.findtext('.//GlassPlatformTitle')==f'GlassSettings ({bits}-bit)'
unins=(root/'GlassSetup/GlassUnins/main.cpp').read_text()
guard=unins[unins.index('        struct RemainingArpRefresh {'):unins.index('        bool msiRebootRequired=false;')]
harness=r'''
#include <cassert>
#include <cstdint>
#include <cwchar>
#include <iostream>
#include <map>
#include <string>
#include <vector>
using DWORD=unsigned;using REGSAM=unsigned;using LONG=int;using HKEY=void*;using BYTE=unsigned char;
using MSIINSTALLCONTEXT=int;
constexpr int INSTALLSTATE_DEFAULT=5,INSTALLSTATE_UNKNOWN=-1,INSTALLSTATE_ABSENT=2;
constexpr int ERROR_SUCCESS=0,ERROR_FILE_NOT_FOUND=2,ERROR_PATH_NOT_FOUND=3,KEY_SET_VALUE=4,KEY_WOW64_64KEY=64,KEY_WOW64_32KEY=32,
 REG_SZ=1,REG_DWORD=4,
 MSIINSTALLCONTEXT_NONE=0,MSIINSTALLCONTEXT_MACHINE=4,MSIINSTALLCONTEXT_ALL=7,MSIINSTALLCONTEXT_USERMANAGED=1,MSIINSTALLCONTEXT_USERUNMANAGED=2;
#define _wcsicmp wcscasecmp
const HKEY HKEY_LOCAL_MACHINE=reinterpret_cast<HKEY>(1),HKEY_CURRENT_USER=reinterpret_cast<HKEY>(2);
std::map<std::wstring,int> states;int context=4;bool safe=true,familyError=false,original=false;
std::map<int,std::map<std::wstring,std::wstring>> registry;
std::wstring failValue;int opens=0,writes=0;bool denied64=false,changeContext=false;
int MsiQueryProductStateW(const wchar_t* c){auto it=states.find(c);return it==states.end()?-1:it->second;}
int MsiEnumProductsExW(const wchar_t*,void*,int,int,wchar_t*,int* out,void*,void*){*out=context;return 0;}
int RegOpenKeyExW(HKEY root,const wchar_t*,int,REGSAM sam,HKEY* out){++opens;if(denied64&&(sam&KEY_WOW64_64KEY))return 5;if(changeContext)context=2;int key=static_cast<int>(reinterpret_cast<std::intptr_t>(root))*100+(sam&~KEY_SET_VALUE);if(!registry.count(key))return 2;*out=reinterpret_cast<HKEY>(key);return 0;}
int RegSetValueExW(HKEY key,const wchar_t* name,int,int type,const BYTE* value,DWORD){++writes;if(failValue==name)return 5;
 registry.at(static_cast<int>(reinterpret_cast<std::intptr_t>(key)))[name]=type==REG_DWORD?std::to_wstring(*reinterpret_cast<const DWORD*>(value)):std::wstring(reinterpret_cast<const wchar_t*>(value));return 0;}
int RegDeleteValueW(HKEY key,const wchar_t* name){return registry.at(static_cast<int>(reinterpret_cast<std::intptr_t>(key))).erase(name)?0:2;}
void RegCloseKey(HKEY){}
namespace Shared::SecureUxThemeInventory {unsigned Query(std::vector<std::wstring>& out){out.clear();if(original)out.push_back(L"vendor-code");return familyError?5:0;}}
namespace GlassMaintenanceCache {bool setupAvailable=true;std::wstring CurrentFile(const wchar_t* name){if(std::wstring(name)==L"GlassSetup.exe"&&!setupAvailable)return {};return std::wstring(L"Current\\")+name;}}
namespace GlassInstallLog {template<class... A>void LineFormat(const wchar_t*,A...) {}}
namespace GlassInstallMsi {
// CONSTANTS
bool IsProductInstalled(const wchar_t* code){return MsiQueryProductStateW(code)==5;}
enum class ProductPathCheck {Safe,Unsafe};
ProductPathCheck CheckProductContexts(const wchar_t*,std::wstring&){return safe?ProductPathCheck::Safe:ProductPathCheck::Unsafe;}
std::wstring UninsExePathForProduct(const std::wstring&,const std::wstring&){return L"local\\unins000.exe";}
// SOURCE
}
namespace Msi=GlassInstallMsi;
void LogLine(const std::wstring&){}
int ExecuteWithActualGuard(bool primaryFailure){
 std::wstring x64Dir_=L"x64",x86Dir_=L"x86",openGlassDir_=L"og";
// GUARD
 if(primaryFailure)return 42;
 return arpRefresh.Refresh()?0:9;
}
void Reset(){GlassMaintenanceCache::setupAvailable=true;states.clear();context=4;safe=true;familyError=original=false;registry.clear();registry[164]={};opens=writes=0;failValue.clear();denied64=changeContext=false;}
int main(){using namespace GlassInstallMsi;
 for(int n=0;n<5;++n){Reset();states[kProductSettingsX64]=5;
  if(n==1)original=true;
  if(n==2)familyError=true;
  if(n==3)states[kProductSecureUxThemeX86]=5;
  if(n==4)states[kProductSecureUxThemeX86]=2;
  assert(PatchArpUninstallString(kProductSettingsX64,L"x64"));
  assert(registry[164][L"DisplayName"]==L"GlassSettings (64-bit)");
  assert(registry[164][L"NoRemove"]==L"0");
  assert(registry[164][L"ModifyPath"].find(L"--modify --product")!=std::wstring::npos);
 }
 {Reset();states[kProductSettingsX86]=5;assert(PatchArpUninstallString(kProductSettingsX86,L"x86"));
  assert(registry[164][L"DisplayName"]==L"GlassSettings (32-bit)");}
 {Reset();states[kProductSettingsX86]=5;registry.clear();registry[132]={};registry[164]={{L"DisplayName",L"stale"}};
  assert(PatchArpUninstallString(kProductSettingsX86,L"x86"));assert(registry[132][L"DisplayName"]==L"GlassSettings (32-bit)");
  assert(registry[164][L"DisplayName"]==L"stale"&&opens==1);}
 {Reset();states[kProductSettingsX64]=5;failValue=L"DisplayName";assert(!PatchArpUninstallString(kProductSettingsX64,L"x64"));}
 for(int n=0;n<4;++n){Reset();states[kProductSettingsX64]=5;
  if(n==0)safe=false;
  if(n==1)states[kProductSettingsX64]=2;
  if(n==2)context=0;
  if(n==3){registry.clear();registry[264]={};}
  assert(!PatchArpUninstallString(kProductSettingsX64,L"x64"));assert(writes==0);
 }
 {Reset();states[kProductSettingsX64]=5;registry[132]={};denied64=true;
  assert(!PatchArpUninstallString(kProductSettingsX64,L"x64"));assert(opens==1&&writes==0);}
 {Reset();states[kProductSettingsX64]=5;changeContext=true;
  assert(!PatchArpUninstallString(kProductSettingsX64,L"x64"));assert(writes==0);}
 {Reset();states[L"vendor-code"]=5;assert(!PatchArpUninstallString(L"vendor-code",L"x64"));assert(opens==0&&writes==0);}
 {Reset();context=2;registry.clear();registry[264]={};states[kProductSettingsX64]=5;
  assert(PatchArpUninstallString(kProductSettingsX64,L"x64"));assert(registry[264].count(L"NoRemove"));}
 {Reset();registry.clear();registry[132]={};states[kProductSettingsX86]=5;
  assert(PatchArpUninstallString(kProductSettingsX86,L"x86"));assert(registry[132].count(L"NoRemove"));}
 {Reset();registry[132]={};registry[164][L"WindowsInstaller"]=L"1";states[kProductSettingsX64]=5;failValue=L"NoRemove";
  assert(!PatchArpUninstallString(kProductSettingsX64,L"x64"));assert(registry[164].count(L"WindowsInstaller"));assert(registry[132].empty());}
 {Reset();states[kProductSettingsX64]=5;original=true;assert(RefreshManagedArp(L"x64",L"x86",L"og"));assert(registry[164][L"NoRemove"]==L"0");
  original=false;assert(RefreshManagedArp(L"x64",L"x86",L"og"));assert(registry[164][L"NoRemove"]==L"0");}
 {Reset();states[kProductSettingsX64]=5;GlassMaintenanceCache::setupAvailable=false;
  assert(PatchArpUninstallString(kProductSettingsX64,L"x64"));assert(registry[164][L"ModifyPath"].find(L"--interactive --product")!=std::wstring::npos);}
 {Reset();states[kLegacyPlatformX86]=5;assert(RefreshManagedArp(L"x64",L"x86",L"og"));assert(writes>0);}
 {Reset();states[kProductPlatformX64]=2;assert(!RefreshManagedArp(L"x64",L"x86",L"og"));assert(writes==0);}
 {Reset();states[kProductSettingsX64]=5;assert(ExecuteWithActualGuard(true)==42);assert(registry[164][L"NoRemove"]==L"0"&&writes==8);}
 {Reset();states[kProductSettingsX64]=5;assert(ExecuteWithActualGuard(false)==0);assert(writes==8);}
 {Reset();states[kProductSettingsX64]=5;failValue=L"NoRemove";assert(ExecuteWithActualGuard(false)==9);assert(writes==8);}
 std::cout<<"PASS: actual ARP refresh: remaining/legacy products, vendor exclusion, original/repack/unknown NoRemove, context/root isolation, write failure, stale-policy reset\n";
}
'''.replace('// CONSTANTS',constants).replace('// SOURCE',code).replace('// GUARD',guard)
with tempfile.TemporaryDirectory(prefix='GlassArp-') as d:
 p=Path(d)/'test.cpp';p.write_text(harness);exe=Path(d)/'test'
 subprocess.run([shutil.which('clang++') or shutil.which('g++'),'-std=c++17','-Wall','-Wextra',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
# Actual call sites must re-enumerate after partial MSI success/failure too.
unins=(root/'GlassSetup/GlassUnins/main.cpp').read_text()
assert unins.index('struct RemainingArpRefresh')<unins.index('const auto result=GlassMaintenanceExecutor::RunPhase(removalPlan')
assert '~RemainingArpRefresh()' in unins
engine=(root/'GlassSetup/src/InstallEngine.cpp').read_text()
launch=engine[engine.index('void InstallEngine::LaunchSelectedProducts()'):]
assert launch.index('GlassMaintenanceCache::Publish(')<launch.index('RefreshManagedArp(')
