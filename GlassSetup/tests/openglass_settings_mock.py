#!/usr/bin/env python3
"""Run production Settings SCM commands/state mapping against an in-memory Win32 model."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
native = (root / 'GlassSettings/Native/PlatformPresenceNative.cpp').read_text(encoding='utf-8')
state = (root / 'GlassShared/src/OpenGlassHostState.cpp').read_text(encoding='utf-8')
def between(text, first, last):
    return text[text.index(first):text.index(last, text.index(first))]
orchestration = between(state, '    HostUiState QueryOrchestrationUiState()', '    OpenGlassBackend QueryActiveBackend()')
query = between(state, '        [[nodiscard]] HostUiState QueryServiceUiState()', '        [[nodiscard]] bool IsProcessNamedRunning(')
stop_wait = between(native, 'namespace\n{', 'extern "C"')
stop = between(native, '    __declspec(dllexport) BOOL WINAPI GlassSettings_StopOpenGlassService()', '    __declspec(dllexport) void WINAPI GlassSettings_RequestPlatformPresenceRefresh()')
start = between(native, '    __declspec(dllexport) BOOL WINAPI GlassSettings_StartOpenGlassService()', '    __declspec(dllexport) BOOL WINAPI GlassSettings_StopOpenGlassService()')
remove = native[native.index('    __declspec(dllexport) BOOL WINAPI GlassSettings_RemoveOpenGlassService()'):].rsplit('}', 1)[0]
prefix = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iostream>
using DWORD=std::uint32_t; using ULONGLONG=std::uint64_t; using BYTE=unsigned char;
using LPBYTE=BYTE*; using SC_HANDLE=void*; using BOOL=int;
#define __declspec(x)
#define WINAPI
constexpr BOOL TRUE=1,FALSE=0;
constexpr DWORD SC_MANAGER_CONNECT=1,SERVICE_START=2,SERVICE_STOP=4,SERVICE_QUERY_CONFIG=8,
 SERVICE_CHANGE_CONFIG=16,SERVICE_QUERY_STATUS=32,DELETE=64,HEAP_ZERO_MEMORY=0,
 SERVICE_STOPPED=1,SERVICE_START_PENDING=2,SERVICE_STOP_PENDING=3,SERVICE_RUNNING=4,
 SERVICE_DISABLED=4,SERVICE_DEMAND_START=3,SERVICE_AUTO_START=2,SERVICE_NO_CHANGE=0xffffffff,
 SERVICE_WIN32_OWN_PROCESS=16,SC_STATUS_PROCESS_INFO=0,SERVICE_CONTROL_STOP=1,
 ERROR_SUCCESS=0,ERROR_ACCESS_DENIED=5,ERROR_SERVICE_DOES_NOT_EXIST=1060,
 ERROR_SERVICE_MARKED_FOR_DELETE=1072,ERROR_SERVICE_NOT_ACTIVE=1062,ERROR_SERVICE_ALREADY_RUNNING=1056;
struct QUERY_SERVICE_CONFIGW { DWORD dwStartType; };
struct SERVICE_STATUS_PROCESS { DWORD dwServiceType=0,dwCurrentState=0; };
struct SERVICE_STATUS {};
struct Scenario {
 bool exists=true,denied=false,configFail=false,statusFail=false,heapFail=false,startFail=false,
 enableFail=false,restoreFail=false,stopFail=false,deleteFail=false,neverStop=false,externalHandle=false,marked=false;
 DWORD state=SERVICE_STOPPED,type=SERVICE_DISABLED,error=0;
 int handles=0,deleteCalls=0,stopCalls=0,changes=0,polls=0,taskQueries=0,taskState=3; bool uiThread=false; ULONGLONG now=0;
} s;
DWORD GetLastError(){return s.error;}
SC_HANDLE OpenSCManagerW(void*,void*,DWORD){if(s.denied){s.error=ERROR_ACCESS_DENIED;return nullptr;}return reinterpret_cast<void*>(1);}
SC_HANDLE OpenServiceW(SC_HANDLE,const wchar_t*,DWORD){
 if(!s.exists){s.error=ERROR_SERVICE_DOES_NOT_EXIST;return nullptr;}
 if(s.marked){++s.polls;if(!s.externalHandle && s.handles==0){s.exists=false;s.error=ERROR_SERVICE_DOES_NOT_EXIST;}
 else {s.error=ERROR_SERVICE_MARKED_FOR_DELETE;} return nullptr;}
 ++s.handles;return reinterpret_cast<void*>(2);
}
BOOL CloseServiceHandle(SC_HANDLE h){if(h==reinterpret_cast<void*>(2)){--s.handles;assert(s.handles>=0);}s.error=999;return TRUE;}
void* GetProcessHeap(){return nullptr;}
void* HeapAlloc(void*,DWORD,DWORD bytes){return s.heapFail?nullptr:std::calloc(1,bytes);}
BOOL HeapFree(void*,DWORD,void* p){std::free(p);return TRUE;}
BOOL QueryServiceConfigW(SC_HANDLE,QUERY_SERVICE_CONFIGW* cfg,DWORD,DWORD* needed){
 *needed=sizeof(QUERY_SERVICE_CONFIGW);if(!cfg || s.configFail)return FALSE;cfg->dwStartType=s.type;return TRUE;
}
BOOL QueryServiceStatusEx(SC_HANDLE,int,LPBYTE data,DWORD,DWORD*){
 if(s.statusFail)return FALSE;
 if(s.state==SERVICE_STOP_PENDING && !s.neverStop)s.state=SERVICE_STOPPED;
 reinterpret_cast<SERVICE_STATUS_PROCESS*>(data)->dwCurrentState=s.state;return TRUE;
}
BOOL ChangeServiceConfigW(SC_HANDLE,DWORD,DWORD type,DWORD,const wchar_t*,const wchar_t*,DWORD*,const wchar_t*,const wchar_t*,const wchar_t*,const wchar_t*){
 ++s.changes;if((type==SERVICE_DEMAND_START && s.enableFail) || (type==SERVICE_DISABLED && s.restoreFail))return FALSE;
 s.type=type;return TRUE;
}
BOOL StartServiceW(SC_HANDLE,DWORD,void*){
 if(s.state==SERVICE_RUNNING){s.error=ERROR_SERVICE_ALREADY_RUNNING;return FALSE;}
 if(s.startFail){s.error=ERROR_ACCESS_DENIED;return FALSE;}assert(s.type!=SERVICE_DISABLED);s.state=SERVICE_RUNNING;return TRUE;
}
BOOL ControlService(SC_HANDLE,DWORD,SERVICE_STATUS*){
 ++s.stopCalls;if(s.stopFail){s.error=ERROR_ACCESS_DENIED;return FALSE;}s.state=SERVICE_STOP_PENDING;return TRUE;
}
BOOL DeleteService(SC_HANDLE){++s.deleteCalls;assert(s.state==SERVICE_STOPPED);
 if(s.deleteFail){s.error=ERROR_ACCESS_DENIED;return FALSE;}s.marked=true;return TRUE;
}
ULONGLONG GetTickCount64(){return s.now;}void Sleep(DWORD n){s.now+=n;}
namespace GlassShared { bool IsSchedulerUiThread(){return s.uiThread;} }
namespace Shared::OpenGlassHostState {
 const wchar_t kOpenGlassHostServiceName[]=L"OpenGlassHost";
 enum class HostUiState {NoTask,Disabled,Ready,Running};
 struct Entry {HostUiState state=HostUiState::NoTask; ULONGLONG timeMs=0;};
 struct Cache {Entry entry; Entry load(std::memory_order){return entry;} void store(Entry value,std::memory_order){entry=value;}} g_orchestrationCache;
 constexpr ULONGLONG kCacheTtlMs=500;
 void InvalidateCache(){g_orchestrationCache.entry={};}
 HostUiState QueryTaskUiState(){++s.taskQueries;return static_cast<HostUiState>(s.taskState);}
'''
main = r'''
void reset(){assert(s.handles==0);s=Scenario{};Shared::OpenGlassHostState::InvalidateCache();}
int main(){
 using namespace Shared::OpenGlassHostState;
 // A leftover running task must not override the selected service backend,
 // on either the UI thread or the background presence watcher.
 for(bool ui : {false,true}){
  reset();s.uiThread=ui;s.taskState=3;assert(QueryOrchestrationUiState()==HostUiState::Disabled);assert(s.taskQueries==0);
  s.state=SERVICE_RUNNING;assert(QueryOrchestrationUiState()==HostUiState::Running);assert(s.taskQueries==0);
 }
 reset();s.exists=false;assert(QueryOrchestrationUiState()==HostUiState::Running);assert(s.taskQueries==1);
 reset();s.exists=false;s.uiThread=true;assert(QueryOrchestrationUiState()==HostUiState::NoTask);assert(s.taskQueries==0);
 for(DWORD type : {SERVICE_DISABLED,SERVICE_DEMAND_START,SERVICE_AUTO_START}){
  reset();s.type=type;s.state=SERVICE_RUNNING;assert(QueryServiceUiState()==HostUiState::Running);
  s.state=SERVICE_STOPPED;assert(QueryServiceUiState()==(type==SERVICE_DISABLED?HostUiState::Disabled:HostUiState::Ready));
  s.state=SERVICE_START_PENDING;assert(QueryServiceUiState()==HostUiState::Ready);
  s.state=SERVICE_STOP_PENDING;s.neverStop=true;assert(QueryServiceUiState()==HostUiState::Ready);
 }
 reset();assert(GlassSettings_StartOpenGlassService());assert(s.type==SERVICE_DISABLED && s.state==SERVICE_RUNNING && s.changes==2);
 assert(QueryServiceUiState()==HostUiState::Running);
 reset();s.state=SERVICE_RUNNING;assert(GlassSettings_StartOpenGlassService()); // last-error survives cleanup
 reset();s.startFail=true;assert(!GlassSettings_StartOpenGlassService());assert(s.type==SERVICE_DISABLED && s.changes==2);
 reset();s.enableFail=true;assert(!GlassSettings_StartOpenGlassService());assert(s.state==SERVICE_STOPPED && s.type==SERVICE_DISABLED);
 reset();s.restoreFail=true;assert(!GlassSettings_StartOpenGlassService());
 for(DWORD type : {SERVICE_DEMAND_START,SERVICE_AUTO_START}){reset();s.type=type;assert(GlassSettings_StartOpenGlassService());assert(s.type==type && s.changes==0);}
 reset();s.configFail=true;assert(!GlassSettings_StartOpenGlassService());assert(s.changes==0);
 reset();s.heapFail=true;assert(!GlassSettings_StartOpenGlassService());
 reset();s.state=SERVICE_RUNNING;assert(GlassSettings_StopOpenGlassService());assert(s.exists && s.state==SERVICE_STOPPED && s.deleteCalls==0 && s.type==SERVICE_DISABLED);
 reset();assert(GlassSettings_StopOpenGlassService());assert(s.stopCalls==0 && s.exists);
 reset();s.state=SERVICE_RUNNING;s.stopFail=true;assert(!GlassSettings_StopOpenGlassService());assert(s.exists && s.deleteCalls==0);
 reset();s.state=SERVICE_RUNNING;assert(GlassSettings_RemoveOpenGlassService());assert(!s.exists && s.deleteCalls==1 && s.stopCalls==1 && s.handles==0);
 reset();assert(GlassSettings_RemoveOpenGlassService());assert(!s.exists && s.stopCalls==0);
 reset();s.exists=false;assert(GlassSettings_RemoveOpenGlassService());assert(s.deleteCalls==0);
 reset();s.state=SERVICE_STOP_PENDING;assert(GlassSettings_RemoveOpenGlassService());assert(s.stopCalls==0);
 reset();s.state=SERVICE_RUNNING;s.stopFail=true;assert(!GlassSettings_RemoveOpenGlassService());assert(s.deleteCalls==0 && s.exists);
 reset();s.state=SERVICE_RUNNING;s.neverStop=true;assert(!GlassSettings_RemoveOpenGlassService());assert(s.deleteCalls==0 && s.now==30000);
 reset();s.deleteFail=true;assert(!GlassSettings_RemoveOpenGlassService());assert(s.exists);
 reset();s.externalHandle=true;assert(!GlassSettings_RemoveOpenGlassService());assert(s.marked && s.exists && s.handles==0 && s.now==30000);
 s.externalHandle=false;assert(GlassSettings_RemoveOpenGlassService());assert(!s.exists);
 reset();s.statusFail=true;assert(!GlassSettings_RemoveOpenGlassService());assert(s.deleteCalls==0);
 reset();s.denied=true;assert(!GlassSettings_RemoveOpenGlassService());assert(!GlassSettings_StartOpenGlassService());
 assert(s.handles==0);std::cout<<"Settings SCM state/start/delete mocks PASS\n";
}
'''
with tempfile.TemporaryDirectory(prefix='glass-og-settings-') as temp:
    cpp = Path(temp) / 'test.cpp'; exe = Path(temp) / 'test'
    cpp.write_text(prefix + query + orchestration + '}\n' + stop_wait + start + stop + remove + main, encoding='utf-8')
    subprocess.run([os.environ.get('CXX', 'g++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

# UI uses orchestration presence, not legacy-task bits alone; start success is SCM,
# not the independently initialized DWM/symbol pipeline. No Windows runtime here.
home = (root / 'GlassSettings/Pages/HomePage.xaml.cs').read_text(encoding='utf-8')
presence = (root / 'GlassSettings/Services/PlatformPresenceService.cs').read_text(encoding='utf-8')
actions = (root / 'GlassSettings/Core/Platform/SystemPlatformRunActions.cs').read_text(encoding='utf-8')
assert 'presence.OpenGlassOrchestration != (int)NativeHostInterop.NativeOpenGlassHostUiState.NoTask' in home
assert 'snap.OpenGlassOrchestration != (int)NativeHostInterop.NativeOpenGlassHostUiState.NoTask' in presence
assert 'TaskExists: NativeHostInterop.OpenGlassOrchestrationInstalled()' in presence
assert 'OpenGlassHostService.LegacyHostTaskExists()' not in home
assert 'OpenGlassHostService.QueryOpenGlassHostUiState() == OpenGlassHostUiState.Running;' in actions
assert 'return ok && OpenGlassHostService.OrchestrationInstalled();' in actions
assert 'TotalMilliseconds < 2000' not in between(home, '    private void RefreshSystemCard()', '    private static void RefreshPlatformToggle(')
print('Settings service-only UI wiring guards PASS')
