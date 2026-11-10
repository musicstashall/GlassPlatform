#!/usr/bin/env python3
"""Execute the production demo readiness/timer state machine with fake Win32; no Setup run."""
from pathlib import Path
import os, subprocess, tempfile
repo=Path(__file__).resolve().parents[2]
s=(repo/'GlassSetup/src/main.cpp').read_text(encoding='utf-8')
def function(token):
    a=s.index(token); b=s.index('{',a); depth=1; c=b+1
    while depth:
        depth+=(s[c]=='{')-(s[c]=='}'); c+=1
    return s[a:c]
begin=function('void BeginStartup()')
preview=begin[begin.index('if (HasCommandLineSwitch(L"--demo-native"))'):begin.index('#endif')]
assert preview.index('InitializeControls();') < preview.index('MaybeStartDebugDemo();') < preview.index('return;')
assert begin.index(preview) < begin.index('startupThread_=std::thread')
for forbidden in ('StartupWorker(', 'ContinueVersionCheck(', 'StartInstallation(', 'InitializeWebView(', 'StartRuntimeInstallation('):
    assert forbidden not in preview, forbidden
assert 'if (NativeDemo()) return;' in function('void InitializeWebView()')
assert 'debugNativeDemo_=HasCommandLineSwitch(L"--demo-native");' in function('void InitializeControls()')
assert '!NativeDemo()' in function('void RenderContent(')
assert 'Page::Welcome && NativeDemo()' in function('void RenderContent(')
assert 'Page::Installing && !webReady_' in function('void RenderContent(')
assert 'return false;' in function('bool NativeDemo()') # Release cannot enable it.
harness=r'''
#include <cassert>
#include <cstddef>
#include <iostream>
constexpr int BM_SETCHECK=1,BST_UNCHECKED=0,kDebugDemoTimer=4,kDebugDemoPageMs=1800;
int timerMs=0; void SendMessageW(int,int,int,int){} void SetTimer(int,int,int ms,void*){timerMs=ms;}
void KillTimer(int,int){} namespace InstallLog {void Line(const wchar_t*){}}
enum class Page {Welcome,Checking,Conflicts,Downgrade,VersionInvalid,License,Components,Certificate,Installing,ModulesBusy,ProcessesStuck,ConfirmCancel,Cancelling,Cancelled,Launching,Complete,Error};
struct Wizard {
 bool debugDemoPending_=true,debugDemoStarted_=false,debugDemoActive_=false,debugNativeDemo_=false;
 bool webReady_=false,webBridgeReady_=false,accepted_=true,webInitSent_=true;
 size_t debugDemoStep_=0;int accept_=1,window=1,finishes=0,changes=0;Page page=Page::Welcome;
 void SetPage(Page p){page=p;++changes;}void SetDebugDemoPage(Page p){SetPage(p);}
 void FinishDebugDemo(){++finishes;debugDemoActive_=false;}
 // METHODS
};
int main(){
 Wizard web; web.MaybeStartDebugDemo();assert(!web.debugDemoActive_&&timerMs==0);
 web.webReady_=true;web.MaybeStartDebugDemo();assert(!web.debugDemoActive_);
 web.webBridgeReady_=true;web.MaybeStartDebugDemo();assert(web.debugDemoActive_&&timerMs==1800);
 Wizard native; native.debugNativeDemo_=true;native.MaybeStartDebugDemo();
 assert(native.debugDemoActive_&&!native.debugDemoPending_&&!native.accepted_&&!native.webInitSent_&&timerMs==4000);
 for(int i=0;i<34;++i)native.TickDebugDemo();
 assert(native.debugDemoActive_&&native.finishes==0&&native.debugDemoStep_==0&&native.page==Page::Welcome);
 for(int i=0;i<17;++i){web.TickDebugDemo();}
 assert(web.finishes==1);
 std::cout<<"PASS: production native tour starts without WebView, loops in isolation; HTML tour retains bridge readiness gate\n";
}
'''.replace('// METHODS',function('void MaybeStartDebugDemo()')+'\n'+function('void TickDebugDemo()'))
with tempfile.TemporaryDirectory(prefix='glass-native-demo-') as d:
    cpp=Path(d)/'demo.cpp';exe=Path(d)/'demo';cpp.write_text(harness,encoding='utf-8')
    subprocess.run([os.environ.get('CXX','g++'),'-std=c++17','-Wall','-Wextra',str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
