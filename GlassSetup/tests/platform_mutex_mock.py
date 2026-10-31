#!/usr/bin/env python3
"""Exercise the real Setup/Unins host-stop loop without Windows or Task Scheduler."""
from pathlib import Path
import os
import subprocess
import tempfile

repo=Path(__file__).resolve().parents[2]
source=(repo / 'GlassSetup/src/InstallSupport/GlassInstallHost.cpp').read_text(encoding='utf-8')
unins=(repo / 'GlassSetup/GlassUnins/main.cpp').read_text(encoding='utf-8')

def function(token):
    start=source.index(token); brace=source.index('{',start); depth=0
    for i in range(brace,len(source)):
        if source[i]=='{': depth+=1
        if source[i]=='}':
            depth-=1
            if depth==0:return source[start:i+1]
    raise ValueError(token)

query=function('bool QueryGlassPlatformRunning(')
stop=function('bool StopGlassPlatformImpl(')
assert 'QueryGlassPlatformRunning(GP::PlatformArch::X64,prior64)' in unins
assert 'SignalPlatformStop' not in stop and 'StopOrphanAutorunTaskInstance' not in stop
prefix=r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
using DWORD=std::uint32_t;using ULONGLONG=std::uint64_t;using HANDLE=void*;
constexpr int SYNCHRONIZE=1,FALSE=0,LANG_RUSSIAN=25;
constexpr DWORD ERROR_SUCCESS=0,ERROR_FILE_NOT_FOUND=2,ERROR_ACCESS_DENIED=5,
 ERROR_CANCELLED=1223,ERROR_TIMEOUT=1460,ERROR_INVALID_STATE=5023;
#define PRIMARYLANGID(x) (x)
struct Fake{bool running=false,denied=false,denyAfterRequest=false,validate=true,accepted=true,cancelOnRequest=false;
 DWORD error=ERROR_FILE_NOT_FOUND;int queries=0,requests=0;ULONGLONG now=0,stopAt=500;std::wstring root;} fake;
namespace GP {enum class PlatformArch{X64,X86};const wchar_t* InstanceMutexName(PlatformArch){return L"mutex";}
 const wchar_t* GlassAutorunTaskName(PlatformArch){return L"fixed-task";}}
HANDLE OpenMutexW(int,int,const wchar_t*){++fake.queries;
 if(fake.denied || (fake.denyAfterRequest && fake.requests)){fake.error=ERROR_ACCESS_DENIED;return nullptr;}
 if(fake.running && fake.requests && fake.now>=fake.stopAt)fake.running=false;
 if(fake.running)return reinterpret_cast<HANDLE>(1);fake.error=ERROR_FILE_NOT_FOUND;return nullptr;}
DWORD GetLastError(){return fake.error;}void SetLastError(DWORD e){fake.error=e;}void CloseHandle(HANDLE){}
ULONGLONG GetTickCount64(){return fake.now;}void Sleep(DWORD ms){fake.now+=ms;}int GetUserDefaultUILanguage(){return 0;}
namespace GlassInstallLog {template<class... T>void LineFormat(const wchar_t*,T...) {}}
namespace GlassInstallHost {bool QueryGlassPlatformRunning(GP::PlatformArch,bool&) noexcept;}
namespace GlassInstallRemoval {
 bool StopValidatedPlatformTask(GP::PlatformArch,const std::wstring& root,const std::atomic_bool* cancel,
  bool& attempted,bool,std::wstring& detail){fake.root=root;attempted=false;
  if(!fake.validate){detail=L"task changed";return false;}
  ++fake.requests;attempted=true;if(fake.cancelOnRequest)const_cast<std::atomic_bool*>(cancel)->store(true);
  if(!fake.accepted){fake.error=ERROR_ACCESS_DENIED;detail=L"request rejected";return false;}return true;
 }
}
'''
main=r'''
int main(){auto reset=[](){fake=Fake{};};bool running=false,attempted=false;std::wstring detail;
 auto stop=[&](std::atomic_bool* cancel=nullptr,bool allow=true){return StopGlassPlatformImpl(
  GP::PlatformArch::X64,L"D:\\Root",cancel,&attempted,&detail,allow);};
 reset();fake.denied=true;assert(!GlassInstallHost::QueryGlassPlatformRunning(GP::PlatformArch::X86,running));
 assert(!stop() && fake.requests==0 && !attempted);
 reset();assert(stop() && fake.requests==0 && fake.now==0 && !attempted);
 reset();fake.running=true;assert(stop() && fake.requests==1 && attempted && fake.root==L"D:\\Root" && fake.now==500);
 reset();fake.running=true;assert(!stop(nullptr,false) && !attempted && fake.requests==0 && fake.error==ERROR_INVALID_STATE);
 reset();fake.running=true;fake.validate=false;assert(!stop() && !attempted && fake.requests==0 && detail==L"task changed");
 reset();fake.running=true;fake.denyAfterRequest=true;assert(!stop() && attempted && fake.requests==1);
 reset();fake.running=true;fake.stopAt=999999;assert(!stop() && attempted && fake.now>=45000 && fake.error==ERROR_TIMEOUT);
 reset();fake.running=true;fake.accepted=false;assert(!stop() && attempted && fake.now==500 && fake.error==ERROR_ACCESS_DENIED);
 reset();fake.running=true;std::atomic_bool cancelled{true};assert(!stop(&cancelled) && !attempted && fake.requests==0);
 reset();fake.running=true;fake.cancelOnRequest=true;cancelled=false;
 assert(!stop(&cancelled) && attempted && fake.now==500 && fake.error==ERROR_CANCELLED);
 std::cout<<"PASS: actual Scheduler-stop loop: absent, denied, validation, S_FALSE, settle, timeout and cancellation\n";
}
'''
with tempfile.TemporaryDirectory(prefix='glass-platform-mutex-') as directory:
    cpp=Path(directory)/'test.cpp';exe=Path(directory)/'test'
    cpp.write_text(prefix+'namespace GlassInstallHost {\n'+query+'\n}\n'+stop+'\n'+main,encoding='utf-8')
    subprocess.run([os.environ.get('CXX','g++'),'-std=c++17','-g','-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer','-no-pie',str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
