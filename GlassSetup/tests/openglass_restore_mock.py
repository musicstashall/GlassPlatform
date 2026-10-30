#!/usr/bin/env python3
"""Exercise the real prior-host rollback path; do not restart a previously stopped OpenGlass."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "GlassSetup/src/InstallEngine.cpp").read_text(encoding="utf-8")

def function(token):
    start = source.index(token); brace = source.index("{", start); depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{": depth += 1
        if source[pos] == "}":
            depth -= 1
            if depth == 0: return source[start:pos + 1]
    raise ValueError(token)

impl = function("bool InstallEngine::RestorePriorHosts(")
prefix = r'''
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
using DWORD=std::uint32_t;using ULONGLONG=std::uint64_t;using BOOL=int;using HANDLE=void*;
constexpr int FALSE=0,STARTF_USESHOWWINDOW=1,SW_HIDE=0,CREATE_NO_WINDOW=1;
constexpr DWORD SERVICE_DISABLED=4,SERVICE_DEMAND_START=3;
struct STARTUPINFOW{DWORD cb=0,dwFlags=0;unsigned short wShowWindow=0;};
struct PROCESS_INFORMATION{HANDLE hThread=nullptr,hProcess=nullptr;};
struct Scenario{bool startOk=true,taskOk=true,queryOk=true,running=false,restoreTypeOk=true;
 int serviceStarts=0,taskStarts=0,restoreTypes=0;DWORD startType=0;ULONGLONG now=0;} s;
ULONGLONG GetTickCount64(){return s.now;}void Sleep(DWORD ms){s.now+=ms;}
BOOL CloseHandle(HANDLE){return 1;}BOOL PathFileExistsW(const wchar_t*){return 1;}
BOOL CreateProcessW(const wchar_t*,wchar_t*,void*,void*,BOOL,DWORD,void*,void*,STARTUPINFOW*,PROCESS_INFORMATION*){return 1;}
namespace GP {
 enum class PlatformArch{X64,X86};
 bool WaitForPlatformStopped(PlatformArch,DWORD){return true;}
 bool StartPlatform(PlatformArch){return true;}
}
bool QueryPlatformMutex(GP::PlatformArch,bool& running){running=false;return true;}
namespace GlassInstallUtil {
 std::wstring NativeRundll64(){return L"rundll64";}std::wstring RundllPathForTaskArch(GP::PlatformArch){return L"rundll";}
 std::wstring TrimTrailingSlashes(std::wstring path){return path;}
 bool RunEnabledOpenGlassTask(){++s.taskStarts;if(s.taskOk)s.running=true;return s.taskOk;}
 bool QueryOpenGlassRunning(bool& running){running=s.running;return s.queryOk;}
}
namespace GlassInstallHost {
 bool RestoreOpenGlassServiceStartType(DWORD){++s.restoreTypes;return s.restoreTypeOk;}
 bool StartOpenGlassServicePreservingStartType(DWORD type){++s.serviceStarts;s.startType=type;return s.startOk;}
}
class InstallEngine {
public:
 struct Prior{bool x64WasRunning=false,x86WasRunning=false,x64TaskExisted=false,x86TaskExisted=false,
  x64TaskEnabled=true,x86TaskEnabled=true,ogServiceExisted=false,ogLegacyTaskExisted=false,
  ogWasRunning=false,ogLegacyTaskEnabled=true;DWORD ogServiceStartType=SERVICE_DEMAND_START;} prior_;
 struct Selection{bool platform64=true,platform32=true;} selection_;
 bool hostsStopped_=true,x64StopSignalled_=false,x86StopSignalled_=false,ogStopSignalled_=false,
  ogServiceStartTypeMayNeedRestore_=false;
 std::wstring x64Dir_=L"x64",x86Dir_=L"x86";
 bool RestorePriorHosts(bool selectedOnly=false) noexcept;
};
'''
main = r'''
int main(){
 auto reset=[](){s={};};
 {reset();InstallEngine e;e.prior_.ogServiceExisted=true;e.prior_.ogWasRunning=false;
  assert(e.RestorePriorHosts());assert(s.serviceStarts==0 && s.taskStarts==0);}
 {reset();InstallEngine e;e.prior_.ogLegacyTaskExisted=true;e.prior_.ogWasRunning=false;
  assert(e.RestorePriorHosts());assert(s.serviceStarts==0 && s.taskStarts==0);}
 {reset();InstallEngine e;e.prior_.ogServiceExisted=true;e.prior_.ogWasRunning=true;e.ogStopSignalled_=true;
  e.prior_.ogServiceStartType=SERVICE_DISABLED;assert(e.RestorePriorHosts());
  assert(s.serviceStarts==1 && s.startType==SERVICE_DISABLED && !e.ogServiceStartTypeMayNeedRestore_);}
 {reset();InstallEngine e;e.prior_.ogLegacyTaskExisted=true;e.prior_.ogWasRunning=true;e.ogStopSignalled_=true;
  assert(e.RestorePriorHosts());assert(s.taskStarts==1 && s.running);}
 {reset();InstallEngine e;e.prior_.ogServiceExisted=true;e.prior_.ogWasRunning=true;e.ogStopSignalled_=false;
  assert(e.RestorePriorHosts());assert(s.serviceStarts==0);} // we did not send a stop
 {reset();InstallEngine e;e.prior_.ogServiceExisted=true;e.prior_.ogWasRunning=true;e.ogStopSignalled_=true;s.startOk=false;
  assert(!e.RestorePriorHosts());assert(s.serviceStarts==1);}
 {reset();InstallEngine e;e.prior_.ogServiceExisted=true;e.prior_.ogWasRunning=true;e.ogStopSignalled_=true;
  assert(e.RestorePriorHosts(true));assert(s.serviceStarts==0);} // normal launch phase handles OG
 std::cout<<"PASS: real host rollback: stopped OpenGlass stays stopped, running service/task restored\n";
}
'''
with tempfile.TemporaryDirectory(prefix="glass-og-restore-") as directory:
    cpp = Path(directory) / "restore.cpp"; exe = Path(directory) / "restore"
    cpp.write_text(prefix + impl + "\n" + main, encoding="utf-8")
    compiler = os.environ.get("CXX", "g++")
    subprocess.run([compiler, "-std=c++17", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
