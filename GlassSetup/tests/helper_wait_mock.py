#!/usr/bin/env python3
"""Test the actual cancellable helper wait with a deterministic process model."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "GlassSetup/src/InstallSupport/GlassInstallUtil.cpp").read_text(encoding="utf-8")
start = source.index("bool RunHiddenCommand(")
end = source.index("bool RunHidden(", start)
function = source[start:end]
prefix = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
using DWORD=std::uint32_t;using ULONGLONG=std::uint64_t;using BOOL=int;using HANDLE=int;
constexpr BOOL TRUE=1,FALSE=0;constexpr DWORD WAIT_OBJECT_0=0,WAIT_TIMEOUT=258,WAIT_FAILED=0xffffffff,
 ERROR_CANCELLED=1223,ERROR_ACCESS_DENIED=5,STARTF_USESHOWWINDOW=1,STARTF_FORCEOFFFEEDBACK=2,SW_HIDE=0,CREATE_NO_WINDOW=1;
struct STARTUPINFOW{DWORD cb=0,dwFlags=0;int wShowWindow=0;};
struct PROCESS_INFORMATION{HANDLE hProcess=0,hThread=0;};
struct Scenario{
 ULONGLONG now=0,exitAt=~ULONGLONG(0),cancelAt=~ULONGLONG(0);bool create=true,failWait=false,failExit=false,terminated=false;
 DWORD error=ERROR_ACCESS_DENIED,code=0;int created=0,terminateCalls=0;std::atomic_bool* cancel=nullptr;
} s;
DWORD GetLastError(){return s.error;}ULONGLONG GetTickCount64(){return s.now;}
BOOL CreateProcessW(const wchar_t*,wchar_t*,void*,void*,BOOL,DWORD,void*,const wchar_t*,STARTUPINFOW*,PROCESS_INFORMATION* out){
 ++s.created;if(!s.create)return FALSE;out->hProcess=1;out->hThread=2;return TRUE;
}
BOOL CloseHandle(HANDLE){return TRUE;}
DWORD WaitForSingleObject(HANDLE h,DWORD ms){
 assert(h==1);if(s.terminated)return WAIT_OBJECT_0;if(s.failWait)return WAIT_FAILED;
 s.now+=ms;if(s.cancel && s.now>=s.cancelAt)s.cancel->store(true);
 return s.now>=s.exitAt?WAIT_OBJECT_0:WAIT_TIMEOUT;
}
BOOL TerminateProcess(HANDLE h,DWORD){assert(h==1);s.terminated=true;++s.terminateCalls;return TRUE;}
BOOL GetExitCodeProcess(HANDLE,DWORD* out){if(s.failExit)return FALSE;*out=s.code;return TRUE;}
namespace GlassInstallUtil {
'''
main = r'''
}
using namespace GlassInstallUtil;
void reset(){s=Scenario{};}
int main(){
 DWORD code=0;bool started=false,completed=false;std::atomic_bool cancelled{true};
 reset();assert(!RunHiddenCommand(L"helper",&code,1000,&started,&completed,&cancelled));
 assert(code==ERROR_CANCELLED && !started && !completed && s.created==0);
 reset();cancelled=false;s.cancel=&cancelled;s.cancelAt=250;
 assert(!RunHiddenCommand(L"helper",&code,1000,&started,&completed,&cancelled));
 assert(code==ERROR_CANCELLED && started && !completed && s.terminated && s.now==300);
 reset();cancelled=false;s.exitAt=250;s.code=7;
 assert(RunHiddenCommand(L"helper",&code,1000,&started,&completed,&cancelled));
 assert(code==7 && started && completed && !s.terminated);
 reset();cancelled=false;assert(!RunHiddenCommand(L"helper",&code,350,&started,&completed,&cancelled));
 assert(code==WAIT_TIMEOUT && s.terminated);
 reset();cancelled=false;s.failWait=true;assert(!RunHiddenCommand(L"helper",&code,1000,&started,&completed,&cancelled));
 assert(code==ERROR_ACCESS_DENIED && s.terminated);
 reset();s.create=false;assert(!RunHiddenCommand(L"helper",&code,1000,&started,&completed,nullptr));assert(!started && !s.terminated);
 std::cout<<"PASS: real helper wait: pre-cancel, in-flight cancel, exit, timeout and failed wait\n";
}
'''
with tempfile.TemporaryDirectory(prefix="glass-helper-test-") as directory:
    cpp = Path(directory) / "helper.cpp"
    exe = Path(directory) / "helper-test"
    cpp.write_text(prefix + function + main, encoding="utf-8")
    compiler = os.environ.get("CXX", "g++")
    subprocess.run([compiler, "-std=c++17", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
