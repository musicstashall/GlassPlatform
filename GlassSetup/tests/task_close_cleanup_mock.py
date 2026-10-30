#!/usr/bin/env python3
"""Exercise actual host close cleanup using fake hook and package-debug APIs."""
from pathlib import Path
import os
import subprocess
import tempfile

repo=Path(__file__).resolve().parents[2]
src=(repo/'GlassCompat/src/CoreWindowHostInject.cpp').read_text(encoding='utf-8')
entry=(repo/'GlassCompat/src/EntryRundll.cpp').read_text(encoding='utf-8')
def function(token):
    start=src.index(token);brace=src.index('{',start);depth=0
    for i in range(brace,len(src)):
        if src[i]=='{':depth+=1
        if src[i]=='}':
            depth-=1
            if depth==0:return src[start:i+1]
    raise ValueError(token)
shutdown=function('void Shutdown(bool wakeProcesses)')
finish=function('void FinishTaskStop()')
for token in ('bool TrySehPlmWake(', 'void TrySehPlmRest('):
    assert 'needUninit = SUCCEEDED(hrInit)' in function(token)
for unsafe in ('ForceUnloadResiduals','WakeRemoteCwProcess','SignalRemoteUnload','CreateRemoteThread'):
    assert unsafe not in finish
assert 'Shutdown(!s_stopFromCloseMessage)' in entry
assert 'if (hadAnyHook && !s_stopFromCloseMessage)' in entry
branch=entry[entry.index('if (s_stopFromCloseMessage)',entry.index('Shutdown(!s_stopFromCloseMessage)')):]
assert branch.index('FinishTaskStop()')<branch.index('ForceUnloadResiduals()')
assert 'case WM_CLOSE:' in entry and 'RequestHostStopFromWindowMessage();' in entry
message_loop=entry[entry.index('while (PeekMessageW(&msg'):entry.index('s_themePolicyDueMs != 0',entry.index('while (PeekMessageW(&msg'))]
after_dispatch=message_loop[message_loop.index('DispatchMessageW(&msg)'):]
assert after_dispatch.index('if (s_stopFromCloseMessage)') < after_dispatch.index('goto done;')
prefix=r'''
#include <cassert>
#include <cstdint>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>
using DWORD=std::uint32_t;using HRESULT=int;using HWINEVENTHOOK=void*;
constexpr HRESULT S_OK=0,S_FALSE=1;constexpr int COINIT_MULTITHREADED=0;
#define SUCCEEDED(hr) ((hr)>=0)
struct ThreadHook{DWORD pid=0;HWINEVENTHOOK hook=nullptr,uncloakHook=nullptr,locationHook=nullptr;};
std::mutex g_mx;std::map<int,ThreadHook> g_hooks;std::set<DWORD> g_bootstrappedPids,g_remoteLoadPids,g_remoteGuiTids,g_remoteCwHwnds,g_attachExistingPosted;
std::set<std::wstring> g_sehPlmDebugPkgs;
struct Fake{int unhooks=0,wakes=0,posts=0,sleeps=0,inits=0,uninits=0,releases=0,closeEvents=0;
 bool settings=true;HRESULT init=S_OK;std::vector<std::wstring> disabled;} f;
void UnhookWinEvent(HWINEVENTHOOK){++f.unhooks;}void WakeRemoteCwProcess(DWORD){++f.wakes;}
void PostWmNullToTrackedHwnds(DWORD){++f.posts;}void Sleep(DWORD){++f.sleeps;}
HRESULT CoInitializeEx(void*,int){++f.inits;return f.init;}void CoUninitialize(){++f.uninits;}
struct IPackageDebugSettings{void DisableDebugging(const wchar_t* p){f.disabled.push_back(p);}void Release(){++f.releases;}};
IPackageDebugSettings* CreatePackageDebugSettings(){static IPackageDebugSettings s;return f.settings?&s:nullptr;}
void CloseCwForceEvents(){++f.closeEvents;}
'''
main=r'''
int main(){
 auto hook=[](int n){return reinterpret_cast<HWINEVENTHOOK>(static_cast<std::uintptr_t>(n));};
 g_hooks[1]={42,hook(1),hook(2),hook(3)};g_bootstrappedPids.insert(42);
 g_remoteLoadPids.insert(42);g_remoteGuiTids.insert(6);g_remoteCwHwnds.insert(7);g_attachExistingPosted.insert(8);
 g_sehPlmDebugPkgs={L"pkg-one",L"pkg-two"};
 Shutdown(false);assert(f.unhooks==3 && f.wakes==0 && f.posts==0 && f.sleeps==0);
 assert(g_hooks.empty() && g_bootstrappedPids.empty());
 FinishTaskStop();assert(f.disabled.size()==2 && f.inits==1 && f.uninits==1 && f.releases==1 && f.closeEvents==1);
 assert(g_remoteLoadPids.empty() && g_remoteGuiTids.empty() && g_remoteCwHwnds.empty() && g_attachExistingPosted.empty() && g_sehPlmDebugPkgs.empty());
 f=Fake{};g_hooks[1]={42,hook(1),nullptr,nullptr};Shutdown(true);
 assert(f.unhooks==1 && f.wakes==1 && f.posts==1 && f.sleeps==1); // legacy event path preserved
 f=Fake{};f.init=S_FALSE;g_sehPlmDebugPkgs={L"pkg"};FinishTaskStop();assert(f.inits==1 && f.uninits==1);
 f=Fake{};f.init=-1;f.settings=false;g_sehPlmDebugPkgs={L"pkg"};FinishTaskStop();assert(f.inits==1 && f.uninits==0 && f.closeEvents==1);
 std::cout<<"PASS: real host-close cleanup avoids remote wake/forced unload, balances PLM and preserves legacy path\n";
}
'''
with tempfile.TemporaryDirectory(prefix='glass-task-close-') as d:
    cpp=Path(d)/'test.cpp';exe=Path(d)/'test';cpp.write_text(prefix+shutdown+'\n'+finish+'\n'+main,encoding='utf-8')
    subprocess.run([os.environ.get('CXX','g++'),'-std=c++17','-g','-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer','-no-pie',str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
