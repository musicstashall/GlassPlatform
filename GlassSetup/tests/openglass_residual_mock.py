"""Run actual residual inspection and UI classification against read-only API seams.
Not a Windows SCM/Scheduler/MSI or signature/ownership integration test.
"""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'GlassSetup/src/InstallSupport/GlassProductInspect.cpp').read_text()
start=source.index('    if (in.os64 && !report.og.installed)')
end=source.index('    if (!report.x64.installed &&',start)
probe=source[start:end]
main=(root/'GlassSetup/src/main.cpp').read_text()
condition=main[main.index('    static GlassProducts::ProductCondition ConditionOf('):main.index('    static UINT InspectReasonId(')]
harness=r'''
#include "GlassProducts.h"
#include <atomic>
#include <cassert>
#include <string>
#include <iostream>
using DWORD=unsigned long;
constexpr DWORD SERVICE_AUTO_START=2,ERROR_SUCCESS=0;
DWORD GetLastError(){return 5;}
struct State {bool service=false,tasks=false,serviceKnown=true,tasksKnown=true,rootsSafe=true,pathsSafe=true;
 int queries=0,validations=0;} s;
namespace GlassProductInspect {
struct ProductReport {bool installed=false,residual=false,residualSafe=false,damaged=false,updateAvailable=false;};
struct Report {ProductReport og;};
struct Inputs {bool os64=true;const std::atomic_bool* cancelled=nullptr;};
}
namespace GlassInstallHost {
bool QueryOpenGlassServiceStartType(bool& exists,DWORD&){++s.queries;exists=s.service;return s.serviceKnown;}
}
namespace GlassInstallUtil {
struct OpenGlassTaskState {bool found=false;};
bool QueryOpenGlassTasks(OpenGlassTaskState& out){++s.queries;out.found=s.tasks;return s.tasksKnown;}
struct InstallRoots {std::wstring openGlass=L"expected-root";};
bool ResolveInstallRoots(InstallRoots&){return s.rootsSafe;}
}
namespace GlassInstallRemoval {
bool ValidateOpenGlassPaths(const std::wstring& root,const std::atomic_bool* cancelled,bool,std::wstring&){
 ++s.validations;assert(root==L"expected-root");return s.pathsSafe&&!(cancelled&&cancelled->load());}
}
namespace GlassInstallLog {template<class... T>void LineFormat(const wchar_t*,T...){} }
GlassProductInspect::Report Inspect(bool registered=false,bool os64=true,const std::atomic_bool* cancel=nullptr){
 GlassProductInspect::Inputs in{os64,cancel};GlassProductInspect::Report report;report.og.installed=registered;
// PROBE
 return report;
}
// CONDITION
int main(){
 using C=GlassProducts::ProductCondition;
 {s={};auto r=Inspect();assert(!r.og.installed&&!r.og.residual&&!r.og.damaged);assert(s.validations==0);}
 for(bool taskOnly:{false,true}){s={};s.service=!taskOnly;s.tasks=taskOnly;auto r=Inspect();
  assert(!r.og.installed&&r.og.residual&&r.og.damaged&&r.og.residualSafe);
  assert(ConditionOf(&r.og,false)==C::Damaged);assert(s.validations==1);}
 {s={};s.service=true;s.pathsSafe=false;auto r=Inspect();assert(r.og.residual&&!r.og.residualSafe);}
 {s={};s.service=true;s.rootsSafe=false;auto r=Inspect();assert(r.og.residual&&!r.og.residualSafe);assert(s.validations==0);}
 {s={};s.service=true;s.serviceKnown=false;auto r=Inspect();assert(r.og.residual&&!r.og.residualSafe);assert(s.validations==0);}
 {s={};s.service=true;s.tasksKnown=false;auto r=Inspect();assert(r.og.residual&&!r.og.residualSafe);}
 {s={};s.service=true;auto r=Inspect(true);assert(r.og.installed&&!r.og.residual);assert(s.queries==0);}
 {s={};s.service=true;auto r=Inspect(false,false);assert(!r.og.residual);assert(s.queries==0);}
 {s={};s.service=true;std::atomic_bool cancel{true};auto r=Inspect(false,true,&cancel);assert(r.og.residual&&!r.og.residualSafe);}
 std::cout<<"PASS: actual OpenGlass residual probe/classification: service/tasks, MSI distinction, absent, foreign path/root, query failure, cancellation\n";
}
'''.replace('// PROBE',probe).replace('// CONDITION',condition)
with tempfile.TemporaryDirectory(prefix='OpenGlassResidual-') as d:
 p=Path(d)/'test.cpp';exe=Path(d)/'test';p.write_text(harness)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(root/'GlassSetup/src'),str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
