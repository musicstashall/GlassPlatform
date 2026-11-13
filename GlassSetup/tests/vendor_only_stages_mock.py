"""Actual worker initial stages with fake operations: vendor-only has no Glass mutations."""
from pathlib import Path
import shutil, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'GlassSetup/src/InstallEngine.cpp').read_text()
a=source.index('            auto* self = static_cast<InstallEngine*>(p);',source.index('DWORD WINAPI InstallEngine::WorkerThread'))
initial=source[a:source.index('            // Финальная стадия запуска компонентов',a)]
extract=source[source.index('bool InstallEngine::ExtractPayload('):source.index('bool InstallEngine::RecheckSnapshottedProduct(')]
assert extract.index('EnsureTempWorkspace()')<extract.index('OnlySecureUxThemeRemoval(selection_)')<extract.index('FetchCatalog(')
harness=r'''
#include "GlassProcedurePlan.h"
#include <atomic>
#include <cassert>
#include <iostream>
#include <string>
#include <vector>
std::vector<std::string> calls;std::string failAt;
bool Call(const char* op){calls.emplace_back(op);return failAt!=op;}
#define PRIMARYLANGID(x) (x)
constexpr unsigned LANG_RUSSIAN=25;
unsigned GetUserDefaultUILanguage(){return 0;}
unsigned GetLastError(){return 5;}
namespace InstallLog {void Line(const wchar_t*){}}
namespace GlassInstallUtil {
 bool IsOs64Bit(){return true;}
 bool CloseSelectedSettingsProcesses(const std::wstring&,const std::wstring&,unsigned,const std::atomic_bool*){return Call("close");}
}
struct InstallEngine {
 unsigned procedureMask_=0x2Fu;bool auxiliaryOnly_=false;
 GlassProducts::Selection selection_;
 bool forceClean_=false,cleanInstall_=false,scanNativeX64_=false,installAttempted_=false,nonSettingsChangesAttempted_=false;
 std::wstring x64Dir_=L"64",x86Dir_=L"32";
 void SetFailure(const std::wstring&,unsigned){}
 std::atomic_bool cancelRequested_{false};int instance_=0;
 bool IsCancelRequested()const{return cancelRequested_.load();}
 bool CapturePriorHostState(){return Call("capture");}
 bool DetermineCleanInstall(){return Call("determine");}
 bool ExtractPayload(int){return Call("extract");}
 bool CreateRollbackSnapshot(){return Call("snapshot");}
 bool CommitEarlySecureUxRemoval(){return Call("commit-vendor");}
 bool PrepareSecureUxRemoval(){return Call("prepare-vendor");}
 bool PrepareFullRemovalCleanup(){return true;}
 bool BuildExecutionPlan(){return Call("build");}
 bool PrepareManagedRemoval(){return !selection_.removeMask || Call("prepare-managed");}
 bool StopRunningHosts(){return Call("stop");}
 bool InstallGlassSettingsEarly(){return Call("helpers");}
 bool ReleaseLockedModules(){return Call("scan");}
 bool PurgeRemnants(){return Call("purge");}
 bool RunMsiSequence(){return Call("msi");}
};
bool Initial(InstallEngine* p){
// INITIAL
 (void)snapshotFailed;return ok&&!p->IsCancelRequested();
}
void VendorOnly(InstallEngine& e){
 e.selection_.platform64=e.selection_.platform32=e.selection_.glassSettings=e.selection_.openGlass=false;
 e.selection_.secureUxThemeRemove=true;
}
int main(){
 {calls.clear();InstallEngine e;VendorOnly(e);e.forceClean_=true;assert(Initial(&e));
  assert(calls==std::vector<std::string>({"extract","prepare-vendor","build"}));
  assert(!e.installAttempted_&&!e.nonSettingsChangesAttempted_&&e.scanNativeX64_);}
 for(const auto* fail:{"extract","prepare-vendor","build"}){
  calls.clear();failAt=fail;InstallEngine e;VendorOnly(e);assert(!Initial(&e));
  assert(calls.back()==failAt);assert(!e.installAttempted_);
 }
 failAt.clear();
 {calls.clear();InstallEngine e;VendorOnly(e);e.cancelRequested_=true;assert(!Initial(&e));assert(calls.empty());}
 {calls.clear();InstallEngine e;VendorOnly(e);e.selection_.platform32=true;assert(Initial(&e));
  assert(calls==std::vector<std::string>({"capture","determine","extract","snapshot","prepare-vendor","build","close","stop","helpers","scan","msi"}));
  assert(e.installAttempted_&&e.nonSettingsChangesAttempted_);}
 {calls.clear();failAt="prepare-vendor";InstallEngine e;VendorOnly(e);e.selection_.platform32=true;
  assert(!Initial(&e));assert(calls==std::vector<std::string>({"capture","determine","extract","snapshot","prepare-vendor"}));assert(!e.installAttempted_);}
 {calls.clear();failAt.clear();InstallEngine e;VendorOnly(e);e.selection_.secureUxThemeRemove=false;
  e.selection_.removeMask=8;e.procedureMask_=8;e.auxiliaryOnly_=true;
  assert(Initial(&e));
  assert(calls==std::vector<std::string>({"capture","extract","snapshot","prepare-vendor","build","prepare-managed","stop","helpers","msi"}));
  assert(e.scanNativeX64_);}
 std::cout<<"PASS: actual worker stages: vendor-only skips Glass snapshot/stop/helpers/scan/MSI/purge; mixed path retained; vendor preflight failure before stops; cancellation\n";
}
'''.replace('// INITIAL',initial)
compiler=shutil.which('clang++') or shutil.which('g++')
if not compiler:raise SystemExit('clang++ or g++ required')
with tempfile.TemporaryDirectory(prefix='GlassVendorOnlyStages-') as directory:
    path=Path(directory)/'stages.cpp';exe=Path(directory)/'stages'
    path.write_text(harness)
    subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror','-pedantic','-I'+str(root/'GlassSetup/src'),str(path),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
