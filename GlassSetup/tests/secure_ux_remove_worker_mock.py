"""Actual Setup vendor-removal methods with fake helper/MSI calls; no Windows IO."""
from pathlib import Path
import subprocess, shutil, tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'GlassSetup/src/InstallEngine.cpp').read_text()
methods=source[source.index('bool InstallEngine::PrepareSecureUxRemoval()'):source.index('bool InstallEngine::InstallOriginalSecureUxTheme()')]
worker=source[source.index('DWORD WINAPI InstallEngine::WorkerThread('):source.index('bool InstallEngine::Start(')]
assert worker.index('CreateRollbackSnapshot()')<worker.index('PrepareSecureUxRemoval()')<worker.index('StopRunningHosts()')
assert worker.index('RegisterSelectedProducts()')<worker.index('InstallOriginalSecureUxTheme()')<worker.index('LaunchSelectedProducts()')
assert 'vendorOnly || self->CapturePriorHostState()' in worker
assert 'if(!vendorOnly)ok = self->RegisterSelectedProducts()' in worker
assert 'if(!vendorOnly)self->LaunchSelectedProducts()' in worker
assert 'if (ok && !vendorOnly)' in worker
assert 'if (ok && !vendorOnly && !self->IsCancelRequested())' in worker
assert 'PerformRollback()' in worker and 'UndoCleanInstall()' in worker
assert 'NoteModified(' not in methods and 'NoteInstalled(' not in methods and 'InvokeSecureUxThemeDeactivate' not in methods
harness=r'''
#include "GlassProducts.h"
#include <atomic>
#include <cassert>
#include <functional>
#include <iostream>
#include <string>
using UINT=unsigned;
constexpr UINT ERROR_SUCCESS=0,ERROR_INSTALL_PACKAGE_REJECTED=1625,ERROR_SUCCESS_REBOOT_REQUIRED=3010,
 ERROR_SUCCESS_REBOOT_INITIATED=1641,INSTALLLOGMODE_VERBOSE=1,INSTALLLOGATTRIBUTES_FLUSHEACHLINE=1;
constexpr int LANG_RUSSIAN=25;
#define PRIMARYLANGID(x) (x)
int GetUserDefaultUILanguage(){return 0;}
int MsiEnableLogW(int,const wchar_t*,int){return 0;}
namespace InstallLog {
 template<class... Args>void LineFormat(const wchar_t*,Args...){}
 std::wstring NextMsiVerboseLogPath(const wchar_t*){return L"log";}
}
namespace GlassInstallUtil {
 enum class SecureUxThemeState {Unavailable,Inactive,Active,ActivationPending,DeactivationPending,External,Conflict};
 SecureUxThemeState state=SecureUxThemeState::Inactive;bool query=true;
 bool QuerySecureUxThemeState(const std::wstring&,const std::wstring&,SecureUxThemeState* out,std::wstring*){*out=state;return query;}
}
namespace GlassSecureUxPackage {
 struct Removal {bool ready=false;std::wstring productCode;};
 bool prepare=true,recheck=true;int prepares=0,rechecks=0;
 bool PrepareRemoval(const std::wstring&,const std::wstring& code,const std::atomic_bool&,Removal& out,std::wstring&){++prepares;if(!prepare||code.empty())return false;out={true,code};return true;}
 bool RecheckRemoval(const std::wstring&,const std::atomic_bool&,const Removal& r,std::wstring&){++rechecks;return recheck&&r.ready;}
}
namespace GlassInstallMsi {
 struct ProgressSink {std::wstring label;int base=0,span=0;const std::atomic_bool* cancelRequested=nullptr;
  std::function<void(const std::wstring&,int)> onProgress;};
 UINT result=0;int uninstalls=0;
 bool UninstallProduct(const wchar_t* code,ProgressSink* sink,UINT* out){
  assert(std::wstring(code)==L"official"&&sink->cancelRequested==nullptr);++uninstalls;*out=result;
  return result==0||result==3010||result==1641;
 }
}
struct InstallEngine {
 struct {bool secureUxTheme=false,secureUxThemeRemove=true;}selection_;
 GlassSecureUxPackage::Removal secureUxRemoval_;
 std::wstring secureUxRemovalCode_=L"official",tempDir_=L"work",x64Dir_=L"x64",x86Dir_=L"x86";
 std::atomic_bool cancelRequested_{false};int failures=0,warnings=0;
 bool IsCancelRequested()const{return cancelRequested_.load();}
 void SetFailure(const wchar_t*,unsigned){++failures;}
 void AddFinishWarning(const wchar_t*,const wchar_t*){++warnings;}
 void SetStatus(const std::wstring&,int){}
 bool PrepareSecureUxRemoval() noexcept;bool RemoveOriginalSecureUxTheme() noexcept;
};
// METHODS
void Reset(){GlassInstallUtil::state=GlassInstallUtil::SecureUxThemeState::Inactive;GlassInstallUtil::query=true;
 GlassSecureUxPackage::prepare=GlassSecureUxPackage::recheck=true;
 GlassSecureUxPackage::prepares=GlassSecureUxPackage::rechecks=0;GlassInstallMsi::uninstalls=0;GlassInstallMsi::result=0;}
int main(){
 for(unsigned mask=0;mask<128;++mask){
  GlassProducts::Selection choice;choice.secureUxThemeRemove=true;
  choice.platform64=mask&1;choice.platform32=mask&2;choice.glassSettings=mask&4;
  choice.openGlass=mask&8;choice.settings64=mask&16;choice.settings32=mask&32;choice.secureUxTheme=mask&64;
  assert(GlassProducts::OnlySecureUxThemeRemoval(choice)==(mask==0));
 }
 {Reset();InstallEngine e;assert(e.PrepareSecureUxRemoval());assert(e.RemoveOriginalSecureUxTheme());assert(GlassInstallMsi::uninstalls==1&&e.failures==0);}
 for(int n:{0,2,3,4,5,6}){
  Reset();InstallEngine e;GlassInstallUtil::state=static_cast<GlassInstallUtil::SecureUxThemeState>(n);
  assert(!e.PrepareSecureUxRemoval());assert(!e.RemoveOriginalSecureUxTheme());assert(GlassInstallMsi::uninstalls==0);
 }
 {Reset();InstallEngine e;assert(e.PrepareSecureUxRemoval());GlassInstallUtil::state=GlassInstallUtil::SecureUxThemeState::Active;
  assert(!e.RemoveOriginalSecureUxTheme());assert(GlassInstallMsi::uninstalls==0);}
 {Reset();InstallEngine e;GlassInstallUtil::query=false;assert(!e.PrepareSecureUxRemoval());assert(!e.RemoveOriginalSecureUxTheme());assert(GlassInstallMsi::uninstalls==0);}
 {Reset();InstallEngine e;GlassSecureUxPackage::prepare=false;assert(!e.PrepareSecureUxRemoval());assert(!e.RemoveOriginalSecureUxTheme());}
 {Reset();InstallEngine e;e.selection_.secureUxTheme=true;assert(!e.PrepareSecureUxRemoval());assert(!e.RemoveOriginalSecureUxTheme());assert(GlassInstallMsi::uninstalls==0);}
 {Reset();InstallEngine e;e.selection_.secureUxThemeRemove=false;assert(e.PrepareSecureUxRemoval());assert(!e.RemoveOriginalSecureUxTheme());assert(GlassSecureUxPackage::prepares==0&&GlassInstallMsi::uninstalls==0);}
 {Reset();InstallEngine e;assert(e.PrepareSecureUxRemoval());GlassSecureUxPackage::recheck=false;assert(!e.RemoveOriginalSecureUxTheme());assert(GlassInstallMsi::uninstalls==0);}
 {Reset();InstallEngine e;e.cancelRequested_=true;assert(!e.PrepareSecureUxRemoval());assert(GlassSecureUxPackage::prepares==0&&e.failures==0);}
 {Reset();InstallEngine e;e.secureUxRemovalCode_.clear();assert(!e.PrepareSecureUxRemoval());}
 for(unsigned code:{0u,1603u,3010u,1641u}){
  Reset();InstallEngine e;assert(e.PrepareSecureUxRemoval());GlassInstallMsi::result=code;
  assert(e.RemoveOriginalSecureUxTheme()==(code!=1603));assert(GlassInstallMsi::uninstalls==1);
  assert(e.failures==int(code==1603));assert(e.warnings==int(code==3010||code==1641));
 }
 std::cout<<"PASS: actual vendor removal worker: explicit intent, inactive-only, preflight/final revalidation, no UI cancellation after commit boundary, MSI failure/reboot; last-transaction order\n";
}
'''.replace('// METHODS',methods)
compiler=shutil.which('clang++') or shutil.which('g++')
if not compiler:raise SystemExit('clang++ or g++ required')
with tempfile.TemporaryDirectory(prefix='GlassSutRemoveWorker-') as directory:
    path=Path(directory)/'worker.cpp';exe=Path(directory)/'worker'
    path.write_text(harness)
    subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror','-pedantic','-I'+str(root/'GlassSetup/src'),str(path),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
