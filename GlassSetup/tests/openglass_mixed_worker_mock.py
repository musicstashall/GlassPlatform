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
namespace InstallLog{template<class... Args>void LineFormat(const wchar_t*,Args...) {}}
bool prep=true,glass=true,vendor=true,resourcesPrep=true,resourcesCommit=true;bool expectedRemoval=false;unsigned preparations=0,glassRuns=0,vendorRuns=0,standalone=0,cleanups=0;std::string order;
bool RemoveDirectoryTree(const std::wstring&){++cleanups;order+="cleanup;";return true;}
struct InstallEngine{
 GlassProducts::Selection selection_;bool mixedOpenGlass_=false,forceClean_=false,succeeded_=false;
 struct {std::wstring finalize;}labels_;
 void SetStatus(const std::wstring&,int){}
 bool FinishOfficialResources(bool commit){order+=commit?"res-finalize;":"res-rollback;";return true;}
 bool OfficialResourcesPhase(bool preparing){order+=preparing?"res-prepare;":"res-commit;";return preparing?resourcesPrep:resourcesCommit;}
 std::wstring tempDir_=L"protected",error;unsigned procedureMask_=15;
 std::atomic_bool finished_{false},launchingPhase_{false},rollbackSucceeded_{true};
 bool PrepareOfficialOpenGlass(bool r){assert(r==expectedRemoval&&!finished_);++preparations;order+="prepare;";return prep;}
 static DWORD RunGlassWorker(LPVOID p){auto* self=static_cast<InstallEngine*>(p);++glassRuns;order+="glass;";
 assert(!self->finished_);if(self->mixedOpenGlass_){assert(!self->selection_.openGlass&&(self->procedureMask_&8)==0&&self->selection_.removeMask==0);}
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
 reset();vendor=false;InstallEngine partial;InstallEngine::WorkerThread(&partial);assert(!partial.succeeded_&&!partial.rollbackSucceeded_&&!partial.error.empty()&&glassRuns==1&&vendorRuns==1&&partial.finished_);
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
 reset();InstallEngine ordinary;ordinary.selection_.openGlass=false;InstallEngine::WorkerThread(&ordinary);assert(ordinary.succeeded_&&preparations==0&&glassRuns==1&&vendorRuns==0);
 reset();InstallEngine only;only.selection_.platform64=only.selection_.platform32=only.selection_.glassSettings=false;InstallEngine::WorkerThread(&only);assert(only.succeeded_&&standalone==1&&glassRuns==0);
 reset();InstallEngine unsupported;unsupported.selection_.secureUxTheme=true;InstallEngine::WorkerThread(&unsupported);assert(standalone==1&&glassRuns==0&&preparations==0);
}
'''.replace('// WRAPPER',wrapper)
with tempfile.TemporaryDirectory() as tmp:
    cpp=Path(tmp)/'worker.cpp';exe=Path(tmp)/'worker';cpp.write_text(h)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I',str(root),str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('PASS: actual mixed worker: preflight, managed row exclusion, commit order, partial failure, staged lifetime, deferred completion')
