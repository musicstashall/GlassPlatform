"""Actual CompleteVersionCheck plus source routing: both async results must be
applied before leaving Checking. Fake UI/network data; no Windows MSI execution.
"""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/main.cpp').read_text()
a=s.index('    void CompleteVersionCheck() {');b=s.index('\n#ifdef _DEBUG',a)
function=s[a:b]
startup=s[s.index('    void CompleteStartupIfReady('):s.index('    void Layout()',s.index('    void CompleteStartupIfReady('))]
assert 'GlassProductInspect::Run' not in startup and 'inspectThread_=std::thread' not in startup
start=s[s.index('    void ContinueVersionCheck()'):s.index('    void StartVersionCheck()')]
assert start.index('SetPage(Page::Checking)')<start.index('BeginProductInspection()')
ready=s[s.index('    void OnInspectReady()'):s.index('    void SetPage(',s.index('    void OnInspectReady()'))]
assert ready.index('inspectionApplied_=true')<ready.index('CompleteVersionCheck()')
assert 'if(!inspectionApplied_)return;' in function
prefix=r'''
#include <cassert>
#include <mutex>
#include <string>
#include <iostream>
namespace InstallLog{void Line(const wchar_t*){}template<class... T>void LineFormat(const wchar_t*,T...){} }
bool IsControllerChild(){return false;}
enum class Page{Checking,Components};
struct VersionCheckResult{bool completed=false,fetched=false,newer=false;int catalog=7;
 std::wstring self=L"2.0.0.90",latest=L"2.0.0.70",preparedSetup;};
struct App{Page page_=Page::Checking;std::mutex versionCheckMutex_;
 VersionCheckResult versionCheckResult_;bool versionCheckHandled_=false,inspectionApplied_=false,controllerCheckHandled_=false;
 int scans=0;int releaseCatalog_=0,advances=0,transitions=0;bool transfer=false;
 bool TransitionController(const std::wstring&){++transitions;return transfer;}
 bool BeginProductInspection(){++scans;return true;}
 bool RunPreflightChecks(){return true;}
 void ContinueAfterLicenseCheck(){++advances;page_=Page::Components;}
'''
main=r'''
};
int main(){
 {App a;a.CompleteVersionCheck();assert(a.advances==0);
  a.versionCheckResult_.completed=true;a.CompleteVersionCheck();assert(a.advances==0&&!a.versionCheckHandled_);
  a.inspectionApplied_=true;a.CompleteVersionCheck();assert(a.advances==1&&a.releaseCatalog_==7);
  a.CompleteVersionCheck();assert(a.advances==1);}
 for(bool fetched:{false,true})for(bool newer:{false,true}){
  App a;a.inspectionApplied_=true;a.CompleteVersionCheck();assert(a.advances==0);
  a.versionCheckResult_.completed=true;a.versionCheckResult_.fetched=fetched;a.versionCheckResult_.newer=newer;
  a.CompleteVersionCheck();assert(a.advances==1&&a.versionCheckHandled_);
 }
 {App a;a.inspectionApplied_=true;a.versionCheckResult_.completed=true;a.transfer=true;
  a.CompleteVersionCheck();assert(a.transitions==1&&a.advances==0&&a.scans==0);}
 std::cout<<"PASS: actual Checking completion gate: inspection/network in both orders, failed/offline catalog, one advance\n";
}
'''
with tempfile.TemporaryDirectory(prefix='CheckingGate-') as d:
    path=Path(d)/'test.cpp';exe=Path(d)/'test'
    path.write_text(prefix+function+main)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(path),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
