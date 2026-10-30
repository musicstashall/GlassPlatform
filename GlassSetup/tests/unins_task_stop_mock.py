#!/usr/bin/env python3
"""Exercise the actual Unins stop ordering and prior-state guard using stubs."""
from pathlib import Path
import os,subprocess,tempfile
repo=Path(__file__).resolve().parents[2]
src=(repo/'GlassSetup/src/InstallSupport/GlassInstallHost.cpp').read_text(encoding='utf-8')
unins=(repo/'GlassSetup/GlassUnins/main.cpp').read_text(encoding='utf-8')
def function(text,token):
    start=text.index(token);brace=text.index('{',start);depth=0
    for i in range(brace,len(text)):
        if text[i]=='{':depth+=1
        if text[i]=='}':
            depth-=1
            if not depth:return text[start:i+1]
    raise ValueError(token)
impl=function(src,'bool StopAllForUninstall(')
worker=function(unins,'int RunOperation(')
assert worker.index('StopAllForUninstall(')<worker.index('EnsureModulesReleased()')<worker.index('Msi::UninstallProduct(')
assert 'stopAttempted64' in worker and 'WaitForPlatformStopped(arch,30000)' in worker
assert 'if (!restoreRemaining())' in worker
prefix=r'''
#include <atomic>
#include <cassert>
#include <iostream>
#include <string>
#include <vector>
namespace GP{enum class PlatformArch{X64,X86};}
struct Fake{std::vector<int> order;bool ok64=true,ok86=true,okOg=true,cancelOnClose=false,cancelOn64=false;
 bool allow64=false,allow86=false;std::wstring root64,root86;} f;
namespace GlassInstallUtil{void CloseGlassSettingsProcesses(const std::atomic_bool* c){f.order.push_back(0);
 if(f.cancelOnClose)const_cast<std::atomic_bool*>(c)->store(true);}}
bool StopGlassPlatformImpl(GP::PlatformArch a,const std::wstring& root,const std::atomic_bool* c,
 bool* attempted,std::wstring*,bool allow){const bool x64=a==GP::PlatformArch::X64;f.order.push_back(x64?1:2);
 if(x64){f.allow64=allow;f.root64=root;}else{f.allow86=allow;f.root86=root;}
 if(attempted)*attempted=allow;if(x64 && f.cancelOn64)const_cast<std::atomic_bool*>(c)->store(true);
 return x64?f.ok64:f.ok86;}
namespace GlassInstallHost {bool StopOpenGlassHost(const std::atomic_bool*){f.order.push_back(3);return f.okOg;}
'''
main=r'''
}
int main(){std::atomic_bool cancel{false};bool attempted64=true,attempted86=true,prior64=false,prior86=true;
 auto reset=[&](){f=Fake{};cancel=false;attempted64=true;attempted86=true;};
 auto run=[&](bool a=true,bool b=true,bool og=true){return GlassInstallHost::StopAllForUninstall(
  L"root64",L"root86",a,b,og,&cancel,&attempted64,&attempted86,&prior64,&prior86);};
 reset();assert(run());assert((f.order==std::vector<int>{0,1,2,3}));
 assert(!f.allow64 && f.allow86 && f.root64==L"root64" && f.root86==L"root86" && !attempted64 && attempted86);
 reset();f.cancelOnClose=true;assert(!run() && (f.order==std::vector<int>{0}) && !attempted64 && !attempted86);
 reset();f.cancelOn64=true;assert(!run() && (f.order==std::vector<int>{0,1}));
 reset();f.ok64=false;assert(!run() && (f.order==std::vector<int>{0,1}));
 reset();f.ok86=false;assert(!run() && (f.order==std::vector<int>{0,1,2}));
 reset();assert(run(false,false,true) && (f.order==std::vector<int>{0,3}) && !attempted64 && !attempted86);
 std::cout<<"PASS: actual Unins stop ordering, cancellation and per-architecture prior-state guard\n";
}
'''
with tempfile.TemporaryDirectory(prefix='glass-unins-stop-') as d:
    cpp=Path(d)/'test.cpp';exe=Path(d)/'test';cpp.write_text(prefix+impl+main,encoding='utf-8')
    subprocess.run([os.environ.get('CXX','g++'),'-std=c++17','-g','-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer','-no-pie',str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
