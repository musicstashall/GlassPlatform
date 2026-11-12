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
assert wrapper.index('PrepareOfficialOpenGlass(false)')<wrapper.index('selection_.openGlass=false')<wrapper.index('RunGlassWorker(p)')<wrapper.index('CommitOfficialOpenGlass(false)')<wrapper.index('RemoveDirectoryTree(')<wrapper.index('finished_.store(')
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
bool prep=true,glass=true,vendor=true;unsigned preparations=0,glassRuns=0,vendorRuns=0,standalone=0,cleanups=0;std::string order;
bool RemoveDirectoryTree(const std::wstring&){++cleanups;order+="cleanup;";return true;}
struct InstallEngine{
 GlassProducts::Selection selection_;bool mixedOpenGlass_=false,forceClean_=false,succeeded_=false;
 std::wstring tempDir_=L"protected",error;unsigned procedureMask_=15;
 std::atomic_bool finished_{false},launchingPhase_{false},rollbackSucceeded_{true};
 bool PrepareOfficialOpenGlass(bool r){assert(!r&&!finished_);++preparations;order+="prepare;";return prep;}
 static DWORD RunGlassWorker(LPVOID p){auto* self=static_cast<InstallEngine*>(p);++glassRuns;order+="glass;";
 assert(!self->finished_);if(self->mixedOpenGlass_){assert(!self->selection_.openGlass&&(self->procedureMask_&8)==0);}
 self->succeeded_=glass;if(glass)self->launchingPhase_=true;return 0;}
 bool CommitOfficialOpenGlass(bool r){assert(!r&&!finished_&&launchingPhase_&&selection_.openGlass&&succeeded_);++vendorRuns;order+="vendor;";return vendor;}
 bool RunOfficialOpenGlassOnly(){++standalone;return vendor;}
 std::wstring ErrorText(){return error;}void SetFailure(const std::wstring& e,int){error=e;}
 static DWORD WINAPI WorkerThread(LPVOID p) noexcept;
};
// WRAPPER
void reset(){prep=glass=vendor=true;preparations=glassRuns=vendorRuns=standalone=cleanups=0;order.clear();}
int main(){
 reset();InstallEngine ok;ok.selection_.platform32=false;ok.selection_.glassSettings=false;
 InstallEngine::WorkerThread(&ok);assert(ok.succeeded_&&ok.finished_&&!ok.launchingPhase_&&ok.selection_.openGlass&&order=="prepare;glass;vendor;cleanup;");
 reset();prep=false;InstallEngine failedPrep;InstallEngine::WorkerThread(&failedPrep);assert(!failedPrep.succeeded_&&glassRuns==0&&vendorRuns==0&&failedPrep.finished_);
 reset();glass=false;InstallEngine failedGlass;InstallEngine::WorkerThread(&failedGlass);assert(!failedGlass.succeeded_&&vendorRuns==0&&failedGlass.selection_.openGlass&&failedGlass.finished_);
 reset();vendor=false;InstallEngine partial;InstallEngine::WorkerThread(&partial);assert(!partial.succeeded_&&!partial.rollbackSucceeded_&&!partial.error.empty()&&glassRuns==1&&vendorRuns==1&&partial.finished_);
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
