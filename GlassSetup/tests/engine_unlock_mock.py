#!/usr/bin/env python3
"""Exercise the actual Setup unlock methods with mocked Win32 and scanner calls.

This is not a Windows integration test. Run from any directory with Python 3
and a C++17 compiler (default: g++); no generated files enter the repository.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "GlassSetup/src/InstallEngine.cpp").read_text(encoding="utf-8")

def extract(begin, end):
    start = source.index(begin)
    return source[start:source.index(end, start)]

signal = extract("void InstallEngine::SignalHold(", "bool InstallEngine::StopGlassPlatform(")
release = extract("bool InstallEngine::ReleaseLockedModules(", "bool InstallEngine::EnsureTempWorkspace(")
header = (root / "GlassSetup/src/WizardUnlockState.h").as_posix()
prefix = r'''
#include "HEADER_PATH"
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cwchar>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
using DWORD=unsigned; using ULONGLONG=unsigned long long;
#define _wcsicmp wcscasecmp
constexpr int INFINITE=-1;
struct SRWLOCK { std::mutex mutex; };
void AcquireSRWLockExclusive(SRWLOCK* p){p->mutex.lock();}
void ReleaseSRWLockExclusive(SRWLOCK* p){p->mutex.unlock();}
struct Event { std::mutex mutex; std::condition_variable cv; bool signaled=false; };
using HANDLE=Event*;
void SetEvent(HANDLE e){std::lock_guard<std::mutex> l(e->mutex);e->signaled=true;e->cv.notify_all();}
void ResetEvent(HANDLE e){std::lock_guard<std::mutex> l(e->mutex);e->signaled=false;}
void WaitForSingleObject(HANDLE e,int){std::unique_lock<std::mutex> l(e->mutex);e->cv.wait(l,[&]{return e->signaled;});}
std::atomic<ULONGLONG> fakeTime{0};
ULONGLONG GetTickCount64(){return fakeTime.load();}
void Sleep(int n){fakeTime.fetch_add(n);}
namespace InstallLog { void Line(const wchar_t*){} template<class... A>void LineFormat(const wchar_t*,A...){} }
struct Harness {
 std::atomic_bool retrySucceeds{true},released{false},forceEntered{false},processEntered{false};
 std::atomic_bool failAfterProcess{false};
 bool emptyInitially=false,failWatch=false; int failAt=0;
 Event forceGate,processGate; std::atomic_int scans{0};
};
static Harness* harness=nullptr;
namespace GlassInstallUnlock {
 struct ModuleHolder {std::wstring exe=L"example.exe",modules=L"GlassCore.dll";DWORD pid=42;};
 std::wstring FormatHolders(const std::vector<ModuleHolder>&){return L"holder";}
 void CloseMatchingProcessWindows(DWORD,const wchar_t*){}
 bool TargetIsWow64(DWORD){return false;}
 std::vector<ModuleHolder> CollectHolders(const std::wstring&,const std::wstring&,bool,bool& failed,const std::atomic_bool*){
   const int n=++harness->scans;
   failed=(harness->failAt==n || (harness->failAfterProcess.load() && harness->processEntered.load()));
   if(failed || harness->emptyInitially || harness->released.load() || (harness->retrySucceeds.load() && n>1)) return {};
   return {ModuleHolder{}};
 }
 bool ForceUnloadHolder(const ModuleHolder&,const std::wstring&,const std::wstring&){
   harness->forceEntered.store(true,std::memory_order_release);WaitForSingleObject(&harness->forceGate,INFINITE);return true;
 }
 void UnlockProcess(DWORD,const wchar_t*){
   harness->processEntered.store(true,std::memory_order_release);WaitForSingleObject(&harness->processGate,INFINITE);
   harness->released.store(true,std::memory_order_release);
 }
}
namespace GlassInstallWatch {
 bool WaitUntilClearOrTimeout(int,const std::wstring&,const std::wstring&,bool,
     std::vector<GlassInstallUnlock::ModuleHolder>&,bool& failed,const std::atomic_bool*){
   failed=harness->failWatch; return false;
 }
}
bool IsProtectedProcessName(const wchar_t*){return false;}
class InstallEngine {
public:
 enum class HoldKind {None,Modules,Processes}; enum class HoldAction {None,Retry,Unlock,Cancel};
 struct Labels {std::wstring unload=L"scan",forceUnload=L"force",unlockProcesses=L"process",nativeScanFailed=L"error";} labels_;
 SRWLOCK statusLock_; std::atomic_bool cancelRequested_{false},quietClose_{false},failed_{false};
 HoldKind holdKind_=HoldKind::None; std::atomic<HoldAction> holdAction_{HoldAction::None};
 WizardUnlock::Activity unlockActivity_; Event gate; HANDLE holdGate_=&gate;
 std::wstring holdReport_,x64Dir_=L"x64",x86Dir_=L"x86"; bool scanNativeX64_=true,forceClean_=false;
 bool IsCancelRequested()const{return cancelRequested_.load(std::memory_order_acquire);}
 void SetFailure(const std::wstring&,unsigned){failed_.store(true);}
 void SetStatus(const std::wstring&,int){}
 void SignalHold(HoldAction action) noexcept;
 bool ReleaseLockedModules() noexcept;
 HoldKind CurrentHold(){AcquireSRWLockExclusive(&statusLock_);auto k=holdKind_;ReleaseSRWLockExclusive(&statusLock_);return k;}
};
'''.replace("HEADER_PATH", header)
main = r'''
template<class F>void until(F predicate){
 auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
 while(!predicate()){assert(std::chrono::steady_clock::now()<deadline);std::this_thread::yield();}
}
bool page(const InstallEngine& engine,const wchar_t* expected){
 return std::wstring_view(WizardUnlock::InstallingPage(engine.unlockActivity_.Current()))==expected;
}
int main(){
 for(bool retrySuccess:{true,false}){
   Harness h;h.retrySucceeds=retrySuccess;harness=&h;fakeTime=0;
   InstallEngine e;bool result=false;std::thread worker([&]{result=e.ReleaseLockedModules();});
   until([&]{return e.CurrentHold()==InstallEngine::HoldKind::Modules;});
   assert(page(e,L"installing")); e.SignalHold(InstallEngine::HoldAction::Retry);
   until([&]{return h.forceEntered.load(std::memory_order_acquire);}); assert(page(e,L"unlock-modules"));
   SetEvent(&h.forceGate);
   if(!retrySuccess){
     until([&]{return e.CurrentHold()==InstallEngine::HoldKind::Processes;}); assert(page(e,L"installing"));
     e.SignalHold(InstallEngine::HoldAction::Unlock);
     until([&]{return h.processEntered.load(std::memory_order_acquire);}); assert(page(e,L"unlock-processes"));
     SetEvent(&h.processGate);
   }
   worker.join();assert(result);assert(!e.failed_);assert(page(e,L"installing"));
 }
 {
   Harness h;harness=&h;fakeTime=0;InstallEngine e;bool result=true;
   std::thread worker([&]{result=e.ReleaseLockedModules();});
   until([&]{return e.CurrentHold()==InstallEngine::HoldKind::Modules;});e.SignalHold(InstallEngine::HoldAction::Retry);
   until([&]{return h.forceEntered.load(std::memory_order_acquire);});
   e.cancelRequested_.store(true,std::memory_order_release);SetEvent(&h.forceGate);worker.join();
   assert(!result);assert(!e.failed_);assert(page(e,L"installing"));
 }
 for(bool clean:{false,true}){
   Harness h;h.failAt=1;harness=&h;fakeTime=0;InstallEngine e;e.forceClean_=clean;
   assert(!e.ReleaseLockedModules());assert(e.failed_);assert(e.CurrentHold()==InstallEngine::HoldKind::None);
 }
 {
   Harness h;h.failWatch=true;harness=&h;fakeTime=0;InstallEngine e;
   assert(!e.ReleaseLockedModules());assert(e.failed_);assert(e.CurrentHold()==InstallEngine::HoldKind::None);
 }
 {
   Harness h;h.failAt=2;harness=&h;fakeTime=0;InstallEngine e;bool result=true;
   std::thread worker([&]{result=e.ReleaseLockedModules();});
   until([&]{return e.CurrentHold()==InstallEngine::HoldKind::Modules;});e.SignalHold(InstallEngine::HoldAction::Retry);
   until([&]{return h.forceEntered.load(std::memory_order_acquire);});SetEvent(&h.forceGate);worker.join();
   assert(!result);assert(e.failed_);assert(page(e,L"installing"));
 }
 {
   Harness h;h.retrySucceeds=false;h.failAfterProcess=true;harness=&h;fakeTime=0;InstallEngine e;bool result=true;
   std::thread worker([&]{result=e.ReleaseLockedModules();});
   until([&]{return e.CurrentHold()==InstallEngine::HoldKind::Modules;});e.SignalHold(InstallEngine::HoldAction::Retry);
   until([&]{return h.forceEntered.load(std::memory_order_acquire);});SetEvent(&h.forceGate);
   until([&]{return e.CurrentHold()==InstallEngine::HoldKind::Processes;});e.SignalHold(InstallEngine::HoldAction::Unlock);
   until([&]{return h.processEntered.load(std::memory_order_acquire);});SetEvent(&h.processGate);worker.join();
   assert(!result);assert(e.failed_);assert(page(e,L"installing"));
 }
 {
   Harness h;h.emptyInitially=true;harness=&h;fakeTime=0;InstallEngine e;
   assert(e.ReleaseLockedModules());assert(!e.failed_);assert(fakeTime.load()>=600);
 }
 std::cout<<"PASS: real unlock worker: retry, process release, cancellation, visibility and fail-closed scans\n";
}
'''

with tempfile.TemporaryDirectory(prefix="glass-unlock-test-") as directory:
    cpp = Path(directory) / "unlock.cpp"
    exe = Path(directory) / "unlock-test"
    cpp.write_text(prefix + signal + release + main, encoding="utf-8")
    compiler = os.environ.get("CXX", "g++")
    subprocess.run([compiler, "-std=c++17", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-pthread", str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
