"""Actual post-vendor graphics transaction with isolated MSI/filesystem seams."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallEngine.cpp').read_text()
a=s.index('bool InstallEngine::OfficialResourcesPhase(');b=s.index('DWORD WINAPI InstallEngine::WorkerThread(',a)
method=s[a:b]
h=r'''
#include "GlassSetup/src/GlassProducts.h"
#include "GlassSetup/src/GlassProductPlan.h"
#include <atomic>
#include <cassert>
#include <string>
#include <vector>
#include <functional>
#include <utility>
#include <stdexcept>
#define PRIMARYLANGID(x) (x)
#define _wcsicmp wcscasecmp
constexpr int LANG_RUSSIAN=25,ERROR_INSTALL_FAILURE=1603,ERROR_INSTALL_PACKAGE_REJECTED=1625;
int GetUserDefaultUILanguage(){return 0;}int GetLastError(){return 5;}
bool protocolOk=true,rootOk=true,prepareOk=true,snapshotOk=true,planOk=true,installOk=true,rollbackOk=true,present=false,wasPresent=false,throwInstall=false;
unsigned prepares=0,snapshots=0,installs=0,rollbacks=0,publishes=0;bool upstream=false;
namespace InstallLog {void Line(const wchar_t*){}}
namespace GlassInstallUtil {bool ResolveOpenGlassResourcesRoot(std::wstring& r){r=L"fixed-resources";return rootOk;}}
namespace GlassPackageCatalog {enum class Product{OpenGlassResources};struct Catalog{};}
namespace GlassPackageAcquisition {
bool FetchCatalog(const std::atomic_bool&,GlassPackageCatalog::Catalog&,std::wstring&){return true;}
bool Prepare(GlassPackageCatalog::Product,const GlassPackageCatalog::Catalog&,const std::wstring&,const std::atomic_bool&,std::wstring&){++prepares;return prepareOk;}
}
namespace GlassInstallMsi {
constexpr auto kProductOpenGlassResourcesX64=L"resources";
bool SupportsResourcesBeforeOpenGlass(const std::wstring& path){assert(path==L"staged\\OpenGlassResources-x64.msi");return protocolOk;}
bool IsProductInstalled(const wchar_t* code){assert(std::wstring(code)==L"resources");return present;}
}
namespace GlassInstallRollback {
struct SessionState {std::wstring workspaceDir;unsigned copyError=5;std::vector<std::wstring> oldProducts,modifiedProducts,installedThisSession;};
bool CreateSnapshot(SessionState& state,const std::wstring&,const std::wstring&,const std::wstring&,bool include,
 std::function<void(int)>,const std::atomic_bool*,unsigned mask){assert(!include&&mask==64);++snapshots;state.workspaceDir=L"resources-backup";if(wasPresent)state.oldProducts={L"resources"};return snapshotOk;}
void NoteInstalled(SessionState& state,const wchar_t* code){state.installedThisSession.push_back(code);}
bool ExecuteRollback(SessionState& state,std::function<void(int)>){++rollbacks;assert(state.workspaceDir==L"resources-backup");return rollbackOk;}
void CleanupSnapshot(SessionState& state){state.workspaceDir.clear();}
}
struct InstallEngine {
 GlassProducts::Selection selection_;
 GlassProductPlan::Plan executionPlan_;
 GlassInstallRollback::SessionState rollbackSession_,officialResourcesRollback_;
 bool executionPlanReady_=false,scanNativeX64_=false,preparingOfficialResources_=false,preserveOfficialResourcesSnapshot_=false;
 unsigned procedureMask_=7;std::atomic_bool cancelRequested_{false};
 std::wstring x64Dir_,x86Dir_,openGlassDir_,tempDir_=L"staged",error;
 bool EnsureTempWorkspace(){return true;}
 void SetFailure(const std::wstring& e,int){error=e;}
 std::wstring ErrorText(){return error;}
 bool BuildExecutionPlan(){assert(selection_.openGlassResources&&!selection_.openGlass&&!selection_.platform64&&!selection_.platform32&&!selection_.glassSettings);
  assert(scanNativeX64_&&procedureMask_==64&&rollbackSession_.workspaceDir==L"resources-backup");
  executionPlanReady_=true;return planOk&&(preparingOfficialResources_||upstream);}
 bool RunOneMsi(const std::wstring& path,const std::wstring& root,const std::wstring&,int,int){
  assert(path==L"staged\\OpenGlassResources-x64.msi"&&root==L"fixed-resources"&&preparingOfficialResources_);
  ++installs;if(throwInstall)throw std::runtime_error("allocation seam");
  if(wasPresent){rollbackSession_.modifiedProducts={L"resources"};}present=true;error=L"MSI failed";return installOk;}
 void LaunchSelectedProducts(){++publishes;assert(selection_.openGlassResources&&!selection_.openGlass);}
 bool OfficialResourcesPhase(bool preparing) noexcept;
 bool FinishOfficialResources(bool commit) noexcept;
};
// METHOD
void reset(){protocolOk=true;rootOk=prepareOk=snapshotOk=planOk=installOk=rollbackOk=true;present=wasPresent=throwInstall=upstream=false;prepares=snapshots=installs=rollbacks=publishes=0;}
int main(){
 reset();protocolOk=false;InstallEngine outdated;assert(!outdated.OfficialResourcesPhase(true));assert(snapshots==0&&installs==0&&rollbacks==0&&publishes==0);assert(outdated.error.find(L"outdated")!=std::wstring::npos);
 for(unsigned failure=0;failure<5;++failure){
  reset();rootOk=failure!=1;prepareOk=failure!=2;snapshotOk=failure!=3;planOk=failure!=4;
  InstallEngine e;e.selection_.openGlassResources=true;e.rollbackSession_.workspaceDir=L"managed";
  assert(e.OfficialResourcesPhase(true)==(failure==0));assert(installs==0&&rollbacks==0&&publishes==0);
  assert(e.selection_.openGlass&&e.selection_.openGlassResources&&e.selection_.platform64);
  assert(e.rollbackSession_.workspaceDir==L"managed"&&!e.preparingOfficialResources_&&!e.executionPlanReady_&&e.procedureMask_==7);
 }
 for(bool old:{false,true})for(unsigned failure=0;failure<4;++failure){
  reset();wasPresent=old;InstallEngine e;e.selection_.openGlassResources=true;
  e.rollbackSession_.workspaceDir=L"managed";assert(e.OfficialResourcesPhase(true));
  upstream=true;installOk=failure==0;rollbackOk=failure!=2;throwInstall=failure==3;
  assert(e.OfficialResourcesPhase(false)==(failure==0));
  assert(e.rollbackSession_.workspaceDir==L"managed"&&e.selection_.openGlass&&e.selection_.platform64&&e.selection_.openGlassResources);
  assert(!e.preparingOfficialResources_&&!e.executionPlanReady_&&e.procedureMask_==7);
  if(failure==0){assert(publishes==0&&rollbacks==0&&!e.officialResourcesRollback_.workspaceDir.empty());assert(e.FinishOfficialResources(true));assert(publishes==1&&e.officialResourcesRollback_.workspaceDir.empty());}
  if(failure==1||failure==2){assert(rollbacks==1&&publishes==0);assert(e.officialResourcesRollback_.installedThisSession==std::vector<std::wstring>{L"resources"});}
  assert(e.preserveOfficialResourcesSnapshot_==(failure==2||failure==3));
 }
 reset();InstallEngine changed;assert(changed.OfficialResourcesPhase(true));assert(changed.OfficialResourcesPhase(false));assert(installs==1);assert(changed.FinishOfficialResources(false));assert(rollbacks==1&&publishes==0); // Vendor failure restores the pre-OG graphics baseline.
}
'''.replace('// METHOD',method)
with tempfile.TemporaryDirectory(prefix='ResourcesPhase-') as d:
 p=Path(d)/'test.cpp';exe=Path(d)/'test';p.write_text(h)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-I',str(root),str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: actual Resources phase: prepare-before-mutation, independent baseline, pre-OG graphics install, fresh/update MSI failure rollback, recovery retention and scoped state restoration')

# Pending authorization is private to the staged graphics product, and early
# managed cache publication excludes the still-pending Resources baseline.
assert 'props+=L" GLASS_OG_INSTALL_PENDING=1"' in s
assert '_wcsicmp(productCode.c_str(),GlassInstallMsi::kProductOpenGlassResourcesX64)==0' in s
assert '!(mixedOpenGlass_&&!selection_.openGlassResources&&!officialResourcesRollback_.workspaceDir.empty()&&sourceRows[i]==6)' in s
ui=(root/'GlassSetup/src/main.cpp').read_text()
assert 'inspect_.og.installedPayloadVersion:L"",inspect_.og.remotePayloadVersion)' in ui
