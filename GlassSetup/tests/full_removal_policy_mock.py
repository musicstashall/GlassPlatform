"""Production full-removal predicate and stale ARP command guard; fake MSI APIs."""
from pathlib import Path
import subprocess,tempfile,shutil
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassMaintenanceLaunch.cpp').read_text()
a=s.index('bool MaintenanceProductAbsent(');b=s.index('\nbool AuthorizeMaintenanceModifyLaunch',a)
h=r'''
#include "GlassSetup/src/GlassProducts.h"
#include <string>
#include <cwchar>
#include <cassert>
using UINT=unsigned;
constexpr unsigned ERROR_NO_MORE_ITEMS=259,ERROR_UNKNOWN_PRODUCT=1605,MSIINSTALLCONTEXT_ALL=7;
int _wcsicmp(const wchar_t* a,const wchar_t* b){return wcscasecmp(a,b);}
namespace GlassInstallMsi {constexpr const wchar_t* kProductPlatformX64=L"p64",*kProductPlatformX86=L"p32",*kProductSettingsX64=L"s64",*kProductSettingsX86=L"s32",*kProductOpenGlass=L"og",*kProductOpenGlassResourcesX64=L"res",*kLegacyPlatformX64=L"old64",*kLegacyPlatformX86=L"old32",*kLegacyOpenGlass=L"oldog";}
namespace GlassOpenGlassInstall {struct Record{bool installed=false;};bool present=false,valid=true;bool Query(Record& r,std::wstring&){r.installed=present;return valid;}}
namespace GlassInstallUtil {struct InstallRoots{std::wstring openGlass=L"root";};bool valid=true;bool ResolveInstallRoots(InstallRoots&){return valid;}}
namespace GlassOpenGlassFiles {bool receipt=false;bool HasReceipt(const std::wstring&){return receipt;}}
unsigned result=ERROR_NO_MORE_ITEMS,calls=0;
unsigned MsiEnumProductsExW(const wchar_t*,const wchar_t*,unsigned mode,unsigned,wchar_t*,void*,void*,void*){assert(mode==MSIINSTALLCONTEXT_ALL);++calls;return result;}
// GUARD
int main(){
 for(unsigned installed=0;installed<128;++installed)for(unsigned removes=0;removes<128;++removes)for(bool vendor:{false,true}){
  if(installed&16||removes&16)continue;
  GlassProducts::Selection choice;choice.platform64=choice.platform32=choice.glassSettings=choice.openGlass=false;
  choice.independentSettings=true;choice.removeMask=removes;
  const bool expected=installed&&removes&&(removes&installed)==installed;
  assert(GlassProducts::FullRemovalSelected(choice,installed,vendor)==expected);
  choice.platform32=true;assert(!GlassProducts::FullRemovalSelected(choice,installed,vendor));
 }
 GlassProducts::Selection vendor;vendor.platform64=vendor.platform32=vendor.glassSettings=vendor.openGlass=false;vendor.secureUxThemeRemove=true;
 assert(GlassProducts::FullRemovalSelected(vendor,0,true));assert(!GlassProducts::FullRemovalSelected(vendor,1,true));
 assert(MaintenanceProductAbsent(L"res"));assert(MaintenanceProductAbsent(L"p64"));result=ERROR_UNKNOWN_PRODUCT;assert(MaintenanceProductAbsent(L"s32"));
 result=0;assert(!MaintenanceProductAbsent(L"p64"));result=5;assert(!MaintenanceProductAbsent(L"p64"));
 result=ERROR_NO_MORE_ITEMS;
 for(const auto* og:{L"og",L"oldog"}){
  GlassOpenGlassInstall::present=true;assert(!MaintenanceProductAbsent(og));GlassOpenGlassInstall::present=false;
  GlassOpenGlassInstall::valid=false;assert(!MaintenanceProductAbsent(og));GlassOpenGlassInstall::valid=true;
  GlassOpenGlassFiles::receipt=true;assert(!MaintenanceProductAbsent(og));GlassOpenGlassFiles::receipt=false;
  GlassInstallUtil::valid=false;assert(!MaintenanceProductAbsent(og));GlassInstallUtil::valid=true;
  assert(MaintenanceProductAbsent(og));result=0;assert(!MaintenanceProductAbsent(og));result=ERROR_NO_MORE_ITEMS;
 }
 calls=0;assert(!MaintenanceProductAbsent(L"unknown")&&calls==0);
}
'''.replace('// GUARD',s[a:b])
engine=(root/'GlassSetup/src/InstallEngine.cpp').read_text()
assert engine.index('ok = self->PrepareFullRemovalCleanup()')<engine.index('ok=self->PrepareManagedRemoval()')
managed=engine[engine.index('DWORD WINAPI InstallEngine::RunGlassWorker('):engine.index('bool InstallEngine::Start(')]
assert managed.index('Launch: commit point reached')<managed.index('self->FinishFullRemovalCleanup()')
wrapper=engine[engine.index('DWORD WINAPI InstallEngine::WorkerThread('):engine.index('DWORD WINAPI InstallEngine::RunGlassWorker(')]
assert wrapper.index('CommitOfficialOpenGlass(mixedRemoval)')<wrapper.index('FinishFullRemovalCleanup()')
main=(root/'GlassSetup/src/main.cpp').read_text()
assert 'fullCleanupReviewed_=false;selection_.cleanupUserData=selection_.cleanupOpenGlassConfig=false;' in main
assert 'fullCleanupReviewed_=true;' in main
assert 'maintenanceLease.handle=INVALID_HANDLE_VALUE' in main
assert 'if(maintenanceMode_&&page_==Page::Components)return 0;' in main
transition=main[main.index('    void SetPage(Page page)'):main.index('    void SetPage(Page page)')+600]
assert 'maintenanceMode_&&(page==Page::License||page==Page::Welcome)' in transition
assert transition.index('page=Page::Components')<transition.index('page_=page')
assert 'return webReady_;' in main[main.index('bool SlidesVisible()'):main.index('bool SlidesVisible()')+350]

assert 'fullRemovalProfiles_.CleanupRegistry(detail)' in engine
assert 'if (restored && (!self->mixedOpenGlass_ || !ok))' in engine
with tempfile.TemporaryDirectory(prefix='GlassFullRemoval-') as temp:
 p=Path(temp)/'test.cpp';p.write_text(h);exe=Path(temp)/'test'
 subprocess.run([shutil.which('g++'),'-std=c++17','-Wall','-Wextra','-Werror','-I'+str(root),str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: production full/partial/mixed/vendor removal gate and stale ARP guard; preflight before mutation, cleanup after commit, explicit UI consent')
