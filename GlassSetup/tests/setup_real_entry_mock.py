"""Actual production StartInstallation/Tick, fake engine + read-only Win32 APIs.
No real Setup, MSI, registry, network, platform or worker thread is launched.
"""
from pathlib import Path
import re, shutil, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/main.cpp').read_text()
def fn(text,token):
 a=text.index(token);b=text.index('{',a);depth=1;c=b+1
 while depth:
  depth+=(text[c]=='{')-(text[c]=='}');c+=1
 return text[a:c]
start=fn(s,'void StartInstallation()')
tick=fn(s,'void Tick()')
h=(root/'GlassSetup/src/InstallEngine.h').read_text()
labels=fn(h,'struct PhaseLabels')+';'
for obsolete in ('GLASS_EMBED_MSI',):
 assert obsolete not in s,obsolete
assert 'installEngine_.Start(' in start and 'RequestCancel()' in s
assert 'HasCommandLineSwitch(L"--demo-tour")' in s
assert 'RemoveOrphanUninstallEntry' not in fn(s,'void StartupWorker(')
finish=fn(s,'void FinishDebugDemo()')
for reset in ('webInitSent_=false','certQueried_=false','certSigner_={}','selection_.trustSigner=false','finishWarnings_.clear()'):
 assert reset in finish,reset
harness=r'''
#include "GlassProducts.h"
#include "WizardProgress.h"
#include "../res/resource.h"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <cassert>
#include <iostream>
#include <string>
#include <vector>
using ULONGLONG=unsigned long long;constexpr int FALSE=0,kInstallTimer=1,kInstallTimerMs=16,kRollbackFinishMs=900;
constexpr double kProgressSmoothTauMs=180;
constexpr ULONGLONG kDuration=30000,kSimulatedRollbackDuration=5000;
int gInstance=1,timers=0;bool timerAvailable=true;ULONGLONG now=100;
ULONGLONG GetTickCount64(){return now;}unsigned GetLastError(){return 5;}
void InvalidateRect(int,void*,int){}int SetTimer(int,int,int,void*){timers=timerAvailable?1:0;return timers;}void KillTimer(int,int){timers=0;}
namespace InstallLog {void Line(const wchar_t*){}template<class... A>void LineFormat(const wchar_t*,A...) {}}
namespace GlassInstallUtil {bool IsOs64Bit(){return true;}
struct InstallRoots {std::wstring x64=L"x64",x86=L"x86",openGlass=L"og";};
bool rootsOk=true;InstallRoots observation;
bool ResolveInstallRoots(InstallRoots& out){out=observation;return rootsOk;}
bool SameInstallPath(const std::wstring& a,const std::wstring& b){return a==b;}
}
struct InstallEngine {
// LABELS
struct FinishWarning {std::wstring title,detail;};
enum class HoldKind {None,Modules,MissingHelper,Processes};
int starts=0;bool startOk=true,finished=false,success=false,cancelled=false,rollbackOk=true,launchWork=true,launchPhase=false;
GlassProducts::Selection captured;std::wstring capturedCode;
bool Start(int,const std::wstring& x64,const std::wstring& x86,const std::wstring& og,
 GlassProducts::Selection selection,GlassProducts::OpenGlassStartupState,bool clean,PhaseLabels labels,void*,const std::wstring& code){
 assert(x64==L"x64"&&x86==L"x86"&&og==L"og"&&!clean&&!labels.extract.empty());
 ++starts;captured=selection;capturedCode=code;return startOk;
}
bool TakeErrorPageRequest(){return false;}void NotifyErrorPageShown(){}
bool Poll(){return finished;}bool WasCancelled(){return cancelled;}bool Succeeded(){return success;}
bool RollbackSucceeded(){return rollbackOk;}std::wstring RollbackNote(){return L"restored";}
bool HasHostLaunchWork(){return launchWork;}bool IsLaunchingPhase(){return launchPhase;}HoldKind CurrentHold(){return HoldKind::None;}
std::wstring HoldReport(){return {};}
int ProgressUnits(){return 5000;}bool IsProgressReverse(){return cancelled;}bool IsMarquee(){return false;}
std::wstring StatusLine(){return L"actual worker";}std::vector<FinishWarning> FinishWarnings(){return {};}
};
enum class Page {Welcome,Components,Certificate,Installing,Error,ConfirmCancel,Cancelling,Cancelled,Launching,Complete,ModulesBusy,ProcessesStuck};
struct Wizard {
 std::mutex inspectMutex_;
 bool SecureUxThemeRemovalSupportedLocked(){return true;}
 bool SecureUxRemovalDependencySatisfiedLocked(){return true;}
 bool NoSettingsInstalledLocked(){return false;}

 Page page_=Page::Components;
 bool simulateInstall_=false,simulateRollback_=false;ULONGLONG started_=0,simulateRollbackStarted_=0;
 bool accepted_=true,transitioning_=false,debugDemoActive_=false,debugDemoPending_=false;
 std::atomic_bool inspectReady_{true};bool preflight=true,certificate=false;
 int window=1,progress_=2,percent_=0,progressValue_=0,progressTarget_=0,displayProgress_=0,rollbackFinishFrom_=0;
 bool progressReverse_=false,progressMarquee_=false,rollbackFinishing_=false;
 ULONGLONG rollbackFinishStarted_=0,progressTickMs_=0,launchStarted_=0,lastWebPush_=0;
 WizardProgress::VisibleProgress visibleProgress_;WizardProgress::RollbackTracker rollbackProgress_;
 std::wstring installStatusLine_,holdReport_,cancelledDetail_;
 std::wstring preflightX64Root_=L"x64",preflightX86Root_=L"x86",preflightOpenGlassRoot_=L"og",openGlassStartupDir_=L"og",secureUxRemovalCode_=L"vendor";
 GlassProducts::Selection selection_;
 GlassProducts::OpenGlassStartupState openGlassStartup_{true,false,false};
 InstallEngine installEngine_;std::vector<InstallEngine::FinishWarning> finishWarnings_;
 int errors=0;
 std::wstring Text(unsigned id){return std::to_wstring(id);}
 bool ConfirmManagedSettingsRemoval(){return true;}
 bool RunPreflightChecks(){return preflight;}bool CertificatePageApplies(){return certificate;}
 void SetPage(Page p){page_=p;}void ShowInstallError(const std::wstring&){++errors;KillTimer(window,kInstallTimer);page_=Page::Error;}
 void ShowFailurePage(bool){++errors;KillTimer(window,kInstallTimer);page_=Page::Error;}
 void BeginRollbackVisual(ULONGLONG){progressReverse_=true;}
 void ShowCancelling(){page_=Page::Cancelling;}
 void SyncWebViewState(){}void InvalidateFooter(){}
// METHODS
};
int main(){
#ifdef _DEBUG
 static_assert(Wizard::SimulationBuild());
#else
 static_assert(!Wizard::SimulationBuild());
#endif
 for(int n=0;n<14;++n){Wizard w;timers=0;timerAvailable=true;GlassInstallUtil::rootsOk=true;GlassInstallUtil::observation={};
  if(n==1)w.accepted_=false;
  if(n==2)w.transitioning_=true;
  if(n==3)w.inspectReady_=false;
  if(n==4)w.preflight=false;
  if(n==5)GlassInstallUtil::rootsOk=false;
  if(n==6)GlassInstallUtil::observation.x64=L"changed";
  if(n==7)w.openGlassStartup_.valid=false;
  if(n==8)w.installEngine_.startOk=false;
  if(n==9)w.page_=Page::Welcome;
  if(n==10){w.selection_.platform64=w.selection_.platform32=w.selection_.glassSettings=w.selection_.openGlass=false;}
  if(n==11)w.page_=Page::Certificate;
  if(n==12)timerAvailable=false;
  if(n==13)w.certificate=true;
  w.selection_.trustSigner=true;w.StartInstallation();
#ifdef _DEBUG
  assert(w.installEngine_.starts==0);
  const bool simulated=n==0||n==5||n==6||n==7||n==8||n==11||n==13;
  assert(timers==(simulated?1:0));assert(w.simulateInstall_==simulated);
  if(n==12)assert(w.page_==Page::Error&&w.errors==1);
#else
  assert(w.installEngine_.starts==((n==0||n==8||n==11||n==13)?1:0));
  assert(timers==((n==0||n==11||n==13)?1:0));
  if(n==8||n==12)assert(w.page_==Page::Error&&w.errors==1);
  if(n==0||n==11||n==13){assert(w.page_==Page::Installing);assert(w.installEngine_.captured.trustSigner==(n==13));}
#endif
 }
#ifdef _DEBUG
 {Wizard w;w.debugDemoActive_=true;w.StartInstallation();assert(w.installEngine_.starts==0);}
 {Wizard w;w.page_=Page::Certificate;w.debugDemoPending_=true;w.StartInstallation();assert(w.installEngine_.starts==0);}
#endif
#ifndef _DEBUG
 {Wizard w;w.selection_.secureUxThemeRemove=true;w.StartInstallation();assert(w.installEngine_.captured.secureUxThemeRemove&&w.installEngine_.capturedCode==L"vendor");}
 // Passing time cannot complete installation without an engine success result.
 {Wizard w;w.page_=Page::Installing;now=60000;w.Tick();assert(w.page_==Page::Installing);
  w.installEngine_.finished=true;w.Tick();assert(w.page_==Page::Error);}
 {Wizard w;w.page_=Page::Installing;w.installEngine_.finished=w.installEngine_.success=true;
  now=100;w.Tick();assert(w.page_==Page::Launching);now=5101;w.Tick();assert(w.page_==Page::Complete);}
 {Wizard w;w.page_=Page::Installing;w.installEngine_.finished=w.installEngine_.cancelled=true;
  w.installEngine_.rollbackOk=false;w.Tick();assert(w.page_==Page::Error);}
#else
 {Wizard w;timerAvailable=true;now=100;w.StartInstallation();assert(w.simulateInstall_&&w.installEngine_.starts==0);
  now=30100;w.Tick();assert(w.page_==Page::Launching);now=35101;w.Tick();assert(w.page_==Page::Complete&&w.installEngine_.starts==0);}
 {Wizard w;now=100;w.StartInstallation();w.page_=Page::ConfirmCancel;w.progressValue_=4300;
  now=20000;w.Tick();assert(w.progressValue_==4300&&w.page_==Page::ConfirmCancel);
  w.simulateRollback_=true;w.simulateRollbackStarted_=now;w.page_=Page::Cancelling;
  now+=5001;w.Tick();assert(w.page_==Page::Cancelled&&!w.simulateInstall_&&w.installEngine_.starts==0);}
#endif
#ifndef _DEBUG
 {Wizard w;now=100;w.StartInstallation();w.installEngine_.launchWork=false;w.installEngine_.launchPhase=true;
  now=150;w.Tick();assert(w.page_==Page::Installing&&w.launchStarted_==0);
  w.installEngine_.finished=w.installEngine_.success=true;now=200;w.Tick();
  assert(w.page_==Page::Complete&&w.launchStarted_==0);}
#endif
 std::cout<<"PASS: actual GUI entry/tick: Debug simulation / Release real engine independent of embedded flag; consent/inspection/demo/path/preflight/start failure guards; captured selection; completion only from engine\n";
}
'''.replace('// LABELS',labels).replace('// METHODS','\n'.join([fn(s,'static constexpr bool SimulationBuild()'),fn(s,'[[nodiscard]] bool DebugDemoLocksControls()'),fn(s,'[[nodiscard]] bool HasSelectedComponent()'),start,'#ifdef _DEBUG\n'+fn(s,'void TickSimulation()')+'\n#endif',tick]))
compiler=shutil.which('clang++') or shutil.which('g++')
with tempfile.TemporaryDirectory(prefix='GlassRealEntry-') as d:
 p=Path(d)/'test.cpp';p.write_text(harness);exe=Path(d)/'test'
 for definitions in ([],['-D_DEBUG'],['-DGLASS_EMBED_MSI'],['-D_DEBUG','-DGLASS_EMBED_MSI']):
  subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror','-pedantic','-I'+str(root/'GlassSetup/src'),*definitions,str(p),'-o',str(exe)],check=True)
  subprocess.run([str(exe)],check=True)
