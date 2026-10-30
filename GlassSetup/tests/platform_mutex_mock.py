#!/usr/bin/env python3
"""Exercise the uninstall/rollback host stop with fake mutex APIs."""
from pathlib import Path
import os
import subprocess
import tempfile

repo=Path(__file__).resolve().parents[2]
source=(repo / "GlassSetup/src/InstallSupport/GlassInstallHost.cpp").read_text(encoding="utf-8")
unins=(repo / "GlassSetup/GlassUnins/main.cpp").read_text(encoding="utf-8")

def function(token):
    start=source.index(token);brace=source.index("{",start);depth=0
    for i in range(brace,len(source)):
        if source[i]=="{": depth+=1
        if source[i]=="}":
            depth-=1
            if depth==0:return source[start:i+1]
    raise ValueError(token)

query=function("bool QueryGlassPlatformRunning(")
stop=function("bool StopGlassPlatformImpl(")
assert "QueryGlassPlatformRunning(GP::PlatformArch::X64,prior64)" in unins
prefix=r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
using DWORD=std::uint32_t;using ULONGLONG=std::uint64_t;using HANDLE=void*;
constexpr int SYNCHRONIZE=1,FALSE=0;constexpr DWORD ERROR_FILE_NOT_FOUND=2,ERROR_ACCESS_DENIED=5;
struct Fake{bool running=false,denied=false,denyAfterSignal=false;DWORD error=ERROR_FILE_NOT_FOUND;
 int queries=0,signals=0,orphans=0;ULONGLONG now=0,stopAt=500;} fake;
namespace GP {enum class PlatformArch{X64,X86};const wchar_t* InstanceMutexName(PlatformArch){return L"mutex";}
 bool SignalPlatformStop(PlatformArch){++fake.signals;return true;}
 bool StopOrphanAutorunTaskInstance(PlatformArch){++fake.orphans;return true;}}
HANDLE OpenMutexW(int,int,const wchar_t*){++fake.queries;
 if(fake.denied || (fake.denyAfterSignal && fake.signals)){fake.error=ERROR_ACCESS_DENIED;return nullptr;}
 if(fake.running && fake.signals && fake.now>=fake.stopAt)fake.running=false;
 if(fake.running)return reinterpret_cast<HANDLE>(1);fake.error=ERROR_FILE_NOT_FOUND;return nullptr;}
DWORD GetLastError(){return fake.error;}void CloseHandle(HANDLE){}
ULONGLONG GetTickCount64(){return fake.now;}void Sleep(DWORD ms){fake.now+=ms;}
namespace GlassInstallLog {template<class... T>void LineFormat(const wchar_t*,T...) {}}
namespace GlassInstallHost {bool QueryGlassPlatformRunning(GP::PlatformArch,bool&) noexcept;}
'''
main=r'''
int main(){auto reset=[](){fake=Fake{};};bool running=false;
 reset();fake.denied=true;assert(!GlassInstallHost::QueryGlassPlatformRunning(GP::PlatformArch::X86,running));
 assert(!StopGlassPlatformImpl(GP::PlatformArch::X86) && fake.signals==0 && fake.orphans==0);
 reset();assert(StopGlassPlatformImpl(GP::PlatformArch::X86) && fake.signals==0 && fake.orphans==1 && fake.now==0);
 reset();fake.running=true;assert(StopGlassPlatformImpl(GP::PlatformArch::X64) && fake.signals==1 && fake.now>=500);
 reset();fake.running=true;fake.denyAfterSignal=true;assert(!StopGlassPlatformImpl(GP::PlatformArch::X64));
 assert(fake.signals==1 && fake.orphans==0);
 reset();fake.running=true;fake.stopAt=999999;assert(!StopGlassPlatformImpl(GP::PlatformArch::X86));
 assert(fake.signals==1 && fake.now>=45000 && fake.orphans==0);
 std::cout<<"PASS: real host stop distinguishes inaccessible mutexes from stopped platforms\n";
}
'''
with tempfile.TemporaryDirectory(prefix="glass-platform-mutex-") as directory:
    cpp=Path(directory)/"test.cpp";exe=Path(directory)/"test"
    cpp.write_text(prefix+"namespace GlassInstallHost {\n"+query+"\n}\n"+stop+"\n"+main,encoding="utf-8")
    subprocess.run([os.environ.get("CXX","g++"),"-std=c++17","-g","-fsanitize=address,undefined","-fno-omit-frame-pointer","-no-pie",str(cpp),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
