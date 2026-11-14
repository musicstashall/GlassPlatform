#!/usr/bin/env python3
"""Exercise the actual temporary disabled-service start/restore code with a Win32 model."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "GlassSetup/src/InstallSupport/GlassInstallHost.cpp").read_text(encoding="utf-8")
start = source.index("bool StartOpenGlassServicePreservingStartType(")
end = source.index("bool RemoveOpenGlassServiceAndTasks(", start)
implementation = source[start:end]
prefix = r'''
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
using DWORD=std::uint32_t;using BOOL=int;using SC_HANDLE=void*;
constexpr BOOL TRUE=1,FALSE=0;constexpr DWORD SERVICE_AUTO_START=2,SERVICE_DEMAND_START=3,SERVICE_DISABLED=4,
 SERVICE_NO_CHANGE=0xffffffff,SC_MANAGER_CONNECT=1,SERVICE_START=2,SERVICE_QUERY_STATUS=4,SERVICE_CHANGE_CONFIG=8,
 ERROR_SUCCESS=0,ERROR_ACCESS_DENIED=5,ERROR_SERVICE_ALREADY_RUNNING=1056;
struct Scenario{
 bool scm=true,open=true,start=true,failEnable=false,failRestore=false;DWORD error=ERROR_SUCCESS,type=SERVICE_DISABLED;
 std::vector<std::string> calls;
} s;
DWORD GetLastError(){return s.error;}void SetLastError(DWORD n){s.error=n;}
SC_HANDLE OpenSCManagerW(void*,void*,DWORD){if(!s.scm){s.error=ERROR_ACCESS_DENIED;return nullptr;}return reinterpret_cast<void*>(1);}
SC_HANDLE OpenServiceW(SC_HANDLE,const wchar_t*,DWORD){if(!s.open){s.error=ERROR_ACCESS_DENIED;return nullptr;}return reinterpret_cast<void*>(2);}
BOOL CloseServiceHandle(SC_HANDLE){return TRUE;}
BOOL ChangeServiceConfigW(SC_HANDLE,DWORD,DWORD type,DWORD,const wchar_t*,const wchar_t*,DWORD*,const wchar_t*,const wchar_t*,const wchar_t*,const wchar_t*){
 s.calls.push_back(type==SERVICE_DISABLED?"disable":"enable");
 if((type==SERVICE_DISABLED && s.failRestore) || (type==SERVICE_DEMAND_START && s.failEnable)){s.error=ERROR_ACCESS_DENIED;return FALSE;}
 s.type=type;return TRUE;
}
BOOL StartServiceW(SC_HANDLE,DWORD,const wchar_t**){s.calls.push_back("start");if(!s.start){if(!s.error)s.error=ERROR_ACCESS_DENIED;return FALSE;}return TRUE;}
namespace GlassInstallLog {template<class... A>void LineFormat(const wchar_t*,A...){} }
namespace GlassInstallHost {
'''
main = r'''
}
using namespace GlassInstallHost;
void reset(){s=Scenario{};}
int main(){
 reset();assert(StartOpenGlassServicePreservingStartType(SERVICE_DISABLED));assert(s.type==SERVICE_DISABLED);
 assert((s.calls==std::vector<std::string>{"enable","start","disable"}));
 reset();s.start=false;assert(!StartOpenGlassServicePreservingStartType(SERVICE_DISABLED));assert(s.type==SERVICE_DISABLED);
 assert((s.calls==std::vector<std::string>{"enable","start","disable"}));
 reset();s.failEnable=true;assert(!StartOpenGlassServicePreservingStartType(SERVICE_DISABLED));assert(s.type==SERVICE_DISABLED);
 assert((s.calls==std::vector<std::string>{"enable"}));
 reset();s.failRestore=true;assert(!StartOpenGlassServicePreservingStartType(SERVICE_DISABLED));assert(s.type==SERVICE_DEMAND_START);
 s.failRestore=false;assert(RestoreOpenGlassServiceStartType(SERVICE_DISABLED));assert(s.type==SERVICE_DISABLED);
 reset();s.start=false;s.error=ERROR_SERVICE_ALREADY_RUNNING;assert(StartOpenGlassServicePreservingStartType(SERVICE_DISABLED));assert(s.type==SERVICE_DISABLED);
 reset();s.type=SERVICE_AUTO_START;assert(StartOpenGlassServicePreservingStartType(SERVICE_AUTO_START));
 assert(s.type==SERVICE_AUTO_START && (s.calls==std::vector<std::string>{"start"}));
 std::cout<<"PASS: real OpenGlass service start: success, failure, disabled preference, rollback repair\n";
}
'''
with tempfile.TemporaryDirectory(prefix="glass-service-test-") as directory:
    cpp = Path(directory) / "service.cpp"
    exe = Path(directory) / "service-test"
    cpp.write_text(prefix + implementation + main, encoding="utf-8")
    compiler = os.environ.get("CXX", "g++")
    subprocess.run([compiler, "-std=c++17", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
