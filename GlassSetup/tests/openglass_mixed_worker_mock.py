#!/usr/bin/env python3
"""Compile actual mixed-plan dispatcher; Windows operations are mocked."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallEngine.cpp').read_text()
wrapper=s[s.index('DWORD WINAPI InstallEngine::WorkerThread('):s.index('DWORD WINAPI InstallEngine::RunGlassWorker(')]
body=s[s.index('DWORD WINAPI InstallEngine::RunGlassWorker('):s.index('bool InstallEngine::Start(')]
assert 'finished_.store(' not in body and 'tempDir_.clear()' not in body
assert 'if(!self->mixedOpenGlass_)self->SetStatus' in body
assert body.index('self->InstallOriginalSecureUxTheme()') < body.index('Launch: commit point reached')
assert 'if(!vendorOnly && !self->auxiliaryOnly_ && !self->mixedOpenGlass_)self->LaunchSelectedProducts()' in body
assert wrapper.index('PrepareOfficialOpenGlass(mixedRemoval)')<wrapper.index('selection_.openGlass=false')<wrapper.index('RunGlassWorker(p)')<wrapper.index('CommitOfficialOpenGlass(mixedRemoval)')<wrapper.index('RemoveDirectoryTree(')<wrapper.index('finished_.store(')
h=r'''
#include "GlassSetup/src/GlassProducts.h"
#include <atomic>
#include <string>
#include <cassert>
using LPVOID=void*;using DWORD=unsigned;
#define WINAPI
#define PRIMARYLANGID(x) (x)
constexpr int LANG_RUSSIAN=25,ERROR_INSTALL_FAILURE=1603;
int GetUserDefaultUILanguage(){return 0;}
namespace GlassInstallUtil{bool IsOs64Bit(){return true;}}
namespace GlassInstallRollback {struct SessionState{std::wstring workspaceDir=L"snapshot";};void CleanupSnapshot(SessionState& s){s.workspaceDir.clear();}}
namespace InstallLog{template<class... Args>void LineFormat(const wchar_t*,Args...) {}}
bool prep=true,glass=true,vendor=true,resourcesPrep=true,resourcesCommit=true;bool expectedRemoval=false;unsigned preparations=0,glassRuns=0,vendorRuns=0,standalone=0,cleanups=0;std::string order;
bool RemoveDirectoryTree(const std::wstring&){++cleanups;order+="cleanup;";return true;}
struct InstallEngine{
 GlassInstallRollback::SessionState rollbackSession_;bool preserveSnapshot_=false;
 GlassProducts::Selection selection_;bool mixedOpenGlass_=false,forceClean_=false,succeeded_=false;
 struct {std::wstring finalize;}labels_;
 bool PrepareFullRemovalCleanup(){if(selection_.cleanupUserData)order+="cleanup-preflight;";return true;}
 void FinishFullRemovalCleanup(){if(selection_.cleanupUserData){assert(vendorRuns==1);order+="data-cleanup;";}}
 void LaunchSelectedProducts(){assert(vendorRuns==1);assert(!selection_.openGlass);}
 void SetStatus(const std::wstring&,int){}
 bool FinishOfficialResources(bool commit){order+=commit?"res-finalize;":"res-rollback;";return true;}
 bool OfficialResourcesPhase(bool preparing){order+=preparing?"res-prepare;":"res-commit;";return preparing?resourcesPrep:resourcesCommit;}
 std::wstring tempDir_=L"protected",error;unsigned procedureMask_=15;
 std::atomic_bool finished_{false},launchingPhase_{false},rollbackSucceeded_{true};
 bool PrepareOfficialOpenGlass(bool r){assert(r==expectedRemoval&&!finished_);++preparations;order+="prepare;";return prep;}
 static DWORD RunGlassWorker(LPVOID p){auto* self=static_cast<InstallEngine*>(p);++glassRuns;order+="glass;";
 assert(!self->finished_);if(self->selection_.Removes(6))assert(self->procedureMask_&64);if(self->mixedOpenGlass_){assert(!self->selection_.openGlass&&(self->procedureMask_&8)==0&&(self->selection_.removeMask&8)==0);}
 if(glass&&(self->selection_.secureUxTheme||self->selection_.secureUxThemeRemove))order+="sut;";
 self->succeeded_=glass;if(glass)self->launchingPhase_=true;return 0;}
 bool CommitOfficialOpenGlass(bool r){assert(r==expectedRemoval&&!finished_&&succeeded_);assert(r?selection_.Removes(3):selection_.openGlass);++vendorRuns;order+="vendor;";return vendor;}
 bool RunOfficialOpenGlassOnly(){++standalone;return vendor;}
 std::wstring ErrorText(){return error;}void SetFailure(const std::wstring& e,int){error=e;}
 static DWORD WINAPI WorkerThread(LPVOID p) noexcept;
};
// WRAPPER
void reset(){expectedRemoval=false;prep=glass=vendor=resourcesPrep=resourcesCommit=true;preparations=glassRuns=vendorRuns=standalone=cleanups=0;order.clear();}
int main(){
 reset();InstallEngine ok;ok.selection_.platform32=false;ok.selection_.glassSettings=false;
 InstallEngine::WorkerThread(&ok);assert(ok.succeeded_&&ok.finished_&&!ok.launchingPhase_&&ok.selection_.openGlass&&order=="prepare;glass;vendor;cleanup;");
 reset();prep=false;InstallEngine failedPrep;InstallEngine::WorkerThread(&failedPrep);assert(!failedPrep.succeeded_&&glassRuns==0&&vendorRuns==0&&failedPrep.finished_);
 reset();glass=false;InstallEngine failedGlass;InstallEngine::WorkerThread(&failedGlass);assert(!failedGlass.succeeded_&&vendorRuns==0&&failedGlass.selection_.openGlass&&failedGlass.finished_);
 reset();vendor=false;InstallEngine partial;InstallEngine::WorkerThread(&partial);assert(partial.preserveSnapshot_&&!partial.rollbackSession_.workspaceDir.empty());assert(!partial.succeeded_&&!partial.rollbackSucceeded_&&!partial.error.empty()&&glassRuns==1&&vendorRuns==1&&partial.finished_);
 reset();expectedRemoval=true;InstallEngine remove;remove.selection_.openGlass=false;remove.selection_.removeMask=8;
 InstallEngine::WorkerThread(&remove);assert(remove.succeeded_&&remove.selection_.removeMask==8&&!remove.selection_.openGlass&&vendorRuns==1&&order=="prepare;glass;vendor;cleanup;");
 reset();expectedRemoval=true;glass=false;InstallEngine removeFail;removeFail.selection_.openGlass=false;removeFail.selection_.removeMask=8;
 InstallEngine::WorkerThread(&removeFail);assert(!removeFail.succeeded_&&vendorRuns==0&&removeFail.selection_.removeMask==8);
 reset();expectedRemoval=true;vendor=false;InstallEngine removePartial;removePartial.selection_.openGlass=false;removePartial.selection_.removeMask=8;
 InstallEngine::WorkerThread(&removePartial);assert(!removePartial.succeeded_&&!removePartial.rollbackSucceeded_&&vendorRuns==1&&!removePartial.error.empty());

 for(unsigned failure=0;failure<5;++failure){
  reset();resourcesPrep=failure!=1;glass=failure!=2;vendor=failure!=3;resourcesCommit=failure!=4;
  InstallEngine combined;combined.selection_.openGlassResources=true;
  InstallEngine::WorkerThread(&combined);
  assert(combined.succeeded_==(failure==0));
  if(failure==0)assert(order=="prepare;res-prepare;glass;res-commit;vendor;res-finalize;cleanup;");
  if(failure==1)assert(glassRuns==0&&vendorRuns==0);
  if(failure==2)assert(vendorRuns==0&&order.find("res-commit;")==std::string::npos);
  if(failure==3)assert(order.find("res-commit;")!=std::string::npos&&order.find("res-rollback;")!=std::string::npos&&!combined.rollbackSucceeded_);
  if(failure==4)assert(vendorRuns==0&&!combined.rollbackSucceeded_&&!combined.error.empty());
  assert(combined.selection_.openGlassResources&&combined.selection_.openGlass);
 }
 reset();InstallEngine pair;pair.selection_.platform64=pair.selection_.platform32=pair.selection_.glassSettings=false;pair.selection_.openGlassResources=true;
 InstallEngine::WorkerThread(&pair);assert(pair.succeeded_&&glassRuns==0&&order=="prepare;res-prepare;res-commit;vendor;res-finalize;cleanup;");
 // Removal subsets route through the managed worker, then official OG last.
 for(unsigned mask=1;mask<128;++mask){
  if(mask&~GlassProducts::Selection::kManagedRemovalMask)continue;
  reset();expectedRemoval=bool(mask&8);InstallEngine e;
  e.selection_.platform64=e.selection_.platform32=e.selection_.glassSettings=e.selection_.openGlass=false;
  e.selection_.removeMask=mask;e.procedureMask_=mask;
  InstallEngine::WorkerThread(&e);
  assert(e.succeeded_&&e.selection_.removeMask==mask);
  if((mask&8)&&(mask&~8u))assert(glassRuns==1&&vendorRuns==1&&order=="prepare;glass;vendor;cleanup;");
  if(!(mask&8))assert(vendorRuns==0&&glassRuns==1); // Resources-only never invokes/removes OG.
 }
 reset();expectedRemoval=true;InstallEngine all;all.selection_.platform64=all.selection_.platform32=all.selection_.glassSettings=all.selection_.openGlass=false;
 all.selection_.removeMask=0x6f;all.procedureMask_=0x6f;all.selection_.secureUxThemeRemove=true;
 InstallEngine::WorkerThread(&all);assert(all.succeeded_&&glassRuns==1&&vendorRuns==1);
 reset();expectedRemoval=true;InstallEngine cleaned;cleaned.selection_.platform64=cleaned.selection_.platform32=cleaned.selection_.glassSettings=cleaned.selection_.openGlass=false;
 cleaned.selection_.removeMask=0x6f;cleaned.procedureMask_=0x6f;cleaned.selection_.cleanupUserData=true;
 InstallEngine::WorkerThread(&cleaned);assert(cleaned.succeeded_&&order=="prepare;cleanup-preflight;glass;vendor;data-cleanup;cleanup;");
 reset();expectedRemoval=true;glass=false;InstallEngine failRemove;failRemove.selection_.platform64=failRemove.selection_.platform32=failRemove.selection_.glassSettings=failRemove.selection_.openGlass=false;
 failRemove.selection_.removeMask=0x48;failRemove.procedureMask_=0x48;InstallEngine::WorkerThread(&failRemove);assert(!failRemove.succeeded_&&vendorRuns==0);
 reset();InstallEngine ordinary;ordinary.selection_.openGlass=false;InstallEngine::WorkerThread(&ordinary);assert(ordinary.succeeded_&&preparations==0&&glassRuns==1&&vendorRuns==0);
 reset();InstallEngine only;only.selection_.platform64=only.selection_.platform32=only.selection_.glassSettings=false;InstallEngine::WorkerThread(&only);assert(only.succeeded_&&standalone==1&&glassRuns==0);
 // SUT alone is sufficient to select the mixed route, with or without Resources.
 for(bool resources:{false,true})for(bool removal:{false,true})for(unsigned failure=0;failure<5;++failure){
  reset();prep=failure!=1;glass=failure!=2;resourcesCommit=failure!=3;vendor=failure!=4;
  InstallEngine e;e.selection_.platform64=e.selection_.platform32=e.selection_.glassSettings=false;
  e.selection_.secureUxTheme=!removal;e.selection_.secureUxThemeRemove=removal;e.selection_.openGlassResources=resources;
  InstallEngine::WorkerThread(&e);
  assert(standalone==0&&e.mixedOpenGlass_);
  assert(e.succeeded_==(failure==0||(!resources&&failure==3)));
  if(failure==1)assert(glassRuns==0&&vendorRuns==0);
  if(failure==2)assert(glassRuns==1&&vendorRuns==0);
  if(failure==4||(resources&&failure==3))assert(!e.rollbackSucceeded_&&e.error.find(L"SecureUxTheme")!=std::wstring::npos);
  if(failure==0)assert(order==(resources?"prepare;res-prepare;glass;sut;res-commit;vendor;res-finalize;cleanup;":"prepare;glass;sut;vendor;cleanup;"));
  assert(e.selection_.secureUxTheme==!removal&&e.selection_.secureUxThemeRemove==removal);
 }
}
'''.replace('// WRAPPER',wrapper)
with tempfile.TemporaryDirectory() as tmp:
    cpp=Path(tmp)/'worker.cpp';exe=Path(tmp)/'worker';cpp.write_text(h)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I',str(root),str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('PASS: actual mixed worker: preflight, managed row exclusion, commit order, partial failure, staged lifetime, deferred completion')
