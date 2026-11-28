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
#include <array>
#include <optional>
#include <mutex>
#include <string>
#include <iostream>
namespace GlassPackageCatalog {
constexpr std::size_t kCount=1;const char* kIds[]={"platform64"};struct Asset{bool downloadable=true;};
struct Catalog {int marker;std::array<std::optional<Asset>,kCount> products{};Catalog(int v=0):marker(v){}bool operator==(int v)const{return marker==v;}};
}
namespace GlassReleaseClient {std::wstring Wide(const char* text){return std::wstring(text,text+std::char_traits<char>::length(text));}}
namespace InstallLog{void Line(const wchar_t*){}template<class... T>void LineFormat(const wchar_t*,T...){} }
bool IsControllerChild(){return false;}
#include "resource.h"
std::wstring Text(unsigned){return L"Setup diagnostic";}
namespace GlassReleaseClient {enum class FailureKind{None,Transport,HttpStatus,Cancelled,Local,InvalidData};struct FailureInfo{FailureKind kind=FailureKind::None;unsigned long code=0;};}
enum class VersionFailureStage{None,Catalog,SetupDownload};
enum class Page{Checking,Components,Error};
bool controllerChild=false,noSelfUpdate=false;
bool IsControllerChild(){return controllerChild;}
bool HasCommandLineSwitch(const wchar_t*){return noSelfUpdate;}
struct VersionCheckResult{bool completed=false,fetched=false,newer=false,setupDownloadFailed=false;GlassPackageCatalog::Catalog catalog=7;
 std::wstring self=L"2.0.0.90",latest=L"2.0.0.70",preparedSetup,diagnostic;
 VersionFailureStage failureStage=VersionFailureStage::None;GlassReleaseClient::FailureInfo failure;};
struct App{Page page_=Page::Checking;std::mutex versionCheckMutex_;
 VersionCheckResult versionCheckResult_;bool versionCheckHandled_=false,inspectionApplied_=false,controllerCheckHandled_=false,versionRecoveryActive_=false;
 GlassPackageCatalog::Catalog releaseCatalog_;std::wstring errorReason_;int scans=0,advances=0,transitions=0,errors=0;bool transfer=false;
 bool TransitionController(const std::wstring&){++transitions;return transfer;}
 bool BeginProductInspection(){++scans;return true;}
 bool RunPreflightChecks(){return true;}
 void SetPage(Page page){page_=page;}
 void ContinueAfterLicenseCheck(){++advances;page_=Page::Components;}
 void ShowInstallError(const std::wstring&){++errors;}
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
 {App a;a.inspectionApplied_=true;a.versionCheckResult_.completed=true;a.versionCheckResult_.fetched=true;
  a.versionCheckResult_.newer=true;a.versionCheckResult_.setupDownloadFailed=true;
  a.CompleteVersionCheck();assert(a.advances==0&&a.errors==1&&a.versionCheckHandled_);}
 {App a;a.inspectionApplied_=true;a.versionCheckResult_.completed=true;a.versionCheckResult_.failureStage=VersionFailureStage::Catalog;
  a.versionCheckResult_.failure.kind=GlassReleaseClient::FailureKind::Transport;a.versionCheckResult_.diagnostic=L"timeout detail";
  a.CompleteVersionCheck();assert(a.page_==Page::Error&&a.versionRecoveryActive_&&a.advances==0&&a.errors==0);
  assert(a.errorReason_.find(L"timeout detail")!=std::wstring::npos);}
 {App a;a.inspectionApplied_=true;a.versionCheckResult_.completed=true;a.versionCheckResult_.fetched=true;
  a.versionCheckResult_.setupDownloadFailed=true;a.versionCheckResult_.failureStage=VersionFailureStage::SetupDownload;
  a.versionCheckResult_.failure.kind=GlassReleaseClient::FailureKind::Transport;
  a.CompleteVersionCheck();assert(a.page_==Page::Error&&a.versionRecoveryActive_&&a.advances==0);}
 {App a;a.inspectionApplied_=true;a.versionCheckResult_.completed=true;a.versionCheckResult_.failureStage=VersionFailureStage::Catalog;
  a.versionCheckResult_.failure.kind=GlassReleaseClient::FailureKind::InvalidData;
  a.CompleteVersionCheck();assert(!a.versionRecoveryActive_&&a.page_==Page::Components&&a.advances==1);}
 {App a;controllerChild=true;a.inspectionApplied_=true;a.versionCheckResult_.completed=true;
  a.versionCheckResult_.failureStage=VersionFailureStage::Catalog;a.versionCheckResult_.failure.kind=GlassReleaseClient::FailureKind::Transport;
  a.CompleteVersionCheck();assert(!a.versionRecoveryActive_&&a.advances==1&&a.scans==1);controllerChild=false;}
 {App a;noSelfUpdate=true;a.inspectionApplied_=true;a.versionCheckResult_.completed=true;
  a.versionCheckResult_.failureStage=VersionFailureStage::Catalog;a.versionCheckResult_.failure.kind=GlassReleaseClient::FailureKind::Transport;
  a.CompleteVersionCheck();assert(!a.versionRecoveryActive_&&a.advances==1);noSelfUpdate=false;}
 std::cout<<"PASS: actual Checking completion gate: transport recovery only on normal Setup path; catalog errors retain inspection gate\n";
}
'''
with tempfile.TemporaryDirectory(prefix='CheckingGate-') as d:
    path=Path(d)/'test.cpp';exe=Path(d)/'test'
    path.write_text(prefix+function+main)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(root/'GlassSetup/res'),str(path),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
