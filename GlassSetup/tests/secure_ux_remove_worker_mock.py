"""Actual Setup vendor-removal methods with fake helper/MSI calls; no Windows IO."""
from pathlib import Path
import subprocess, shutil, tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'GlassSetup/src/InstallEngine.cpp').read_text()
methods=source[source.index('bool InstallEngine::PrepareSecureUxRemoval()'):source.index('bool InstallEngine::BuildExecutionPlan()')]
worker=source[source.index('DWORD WINAPI InstallEngine::WorkerThread('):source.index('bool InstallEngine::Start(')]
assert worker.index('CreateRollbackSnapshot()')<worker.index('PrepareSecureUxRemoval()')<worker.index('StopRunningHosts()')
assert worker.index('RegisterSelectedProducts()')<worker.index('InstallOriginalSecureUxTheme()')<worker.index('LaunchSelectedProducts()')
assert 'vendorOnly || self->CapturePriorHostState()' in worker
assert 'if(!vendorOnly)ok = self->RegisterSelectedProducts()' in worker
assert 'if(!vendorOnly && !self->auxiliaryOnly_)self->LaunchSelectedProducts()' in worker
assert 'if (ok && !vendorOnly)' in worker
assert 'if (ok && !vendorOnly && !self->IsCancelRequested())' in worker
assert 'PerformRollback()' in worker and 'UndoCleanInstall()' in worker
assert 'NoteModified(' not in methods and 'NoteInstalled(' not in methods and 'InvokeSecureUxThemeDeactivate' not in methods
harness=r'''
#include "GlassProducts.h"
#include "GlassProductPlan.h"
#include <atomic>
#include <cassert>
#include <functional>
#include <iostream>
#include <string>
using UINT=unsigned;
#define _wcsicmp wcscasecmp
#include <cwchar>
constexpr UINT ERROR_INVALID_STATE=5023,ERROR_RETRY=1237,ERROR_SUCCESS=0,ERROR_INSTALL_PACKAGE_REJECTED=1625,ERROR_SUCCESS_REBOOT_REQUIRED=3010,
 ERROR_SUCCESS_REBOOT_INITIATED=1641,INSTALLLOGMODE_VERBOSE=1,INSTALLLOGATTRIBUTES_FLUSHEACHLINE=1;
constexpr int LANG_RUSSIAN=25;
#define PRIMARYLANGID(x) (x)
int GetUserDefaultUILanguage(){return 0;}
int MsiEnableLogW(int,const wchar_t*,int){return 0;}
namespace InstallLog {
 void Line(const wchar_t*){}
 template<class... Args>void LineFormat(const wchar_t*,Args...){}
 std::wstring NextMsiVerboseLogPath(const wchar_t*){return L"log";}
}
namespace GlassInstallUtil {
 enum class SecureUxThemeState {Unavailable,Inactive,Active,ActivationPending,DeactivationPending,External,Conflict};
 SecureUxThemeState state=SecureUxThemeState::Inactive;bool query=true;
 bool IsOs64Bit(){return true;}
 bool helperSuccess=true,lateExternal=false;int styleCalls=0;
 struct SecureUxThemeDeactivateRun {bool ran=false,themeNotRestored=false,skippedExternalInstall=false;};
 bool PrepareSecureUxRemovalVisualStyle(const std::wstring&,const std::wstring&,SecureUxThemeDeactivateRun* run,std::wstring*){++styleCalls;if(lateExternal)state=SecureUxThemeState::External;run->ran=helperSuccess;return helperSuccess;}
 bool pending=false,pendingKnown=true,overrideAfter=false;SecureUxThemeState after=SecureUxThemeState::Inactive;
 bool QuerySecureUxPendingFileOperations(bool& out){out=pending;return pendingKnown;}
 bool QuerySecureUxThemeState(const std::wstring&,const std::wstring&,SecureUxThemeState* out,std::wstring*){*out=state;return query;}
}
namespace GlassSecureUxPackage {
 struct Removal {bool ready=false;std::wstring productCode;};
 struct Package {std::wstring path=L"vendor.msi",productCode=L"official",version=L"4.0.0";bool alreadyInstalled=false;};
 bool presenceSafe=true,legacyAbsent=true;std::vector<std::wstring> installed;
 bool QueryInstallPresence(std::vector<std::wstring>& out,const std::atomic_bool&,std::wstring&){out=installed;return presenceSafe;}
 bool CheckLegacyRepackAbsent(std::wstring&){return legacyAbsent;}
 bool prepare=true,recheck=true;int prepares=0,rechecks=0,failRecheckAt=0;
 bool PrepareRemoval(const std::wstring&,const std::wstring& code,const std::atomic_bool&,Removal& out,std::wstring&){++prepares;if(!prepare||code.empty())return false;out={true,code};return true;}
 bool RecheckRemoval(const std::wstring&,const std::atomic_bool&,const Removal& r,std::wstring&){++rechecks;return recheck&&r.ready&&rechecks!=failRecheckAt;}
}
namespace GlassMaintenanceCache {
 bool valid=true;std::wstring code=L"official",version=L"4.0.0";
 bool Identity(const std::wstring&,std::wstring& outCode,std::wstring& outVersion){outCode=code;outVersion=version;return valid;}
}
namespace GlassInstallMsi {
 bool arpRefreshOk=true;
 bool RefreshManagedArp(const std::wstring&,const std::wstring&,const std::wstring&){return arpRefreshOk;}
 struct ProgressSink {std::wstring label;int base=0,span=0;const std::atomic_bool* cancelRequested=nullptr;
  std::function<void(const std::wstring&,int)> onProgress;};
 UINT result=0;int uninstalls=0,installs=0;
 bool MsiSuccess(UINT code){return code==0||code==3010||code==1641;}
 UINT InstallProduct(const std::wstring&,const std::wstring& properties,ProgressSink* sink){assert(properties==L"REBOOT=ReallySuppress"&&sink->cancelRequested==nullptr);++installs;if(GlassInstallUtil::overrideAfter)GlassInstallUtil::state=GlassInstallUtil::after;return result;}
 bool UninstallProduct(const wchar_t* code,ProgressSink* sink,UINT* out){
  assert(std::wstring(code)==L"official"&&sink->cancelRequested==nullptr);++uninstalls;*out=result;if(GlassInstallUtil::overrideAfter)GlassInstallUtil::state=GlassInstallUtil::after;
  return result==0||result==3010||result==1641;
 }
}
namespace GlassProfileMaintenance {
 bool preflight=true,recheckStyles=true,commitStyles=true;int preflights=0,commits=0,styleRechecks=0,failStyleAt=0;
 std::vector<std::pair<std::wstring,std::wstring>> skipped;
 struct Inventory {
 const auto& SkippedLoadedProfiles()const{return skipped;}
 bool PreflightLoadedRemoval(std::wstring&){++preflights;return preflight;}
 bool RecheckLoadedRemoval(std::wstring&)const{return recheckStyles&&++styleRechecks!=failStyleAt;}
 bool CommitLoadedRemovalStyle(std::wstring&){++commits;return commitStyles;}
 };
}
struct InstallEngine {
 GlassProductPlan::Plan executionPlan_{};bool executionPlanReady_=true;
 InstallEngine(){executionPlan_.rows[GlassProductPlan::Index(GlassProductPlan::Product::SecureUxTheme)].action=GlassProductPlan::Action::Remove;}
 struct {bool secureUxTheme=false,secureUxThemeRemove=true;unsigned removeMask=0;bool Removes(unsigned row)const{return (removeMask&(1u<<row))!=0;}}selection_;
 GlassSecureUxPackage::Removal secureUxRemoval_;
 GlassProfileMaintenance::Inventory secureUxRemovalProfiles_;bool secureUxLoadedRemoval_=false;
 GlassSecureUxPackage::Package secureUxPackage_;
 struct {std::wstring secureUxTheme;}labels_;
 std::wstring secureUxRemovalCode_=L"official",tempDir_=L"work",x64Dir_=L"x64",x86Dir_=L"x86",openGlassDir_=L"og";
 std::atomic_bool cancelRequested_{false};int failures=0,warnings=0;
 bool IsCancelRequested()const{return cancelRequested_.load();}
 void SetFailure(const std::wstring&,unsigned){++failures;}
 std::vector<std::wstring> warningDetails;
 void AddFinishWarning(const std::wstring&,const std::wstring& detail){++warnings;warningDetails.push_back(detail);}
 void SetStatus(const std::wstring&,int){}
 bool PrepareSecureUxRemoval() noexcept;bool RemoveOriginalSecureUxTheme() noexcept;bool InstallOriginalSecureUxTheme() noexcept;
};
// METHODS
void Reset(){GlassProfileMaintenance::skipped.clear();GlassInstallUtil::lateExternal=false;GlassSecureUxPackage::failRecheckAt=0;GlassProfileMaintenance::styleRechecks=GlassProfileMaintenance::failStyleAt=0;GlassInstallUtil::helperSuccess=true;GlassInstallUtil::styleCalls=0;
 GlassProfileMaintenance::preflight=GlassProfileMaintenance::recheckStyles=GlassProfileMaintenance::commitStyles=true;GlassProfileMaintenance::preflights=GlassProfileMaintenance::commits=0;GlassInstallUtil::overrideAfter=false;GlassInstallUtil::pending=false;GlassInstallUtil::pendingKnown=true;GlassInstallMsi::arpRefreshOk=true;GlassSecureUxPackage::presenceSafe=GlassSecureUxPackage::legacyAbsent=true;GlassSecureUxPackage::installed.clear();
 GlassMaintenanceCache::valid=true;GlassMaintenanceCache::code=L"official";GlassMaintenanceCache::version=L"4.0.0";
 GlassInstallMsi::installs=0;GlassInstallUtil::state=GlassInstallUtil::SecureUxThemeState::Inactive;GlassInstallUtil::query=true;
 GlassSecureUxPackage::prepare=GlassSecureUxPackage::recheck=true;
 GlassSecureUxPackage::prepares=GlassSecureUxPackage::rechecks=0;GlassInstallMsi::uninstalls=0;GlassInstallMsi::result=0;}
int main(){
 {Reset();InstallEngine e;GlassInstallUtil::state=GlassInstallUtil::SecureUxThemeState::Active;
  GlassProfileMaintenance::skipped={{L"SID-1008",L"C:\\Users\\OldUser"},{L"SID-1011",L"C:\\Users\\WsiAccount"}};
  assert(e.PrepareSecureUxRemoval());assert(e.warnings==0);assert(e.RemoveOriginalSecureUxTheme());
  assert(e.warnings==3&&GlassInstallMsi::uninstalls==1);
  assert(e.warningDetails[0].find(L"SID-1008")!=std::wstring::npos&&e.warningDetails[1].find(L"WsiAccount")!=std::wstring::npos);
 }

 for(auto state:{GlassInstallUtil::SecureUxThemeState::Active,GlassInstallUtil::SecureUxThemeState::DeactivationPending}){
  for(unsigned result:{0u,3010u}){Reset();InstallEngine e;GlassInstallUtil::state=state;
   assert(e.PrepareSecureUxRemoval());assert(e.secureUxLoadedRemoval_&&GlassInstallUtil::styleCalls==0);
   GlassInstallUtil::overrideAfter=true;GlassInstallUtil::after=GlassInstallUtil::SecureUxThemeState::DeactivationPending;GlassInstallMsi::result=result;
   assert(e.RemoveOriginalSecureUxTheme());assert(GlassInstallUtil::styleCalls==1&&GlassProfileMaintenance::commits==1&&GlassInstallMsi::uninstalls==1&&e.warnings==1);
  }
 }
 for(int late=0;late<3;++late){Reset();InstallEngine e;GlassInstallUtil::state=GlassInstallUtil::SecureUxThemeState::Active;assert(e.PrepareSecureUxRemoval());
  if(late==0)GlassInstallUtil::lateExternal=true;
  if(late==1)GlassSecureUxPackage::failRecheckAt=2;
  if(late==2)GlassProfileMaintenance::failStyleAt=3;
  assert(!e.RemoveOriginalSecureUxTheme());assert(GlassInstallUtil::styleCalls==1&&GlassInstallMsi::uninstalls==0);
 }
 // Loaded history cannot lose its restart when the post-query becomes inactive.
 {Reset();InstallEngine e;GlassInstallUtil::state=GlassInstallUtil::SecureUxThemeState::Active;assert(e.PrepareSecureUxRemoval());
  GlassInstallUtil::overrideAfter=true;GlassInstallUtil::after=GlassInstallUtil::SecureUxThemeState::Inactive;
  assert(e.RemoveOriginalSecureUxTheme());assert(e.warnings==1);}
 for(int failure=1;failure<6;++failure){Reset();InstallEngine e;GlassInstallUtil::state=GlassInstallUtil::SecureUxThemeState::Active;
  if(failure==1)GlassProfileMaintenance::preflight=false;
  if(failure<2){assert(!e.PrepareSecureUxRemoval());assert(GlassInstallUtil::styleCalls==0&&GlassSecureUxPackage::prepares==0);continue;}
  assert(e.PrepareSecureUxRemoval());
  if(failure==2)GlassProfileMaintenance::recheckStyles=false;
  if(failure==3)GlassInstallUtil::helperSuccess=false;
  if(failure==4)GlassProfileMaintenance::commitStyles=false;
  if(failure==5)GlassSecureUxPackage::recheck=false;
  assert(!e.RemoveOriginalSecureUxTheme());assert(GlassInstallMsi::uninstalls==0);
 }
 {Reset();InstallEngine e;GlassInstallUtil::state=GlassInstallUtil::SecureUxThemeState::Active;e.selection_.removeMask=1u<<2;
  assert(!e.PrepareSecureUxRemoval());assert(GlassInstallUtil::styleCalls==0&&GlassSecureUxPackage::prepares==0);}
 {Reset();InstallEngine e;GlassInstallUtil::state=GlassInstallUtil::SecureUxThemeState::Active;
  assert(e.PrepareSecureUxRemoval());GlassInstallMsi::result=1603;assert(!e.RemoveOriginalSecureUxTheme());assert(GlassInstallUtil::styleCalls==1&&e.failures==1);}

 // Fresh registration requires restart even if MSI reports success without 3010.
 for(unsigned result:{0u,3010u}){Reset();InstallEngine e;e.selection_.secureUxThemeRemove=false;e.selection_.secureUxTheme=true;
  e.executionPlan_.rows[GlassProductPlan::Index(GlassProductPlan::Product::SecureUxTheme)].action=GlassProductPlan::Action::Install;
  GlassInstallUtil::overrideAfter=true;GlassInstallUtil::after=GlassInstallUtil::SecureUxThemeState::ActivationPending;GlassInstallMsi::result=result;
  assert(e.InstallOriginalSecureUxTheme());assert(e.warnings==1);
 }

 // Install -> remove before reboot: registration disappears, hook never loaded.
 for(unsigned result:{0u,3010u}){Reset();InstallEngine e;GlassInstallUtil::state=GlassInstallUtil::SecureUxThemeState::ActivationPending;
  assert(e.PrepareSecureUxRemoval());GlassInstallUtil::overrideAfter=true;GlassInstallUtil::after=GlassInstallUtil::SecureUxThemeState::Inactive;GlassInstallMsi::result=result;
  assert(e.RemoveOriginalSecureUxTheme());assert(e.warnings==0);
 }
 // Reinstall while already loaded: advisory vendor reboot does not force reload.
 for(unsigned result:{0u,3010u}){Reset();InstallEngine e;e.selection_.secureUxThemeRemove=false;e.selection_.secureUxTheme=true;
  e.executionPlan_.rows[GlassProductPlan::Index(GlassProductPlan::Product::SecureUxTheme)].action=GlassProductPlan::Action::Install;
  GlassInstallUtil::overrideAfter=true;GlassInstallUtil::after=GlassInstallUtil::SecureUxThemeState::Active;GlassInstallMsi::result=result;
  assert(e.InstallOriginalSecureUxTheme());assert(e.warnings==0);
 }
 for(bool pendingKnown:{false,true}){Reset();InstallEngine e;assert(e.PrepareSecureUxRemoval());
  GlassInstallMsi::result=3010;GlassInstallUtil::pendingKnown=pendingKnown;GlassInstallUtil::pending=true;
  assert(e.RemoveOriginalSecureUxTheme());assert(e.warnings==1);
 }
 {Reset();InstallEngine e;assert(e.PrepareSecureUxRemoval());GlassInstallUtil::overrideAfter=true;
  GlassInstallUtil::after=GlassInstallUtil::SecureUxThemeState::DeactivationPending;
  assert(e.RemoveOriginalSecureUxTheme());assert(e.warnings==1);}

 {Reset();InstallEngine e;GlassInstallUtil::state=GlassInstallUtil::SecureUxThemeState::ActivationPending;
  assert(e.PrepareSecureUxRemoval());assert(e.RemoveOriginalSecureUxTheme());assert(GlassInstallMsi::uninstalls==1);}

 {Reset();InstallEngine e;assert(e.PrepareSecureUxRemoval());GlassInstallMsi::arpRefreshOk=false;
  assert(e.RemoveOriginalSecureUxTheme());assert(e.failures==0&&e.warnings==1&&GlassInstallMsi::uninstalls==1);}
 for(int n=0;n<16;++n){Reset();InstallEngine e;e.selection_.secureUxTheme=true;e.selection_.secureUxThemeRemove=false;
  e.executionPlan_.rows[GlassProductPlan::Index(GlassProductPlan::Product::SecureUxTheme)].action=GlassProductPlan::Action::Install;
  if(n==1)e.executionPlanReady_=false;
  if(n==2)e.executionPlan_.valid=false;
  if(n==3)e.executionPlan_.rows[GlassProductPlan::Index(GlassProductPlan::Product::SecureUxTheme)].action=GlassProductPlan::Action::Keep;
  if(n==4)GlassSecureUxPackage::presenceSafe=false;
  if(n==5)GlassSecureUxPackage::installed={L"official"};
  if(n==6)e.secureUxPackage_.alreadyInstalled=true;
  if(n==7)e.secureUxPackage_.path.clear();
  if(n==8)GlassSecureUxPackage::legacyAbsent=false;
  if(n==9)GlassMaintenanceCache::valid=false;
  if(n==10)GlassMaintenanceCache::code=L"other";
  if(n==11)GlassMaintenanceCache::version=L"4.1.0";
  if(n==12)GlassInstallMsi::result=1603;
  if(n==13)GlassInstallMsi::result=3010;
  if(n==14)GlassInstallMsi::result=1641;
  if(n==15)e.selection_.secureUxTheme=false;
  assert(e.InstallOriginalSecureUxTheme()==(n==0||n>=13));
  assert(GlassInstallMsi::installs==((n==0||(n>=12&&n<=14))?1:0));
  assert(GlassInstallMsi::uninstalls==0);
  assert(e.warnings==((n==13||n==14)?1:0));
 }
 for(int n=0;n<3;++n){Reset();InstallEngine e;assert(e.PrepareSecureUxRemoval());
  if(n==0)e.executionPlanReady_=false;
  if(n==1)e.executionPlan_.valid=false;
  if(n==2)e.executionPlan_.rows[GlassProductPlan::Index(GlassProductPlan::Product::SecureUxTheme)].action=GlassProductPlan::Action::Keep;
  assert(!e.RemoveOriginalSecureUxTheme());assert(GlassInstallMsi::uninstalls==0);
 }
 for(unsigned mask=0;mask<128;++mask){
  GlassProducts::Selection choice;choice.secureUxThemeRemove=true;
  choice.platform64=mask&1;choice.platform32=mask&2;choice.glassSettings=mask&4;
  choice.openGlass=mask&8;choice.settings64=mask&16;choice.settings32=mask&32;choice.secureUxTheme=mask&64;
  assert(GlassProducts::OnlySecureUxThemeRemoval(choice)==(mask==0));
 }
 {Reset();InstallEngine e;assert(e.PrepareSecureUxRemoval());assert(e.RemoveOriginalSecureUxTheme());assert(GlassInstallMsi::uninstalls==1&&e.failures==0);}
 for(int n:{0,5,6}){
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
  assert(e.failures==int(code==1603));assert(e.warnings==int(code==1641));
 }
 std::cout<<"PASS: actual vendor install/removal worker: plan authority, source identity, presence changes, explicit removal, unloaded/loaded style gates, partial failure, MSI failure/reboot; last-transaction order\n";
}
'''.replace('// METHODS',methods)
compiler=shutil.which('clang++') or shutil.which('g++')
if not compiler:raise SystemExit('clang++ or g++ required')
with tempfile.TemporaryDirectory(prefix='GlassSutRemoveWorker-') as directory:
    path=Path(directory)/'worker.cpp';exe=Path(directory)/'worker'
    path.write_text(harness)
    subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror','-pedantic','-I'+str(root/'GlassSetup/src'),str(path),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
