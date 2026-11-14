#!/usr/bin/env python3
"""Exercise exact x86 uninstall root during the Native unlock lifecycle.

The fake process and scheduler APIs do not replace testing Windows unloads.
"""
from pathlib import Path
import os
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[2]
source = (repo / 'GlassSetup/src/InstallSupport/GlassInstallUnlock.cpp').read_text(encoding='utf-8')

def function(token):
    start=source.index(token)
    opening=source.index('{', start)
    depth=0
    for i in range(opening,len(source)):
        if source[i]=='{': depth+=1
        elif source[i]=='}':
            depth-=1
            if not depth:return source[start:i+1]
    raise ValueError(token)

body=function('bool ForceUnloadHolder(')
assert 'TryGetX86InstallRoot' not in body
assert 'SpawnX86ForceUnloadAgent(pid, x86Dir.c_str())' in body
prefix=r'''
#include <cassert>
#include <cwchar>
#include <cwctype>
#include <iostream>
#include <string>
using DWORD=unsigned long;
int _wcsicmp(const wchar_t* a,const wchar_t* b){while(*a&&*b){auto x=std::towlower(*a++),y=std::towlower(*b++);if(x!=y)return x<y?-1:1;}return *a?1:*b?-1:0;}
struct Fake {bool identity=true,protectedPid=false,signal=false,archKnown=true,native64=false,safe=true;
 int direct=1,spawned=0,local=0,helper=0;std::wstring agentRoot,checkedPath,command;} fake;
DWORD GetCurrentProcessId(){return 42;}
void Sleep(unsigned){}
bool IsProtectedProcess(const wchar_t*){return fake.protectedPid;}
bool IsExplorerExeName(const wchar_t* s){return _wcsicmp(s,L"explorer.exe")==0;}
struct ProcessGuard{};
bool OpenMatchingProcess(DWORD,const wchar_t*,ProcessGuard&){return fake.identity;}
void CloseProcessWindows(DWORD){}
bool QueryTargetIsNative64(DWORD,bool& value){value=fake.native64;return fake.archKnown;}
namespace GlassInstallLog {template<class... T>void LineFormat(const wchar_t*,T...) {}}
namespace Shared::InjectProcessMap {bool SignalUnloadRequest(DWORD){return fake.signal;}}
namespace Shared::ForceUnloadGlassModules {
 enum class ForceUnloadResult{Ok,NeedsX86Agent,BlockedHost,Failed};
 ForceUnloadResult ForceUnload(DWORD){return static_cast<ForceUnloadResult>(fake.direct);}
}
namespace Shared::ForceUnloadAgent {
 bool SpawnX86ForceUnloadAgent(DWORD,const wchar_t* root){++fake.spawned;fake.agentRoot=root;return true;}
}
namespace GP {enum class PlatformArch{X64,X86};}
namespace GlassInstallWatch {bool ForceUnloadInLocalNative(DWORD){++fake.local;return true;}}
namespace GlassInstallUtil {
 std::wstring TrimTrailingSlashes(std::wstring s){while(!s.empty()&&(s.back()==L'\\'||s.back()==L'/'))s.pop_back();return s;}
 bool IsSafeInstallPath(const std::wstring& p){fake.checkedPath=p;return fake.safe;}
 std::wstring NativeDllForArch(GP::PlatformArch,const std::wstring&,const std::wstring&){return L"protected\\Native.dll";}
 std::wstring NativeRundll64(){return L"system\\rundll32.exe";}
 bool RunHiddenCommand(const std::wstring& cmd,DWORD* code,DWORD){fake.command=cmd;*code=0;++fake.helper;return true;}
}
namespace GlassInstallUnlock {struct ModuleHolder{DWORD pid;std::wstring exe;};
'''
main=r'''
int main(){using namespace GlassInstallUnlock;auto reset=[](){fake=Fake{};};
 ModuleHolder x{123,L"app.exe"};
 reset();assert(ForceUnloadHolder(x,L"E:\\64",L"D:\\User\\Glass"));
#ifdef GLASS_SETUP_NATIVE_MONITOR
 assert(fake.local==1 && fake.spawned==0);
#else
 assert(fake.spawned==1 && fake.agentRoot==L"D:\\User\\Glass" && fake.checkedPath==L"D:\\User\\Glass\\GlassCompat.dll");
 reset();fake.safe=false;assert(!ForceUnloadHolder(x,L"E:\\64",L"D:\\Glass") && fake.spawned==0);
 reset();assert(!ForceUnloadHolder(x,L"E:\\64",L"") && fake.spawned==0);
 reset();fake.direct=0;assert(ForceUnloadHolder(x,L"E:\\64",L"D:\\Glass") && fake.spawned==0);
#endif
 reset();fake.native64=true;assert(ForceUnloadHolder(x,L"E:\\64",L"D:\\Glass") && fake.helper==1);
 reset();x.exe=L"explorer.exe";assert(!ForceUnloadHolder(x,L"E:\\64",L"D:\\Glass") && fake.spawned==0 && fake.helper==0);
 reset();x.exe=L"app.exe";x.pid=42;assert(!ForceUnloadHolder(x,L"E:\\64",L"D:\\Glass") && fake.spawned==0);
 std::cout<<"PASS: unlock uses selected x86 root or the embedded local Native helper\n";
}
'''
with tempfile.TemporaryDirectory(prefix='glass-unlock-root-') as directory:
    cpp=Path(directory)/'test.cpp';cpp.write_text(prefix+body+'\n}\n'+main,encoding='utf-8')
    for setup in (False,True):
        exe=Path(directory)/('setup' if setup else 'unins')
        cmd=[os.environ.get('CXX','g++'),'-std=c++17','-g','-fsanitize=address,undefined',
             '-fno-omit-frame-pointer','-no-pie',str(cpp),'-o',str(exe)]
        if setup:cmd.insert(1,'-DGLASS_SETUP_NATIVE_MONITOR')
        subprocess.run(cmd,check=True);subprocess.run([str(exe)],check=True)
