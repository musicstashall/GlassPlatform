"""Extract production UI-thread download gate; no network or Windows execution."""
from pathlib import Path
import subprocess, tempfile, shutil
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/main.cpp').read_text()
def fn(token):
 a=s.index(token);b=s.index('{',a);i=b+1;depth=1
 while depth:
  depth+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[a:i]
methods='\n'.join(fn(t) for t in ['void ReconcileDownloads()', 'void RequestComponentContinue()', 'void ContinueAfterDownloads()', 'void UpdateDownloadProgress()'])
code=r'''
#include <array>
#include <atomic>
#include <algorithm>
#include <memory>
#include <cstdint>
#include <cassert>
constexpr int kProductDownloadTimer=7,kMarqueePeriodMs=1400,FALSE=0;
int ticks=0;
void SetTimer(int,int,int,void*){} void KillTimer(int,int){}
void EnableWindow(int,bool){} void InvalidateRect(int,void*,int){}
unsigned long long GetTickCount64(){return ++ticks;}
namespace WizardProgress {int Percent(int v){return v/100;}}
enum class Page {Components,Checking,Certificate,Installing,License};
struct Job {std::atomic<bool> done{false};bool ok=false;std::atomic<std::uint64_t> total{0},bytes{0};};
struct Downloads {int calls=0;bool ready=false;std::array<std::shared_ptr<Job>,6> jobs;
 void Reconcile(const std::array<bool,6>&,int){++calls;}};
struct UI {
 Downloads downloads_;int releaseCatalog_=0,window=0,next_=0,progress_=0;
 Page page_=Page::Components;bool downloadContinuePending_=false,controlsReady_=true,simulation=false,locked=false,certificate=false;
 std::atomic<bool> inspectReady_{true};std::atomic<std::uint64_t> setupDownloadBytes_{0},setupDownloadTotal_{0};
 int progressValue_=0,displayProgress_=0,progressTarget_=0,percent_=0,installs=0;
 bool transitioning_=false;bool progressMarquee_=false;double marqueePhase_=0;
 bool SimulationBuild(){return simulation;}bool DebugDemoLocksControls(){return locked;}
 bool DownloadsReady(){return simulation||downloads_.ready;}
 bool ComponentsReady(){return !downloadContinuePending_||DownloadsReady();}
 std::array<bool,6> DownloadSelection(){return {{true,false,false,false,false,false}};}
 bool CertificatePageApplies(){return certificate;}
 void SetPage(Page p){page_=p;}void StartInstallation(){++installs;page_=Page::Installing;}
 void SyncWebViewState(){ReconcileDownloads();} void ShowContentControls(){}void InvalidateFooter(){}
// METHODS
};
int main(){
 UI ui;
 // Selecting/echoing the plan must not prepare or transfer packages.
 ui.SyncWebViewState();assert(ui.downloads_.calls==0);
 ui.RequestComponentContinue();assert(ui.page_==Page::Components&&ui.installs==0&&ui.downloadContinuePending_);
 const int calls=ui.downloads_.calls;ui.RequestComponentContinue();assert(ui.downloads_.calls==calls);
 ui.ContinueAfterDownloads();assert(ui.installs==0);
 // Timer completion automatically continues once, never before readiness.
 ui.downloads_.ready=true;ui.transitioning_=true;ui.ContinueAfterDownloads();assert(ui.installs==0&&ui.downloadContinuePending_);ui.transitioning_=false;ui.ContinueAfterDownloads();assert(ui.installs==1&&ui.page_==Page::Installing);
 ui.ContinueAfterDownloads();assert(ui.installs==1);
 UI ready;ready.downloads_.ready=true;ready.RequestComponentContinue();assert(ready.installs==1);
 UI cert;cert.certificate=true;cert.downloads_.ready=true;cert.RequestComponentContinue();assert(cert.page_==Page::Certificate&&cert.installs==0);
 UI back;back.page_=Page::License;back.ReconcileDownloads();assert(back.downloads_.calls==1);
 UI setup;setup.page_=Page::Checking;setup.setupDownloadTotal_=1000;setup.setupDownloadBytes_=500;
 setup.UpdateDownloadProgress();assert(setup.progressValue_==5000&&setup.percent_==50);
 setup.setupDownloadBytes_=1000;setup.UpdateDownloadProgress();assert(setup.progressValue_==9900);
 setup.setupDownloadTotal_=0;setup.UpdateDownloadProgress();assert(setup.progressMarquee_);
 UI transfer;auto job=std::make_shared<Job>();transfer.downloads_.jobs[0]=job;job->total=200;job->bytes=100;
 transfer.UpdateDownloadProgress();assert(transfer.percent_==50);job->done=true;job->ok=true;
 transfer.UpdateDownloadProgress();assert(transfer.percent_==100);
}
'''.replace('// METHODS',methods)
assert 'components-downloads' not in (root/'GlassSetup/res/slideshow.html').read_text()
assert 'GlassReleaseClient::packageProgress=[this,target' in s
assert 'case kSetupDownloadProgress:' in s
with tempfile.TemporaryDirectory(prefix='GlassDownloadContinue-') as temp:
 p=Path(temp)/'test.cpp';p.write_text(code);exe=Path(temp)/'test'
 subprocess.run([shutil.which('g++'),'-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: real Next gate: selection idle, wait, retry-safe, auto-continue, ready fast path, certificate, existing footer Setup/product progress')
