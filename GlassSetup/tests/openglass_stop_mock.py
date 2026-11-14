#!/usr/bin/env python3
"""Exercise the real OpenGlass stop helper with simulated SCM and Task Scheduler."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "GlassSetup/src/InstallSupport/GlassInstallHost.cpp").read_text(encoding="utf-8")

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

impl = function("bool StopOpenGlassHost(")
prefix = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
using DWORD=std::uint32_t;using ULONGLONG=std::uint64_t;using BOOL=int;using BYTE=unsigned char;using SC_HANDLE=void*;
constexpr BOOL TRUE=1,FALSE=0;
constexpr DWORD ERROR_SUCCESS=0,ERROR_CANCELLED=1223,ERROR_INVALID_DATA=13,ERROR_NOT_SUPPORTED=50,ERROR_TIMEOUT=1460,
 ERROR_ACCESS_DENIED=5,ERROR_SERVICE_DOES_NOT_EXIST=1060,ERROR_SERVICE_NOT_ACTIVE=1062;
constexpr DWORD SERVICE_STOPPED=1,SERVICE_START_PENDING=2,SERVICE_STOP_PENDING=3,SERVICE_RUNNING=4;
constexpr DWORD SERVICE_QUERY_STATUS=1,SERVICE_STOP=2,SERVICE_CONTROL_STOP=1,SC_MANAGER_CONNECT=1,SC_STATUS_PROCESS_INFO=0;
struct SERVICE_STATUS_PROCESS{DWORD dwCurrentState=0;};struct SERVICE_STATUS{};
namespace GlassInstallUtil {
 struct OpenGlassTaskState {bool found=false,anyDisabled=false,anyRunning=false,multipleTasks=false,multipleRunningInstances=false;};
}
struct Scenario {
 bool known=true,tasksKnown=true,scm=true,serviceExists=true,serviceAccess=true,serviceQuery=true,stopSuccess=true;
 bool manual=false,autoStop=true,cancelOnQuery=false,cancelOnSleep=false,taskStopSuccess=true;
 DWORD serviceState=SERVICE_STOPPED,error=0;GlassInstallUtil::OpenGlassTaskState tasks;
 int queries=0,taskQueries=0,serviceOpens=0,stopCalls=0,taskStopCalls=0,sleeps=0,logs=0;
 ULONGLONG tick=0;std::atomic_bool* cancel=nullptr;
} s;
DWORD GetLastError(){return s.error;}void SetLastError(DWORD error){s.error=error;}
ULONGLONG GetTickCount64(){return s.tick;}
void Sleep(DWORD ms){++s.sleeps;s.tick+=ms;if(s.cancelOnSleep && s.cancel)s.cancel->store(true);
 if(s.autoStop && s.sleeps>=2){s.serviceState=SERVICE_STOPPED;s.tasks.anyRunning=false;}}
SC_HANDLE OpenSCManagerW(void*,void*,DWORD){if(!s.scm){s.error=ERROR_ACCESS_DENIED;return nullptr;}return reinterpret_cast<void*>(1);}
SC_HANDLE OpenServiceW(SC_HANDLE,const wchar_t*,DWORD){++s.serviceOpens;
 if(!s.serviceAccess){s.error=ERROR_ACCESS_DENIED;return nullptr;}
 if(!s.serviceExists){s.error=ERROR_SERVICE_DOES_NOT_EXIST;return nullptr;}return reinterpret_cast<void*>(2);}
BOOL CloseServiceHandle(SC_HANDLE){return TRUE;}
BOOL QueryServiceStatusEx(SC_HANDLE,int,BYTE* out,DWORD,DWORD*){if(!s.serviceQuery){s.error=ERROR_ACCESS_DENIED;return FALSE;}
 reinterpret_cast<SERVICE_STATUS_PROCESS*>(out)->dwCurrentState=s.serviceState;return TRUE;}
BOOL ControlService(SC_HANDLE,DWORD,SERVICE_STATUS*){++s.stopCalls;if(!s.stopSuccess){s.error=ERROR_ACCESS_DENIED;return FALSE;}
 s.serviceState=SERVICE_STOP_PENDING;return TRUE;}
namespace GlassInstallLog{void Line(const wchar_t*){++s.logs;}}
namespace GlassInstallUtil {
 bool QueryOpenGlassRunning(bool& running){++s.queries;if(!s.known){s.error=ERROR_ACCESS_DENIED;return false;}
  running=(s.serviceExists && s.serviceState!=SERVICE_STOPPED) || s.tasks.anyRunning ||
   (!s.serviceExists && !s.tasks.found && s.manual);
  if(s.cancelOnQuery && s.cancel)s.cancel->store(true);return true;}
 bool QueryOpenGlassTasks(OpenGlassTaskState& out){++s.taskQueries;if(!s.tasksKnown){s.error=ERROR_ACCESS_DENIED;return false;}
  out=s.tasks;return true;}
 bool StopRunningOpenGlassTasks(bool& requested,const std::atomic_bool* cancel){++s.taskStopCalls;
  if(cancel && cancel->load()){SetLastError(ERROR_CANCELLED);return false;}
  requested=true;if(!s.taskStopSuccess){s.error=ERROR_ACCESS_DENIED;return false;}return true;}
}
namespace GlassInstallHost {
'''
main = r'''
}
using GlassInstallHost::StopOpenGlassHost;
int main(){
 auto reset=[](){s={};};bool requested=false;std::atomic_bool cancel=false;
 reset();assert(StopOpenGlassHost(&cancel,&requested));assert(!requested && s.queries==1 && s.serviceOpens==0 && s.sleeps==0);
 reset();s.serviceState=SERVICE_RUNNING;assert(StopOpenGlassHost(&cancel,&requested));
 assert(requested && s.stopCalls==1 && s.sleeps==2 && s.serviceState==SERVICE_STOPPED);
 reset();s.serviceExists=false;s.tasks.found=true;s.tasks.anyRunning=true;s.tasks.anyDisabled=true;
 assert(StopOpenGlassHost(&cancel,&requested));assert(requested && s.taskStopCalls==1 && s.stopCalls==0 && s.sleeps==2);
 reset();cancel.store(true);assert(!StopOpenGlassHost(&cancel,&requested));assert(!requested && s.queries==0 && s.error==ERROR_CANCELLED);
 cancel.store(false);reset();s.cancel=&cancel;s.cancelOnQuery=true;s.serviceState=SERVICE_RUNNING;
 assert(!StopOpenGlassHost(&cancel,&requested));assert(!requested && s.stopCalls==0 && s.error==ERROR_CANCELLED);
 cancel.store(false);reset();s.cancel=&cancel;s.cancelOnSleep=true;s.serviceState=SERVICE_RUNNING;
 assert(!StopOpenGlassHost(&cancel,&requested));assert(requested && s.stopCalls==1 && s.sleeps==2 && s.error==ERROR_CANCELLED);
 cancel.store(false);reset();s.cancel=&cancel;s.cancelOnSleep=true;s.serviceState=SERVICE_STOP_PENDING;s.autoStop=false;
 assert(!StopOpenGlassHost(&cancel,&requested));assert(!requested && s.stopCalls==0 && s.sleeps==1 && s.error==ERROR_CANCELLED);
 cancel.store(false);reset();s.serviceState=SERVICE_RUNNING;s.tasks.found=true;s.tasks.anyRunning=true;s.tasks.multipleTasks=true;
 assert(!StopOpenGlassHost(&cancel,&requested));assert(!requested && s.serviceOpens==0 && s.error==ERROR_INVALID_DATA);
 reset();s.serviceState=SERVICE_RUNNING;s.stopSuccess=false;assert(!StopOpenGlassHost(&cancel,&requested));
 assert(requested && s.stopCalls==1 && s.error==ERROR_ACCESS_DENIED);
 reset();s.known=false;assert(!StopOpenGlassHost(&cancel,&requested));assert(!requested && s.sleeps==0);
 reset();s.serviceState=SERVICE_RUNNING;s.autoStop=false;assert(!StopOpenGlassHost(&cancel,&requested));
 assert(requested && s.error==ERROR_TIMEOUT && s.tick>=20000);
 reset();s.serviceExists=false;s.manual=true;assert(!StopOpenGlassHost(&cancel,&requested));
 assert(!requested && s.error==ERROR_NOT_SUPPORTED && s.sleeps==0);
 std::cout<<"PASS: real stop helper: stopped service/task, running backends, cancellation, errors and timeout\n";
}
'''
with tempfile.TemporaryDirectory(prefix="glass-og-stop-") as directory:
    cpp = Path(directory) / "stop.cpp"
    exe = Path(directory) / "stop"
    cpp.write_text(prefix + impl + "\n" + main, encoding="utf-8")
    compiler = os.environ.get("CXX", "g++")
    subprocess.run([compiler, "-std=c++17", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
