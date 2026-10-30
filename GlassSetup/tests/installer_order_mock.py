#!/usr/bin/env python3
"""Verify Setup stage order and exercise the real early-MSI/purge/mutex code with mocks.

This is a deterministic C++ model, not a real MSI or Windows integration test.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "GlassSetup/src/InstallEngine.cpp").read_text(encoding="utf-8")

def section(start, end):
    pos = source.index(start)
    return source[pos:source.index(end, pos)]

def function(token):
    start = source.index(token)
    brace = source.index("{", start)
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{": depth += 1
        if source[pos] == "}":
            depth -= 1
            if depth == 0: return source[start:pos + 1]
    raise ValueError(token)

worker = section("DWORD WINAPI InstallEngine::WorkerThread(", "bool InstallEngine::Start(")
stages = ["CapturePriorHostState()", "ExtractPayload(", "CreateRollbackSnapshot()", "PrepareProductRemoval()",
          "CloseGlassSettingsProcesses(", "StopRunningHosts()", "InstallGlassSettingsEarly()",
          "ReleaseLockedModules()", "PurgeRemnants()", "RunMsiSequence()", "LaunchAndVerifySelectedProducts()"]
positions = [worker.index(stage) for stage in stages]
assert positions == sorted(positions), stages
assert worker.index("nonSettingsChangesAttempted_ = true") < worker.index("PurgeRemnants()")
assert "self->PerformRollback()" in worker and "self->UndoCleanInstall()" in worker
sequence = section("bool InstallEngine::RunMsiSequence()", "bool InstallEngine::LaunchAndVerifySelectedProducts()")
assert 'steps.push_back({L"GlassSettings-' not in sequence
assert "!selection_.glassSettings" in sequence and "kProductSettingsX86" in sequence

mutex_fn = function("bool QueryPlatformMutex(")
stop_fn = function("bool InstallEngine::StopGlassPlatform(")
determine_fn = function("bool InstallEngine::DetermineCleanInstall(")
early_fn = function("bool InstallEngine::InstallGlassSettingsEarly(")
purge_fn = function("bool InstallEngine::PurgeRemnants(")

prefix = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cwchar>
#include <functional>
#include <iostream>
#include <set>
#include <string>
#include <vector>
using DWORD=std::uint32_t;using UINT=unsigned;using ULONGLONG=std::uint64_t;using BOOL=int;
#define _wcsicmp wcscasecmp
#define PRIMARYLANGID(x) (x)
constexpr DWORD ERROR_FILE_NOT_FOUND=2,ERROR_ACCESS_DENIED=5,ERROR_SUCCESS=0,ERROR_INSTALL_USEREXIT=1602;
constexpr DWORD SYNCHRONIZE=1;constexpr BOOL FALSE=0;constexpr int MAX_PATH=260,LANG_RUSSIAN=25;
struct Mock {
 bool is64=true,settingsRunning=false,queryOk=true,msiSuccess=true,snapshotted=true,clearOk=true;
 bool mutexPresent=false,denyMutex=false,signalOk=true,wowOk=true,wow=true;
 ULONGLONG now=0,closeAt=0,denyAt=~ULONGLONG(0);
 DWORD lastError=0;int queries=0,msiCalls=0,signals=0,orphans=0;
 std::wstring msiPath,msiDir,msiLabel;int base=0,span=0;
 std::set<std::wstring> installed;std::vector<std::wstring> modified,removed;std::vector<int> progress;
} fake;
using HANDLE=void*;
DWORD GetLastError(){return fake.lastError;}
int GetUserDefaultUILanguage(){return 0;}
ULONGLONG GetTickCount64(){return fake.now;}
void Sleep(DWORD ms){fake.now+=ms;}
BOOL CloseHandle(HANDLE){return 1;}
HANDLE GetCurrentProcess(){return nullptr;}
BOOL IsWow64Process(HANDLE,BOOL* result){*result=fake.wow;return fake.wowOk;}
HANDLE OpenMutexW(DWORD,BOOL,const wchar_t*){
 if(fake.denyMutex || fake.now>=fake.denyAt){fake.lastError=ERROR_ACCESS_DENIED;return nullptr;}
 if(fake.mutexPresent && (fake.closeAt==0 || fake.now<fake.closeAt))return reinterpret_cast<HANDLE>(1);
 fake.lastError=ERROR_FILE_NOT_FOUND;return nullptr;
}
wchar_t* PathCombineW(wchar_t* out,const wchar_t* dir,const wchar_t* name){
 std::wstring result=std::wstring(dir)+L"/"+name;if(result.size()>=MAX_PATH)return nullptr;
 std::wcscpy(out,result.c_str());return out;
}
namespace GP {
 enum class PlatformArch {X86,X64};
 const wchar_t* InstanceMutexName(PlatformArch arch){return arch==PlatformArch::X64?L"x64":L"x86";}
 bool SignalPlatformStop(PlatformArch){++fake.signals;return fake.signalOk;}
 bool StopOrphanAutorunTaskInstance(PlatformArch){++fake.orphans;return true;}
}
enum class ArchPresence {Absent,Running};
ArchPresence ClassifyArch(GP::PlatformArch){return ArchPresence::Absent;}
bool OpenGlassPresent(const std::wstring&){return false;}
namespace InstallLog { void Line(const wchar_t*){} template<class... T>void LineFormat(const wchar_t*,T...){} }
namespace GlassInstallRollback {
 struct SessionState {};
 void NoteModified(SessionState&,const wchar_t* code){fake.modified.push_back(code);}
}
bool ProductWasSnapshotted(const GlassInstallRollback::SessionState&,const wchar_t*){return fake.snapshotted;}
namespace GlassInstallUtil {
 bool IsOs64Bit(){return fake.is64;}
 bool QueryImageRunning(const wchar_t*,bool& found){++fake.queries;found=fake.settingsRunning;return fake.queryOk;}
 bool ClearPlatformInstallPath(bool){return fake.clearOk;}
 bool ClearOpenGlassInstallPath(){return fake.clearOk;}
}
namespace GlassInstallMsi {
 constexpr wchar_t kProductSettingsX64[]=L"gs64",kProductSettingsX86[]=L"gs86",kProductPlatformX64[]=L"p64",
  kProductPlatformX86[]=L"p86",kProductOpenGlass[]=L"og";
 struct ProgressSink {
  std::wstring label;int base=0,span=0;std::atomic_bool* cancelRequested=nullptr;
  std::function<void(const std::wstring&)> onError;
 };
 bool IsProductInstalled(const wchar_t* code){return fake.installed.count(code)!=0;}
 bool UninstallProduct(const wchar_t* code,ProgressSink*,UINT* result){
  fake.removed.push_back(code);*result=fake.msiSuccess?ERROR_SUCCESS:1;
  if(fake.msiSuccess)fake.installed.erase(code);return fake.msiSuccess;
 }
}
class InstallEngine {
public:
 struct Selection {bool glassSettings=true;} selection_;
 struct Prior {bool x64WasRunning=false,x86WasRunning=false,x64TaskExisted=false,x86TaskExisted=false,
  ogLegacyTaskExisted=false,ogServiceExisted=false,ogWasRunning=false;} prior_;
 struct Labels {std::wstring glassSettings64=L"gs64-label",glassSettings86=L"gs86-label",purge=L"purge",purgeFailed=L"purge-error",nativeScanFailed=L"scan-error";} labels_;
 GlassInstallRollback::SessionState rollbackSession_;
 std::wstring tempDir_=L"secure",x64Dir_=L"root64",x86Dir_=L"root86",openGlassDir_=L"og",error_;
 bool installAttempted_=false,purgeCompleted_=false,x64StopSignalled_=false,x86StopSignalled_=false,
  cleanInstall_=false,scanNativeX64_=false;
 std::atomic_bool cancelRequested_{false};
 bool IsCancelRequested()const{return cancelRequested_.load();}
 void SetFailure(const std::wstring& text,unsigned){error_=text;}
 void RememberMsiMessage(const wchar_t*){}
 void SetStatus(const std::wstring&,int value){fake.progress.push_back(value);}
 bool RunOneMsi(const std::wstring& path,const std::wstring& dir,const std::wstring& label,int base,int span){
  assert(installAttempted_);++fake.msiCalls;fake.msiPath=path;fake.msiDir=dir;fake.msiLabel=label;fake.base=base;fake.span=span;return fake.msiSuccess;
 }
 bool InstallGlassSettingsEarly() noexcept;
 bool DetermineCleanInstall() noexcept;
 bool PurgeRemnants() noexcept;
 bool StopGlassPlatform(GP::PlatformArch,const std::wstring&,const wchar_t*,const std::function<bool(const wchar_t*)>&) noexcept;
};
'''
main = r'''
int main(){
 auto reset=[](){fake={};};
 {reset();InstallEngine e;assert(e.DetermineCleanInstall());assert(e.scanNativeX64_ && e.cleanInstall_);}
 {reset();InstallEngine e;fake.wow=false;assert(e.DetermineCleanInstall());assert(!e.scanNativeX64_);}
 {reset();InstallEngine e;fake.wowOk=false;assert(!e.DetermineCleanInstall());assert(!e.error_.empty());}
 {reset();InstallEngine e;e.selection_.glassSettings=false;assert(e.InstallGlassSettingsEarly());
  assert(fake.msiCalls==0 && fake.queries==0 && !e.installAttempted_);}
 {reset();InstallEngine e;fake.queryOk=false;assert(!e.InstallGlassSettingsEarly());assert(!e.error_.empty() && !e.installAttempted_);}
 {reset();InstallEngine e;fake.settingsRunning=true;assert(!e.InstallGlassSettingsEarly());assert(fake.msiCalls==0 && !e.installAttempted_);}
 for(bool x64:{false,true}){reset();InstallEngine e;fake.is64=x64;e.scanNativeX64_=x64;assert(e.InstallGlassSettingsEarly());
  assert(e.installAttempted_ && fake.msiCalls==1 && fake.base==800 && fake.span==1200);
  assert(fake.msiPath==(x64?L"secure/GlassSettings-x64.msi":L"secure/GlassSettings-x86.msi"));
  assert(fake.msiDir==(x64?L"root64":L"root86"));}
 {reset();InstallEngine e;fake.msiSuccess=false;assert(!e.InstallGlassSettingsEarly());assert(e.installAttempted_);}
 {reset();InstallEngine e;e.scanNativeX64_=true;fake.installed={L"gs64",L"gs86",L"p64"};assert(e.PurgeRemnants());
  assert(fake.installed.count(L"gs64") && !fake.installed.count(L"gs86") && !fake.installed.count(L"p64"));
  assert(fake.removed.size()==2 && fake.modified.size()==2 && e.purgeCompleted_ && fake.progress.back()==2200);}
 {reset();InstallEngine e;e.selection_.glassSettings=false;fake.installed={L"gs64",L"gs86"};
  assert(e.PurgeRemnants());assert(fake.installed.empty() && fake.removed.size()==2);}
 {reset();InstallEngine e;fake.is64=false;fake.installed={L"gs86",L"p86"};assert(e.PurgeRemnants());
  assert(fake.installed.count(L"gs86") && !fake.installed.count(L"p86"));}
 {reset();InstallEngine e;fake.installed={L"p64"};fake.snapshotted=false;assert(!e.PurgeRemnants());
  assert(!e.error_.empty() && fake.removed.empty());}
 {reset();InstallEngine e;fake.installed={L"p64"};e.cancelRequested_=true;assert(!e.PurgeRemnants());assert(fake.removed.empty());}
 int fails=0;auto fail=[&](const wchar_t*){++fails;return false;};
 {reset();InstallEngine e;assert(e.StopGlassPlatform(GP::PlatformArch::X64,L"stop",L"x64",fail));
  assert(fake.signals==0 && fake.orphans==0);}
 {reset();InstallEngine e;e.prior_.x64WasRunning=true;fake.mutexPresent=true;fake.closeAt=750;
  assert(e.StopGlassPlatform(GP::PlatformArch::X64,L"stop",L"x64",fail));
  assert(fake.now==750 && fake.signals==1 && fake.orphans==1 && e.x64StopSignalled_);}
 {reset();InstallEngine e;fake.denyMutex=true;fails=0;
  assert(!e.StopGlassPlatform(GP::PlatformArch::X64,L"stop",L"x64",fail));assert(fails==1 && fake.signals==0);}
 {reset();InstallEngine e;e.prior_.x64WasRunning=true;fake.mutexPresent=true;fake.denyAt=250;fails=0;
  assert(!e.StopGlassPlatform(GP::PlatformArch::X64,L"stop",L"x64",fail));assert(fails==1 && fake.signals==1);}
 {reset();InstallEngine e;fake.mutexPresent=true;
  assert(!e.StopGlassPlatform(GP::PlatformArch::X64,L"stop",L"x64",fail));assert(!e.error_.empty() && fake.signals==0);}
 {reset();InstallEngine e;e.prior_.x64WasRunning=true;fake.mutexPresent=true;fails=0;
  assert(!e.StopGlassPlatform(GP::PlatformArch::X64,L"stop",L"x64",fail));assert(fails==1 && fake.now>=45000);}
 {reset();InstallEngine e;e.prior_.x64WasRunning=true;fake.mutexPresent=true;fake.closeAt=500;e.cancelRequested_=true;
  assert(!e.StopGlassPlatform(GP::PlatformArch::X64,L"stop",L"x64",fail));assert(fake.now==500 && e.x64StopSignalled_);}
 std::cout<<"PASS: real stage functions: MSI selection, purge preservation, mutex states, timeout and cancellation\n";
}
'''
with tempfile.TemporaryDirectory(prefix="glass-order-test-") as directory:
    cpp = Path(directory) / "order.cpp"
    exe = Path(directory) / "order"
    cpp.write_text(prefix + mutex_fn + "\n" + stop_fn + "\n" + determine_fn + "\n" + early_fn + "\n" + purge_fn + "\n" + main, encoding="utf-8")
    compiler = os.environ.get("CXX", "g++")
    subprocess.run([compiler, "-std=c++17", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("PASS: worker stage order and rollback boundaries")
