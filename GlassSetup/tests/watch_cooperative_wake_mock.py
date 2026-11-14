"""Actual holder wake and shared WM_NULL implementation; no real process operations."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
watch=(root/'GlassSetup/src/InstallSupport/GlassInstallWatch.cpp').read_text()
stop=(root/'GlassShared/src/GlassStop.cpp').read_text()
def fn(text,token):
 a=text.index(token);b=text.index('{',a);d=0
 for i in range(b,len(text)):
  d+=(text[i]=='{')-(text[i]=='}')
  if not d:return text[a:i+1]
 raise ValueError(token)
h=r'''
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
#include <iostream>
using DWORD=unsigned;using BOOL=int;using HWND=unsigned;using HANDLE=int;using LPARAM=intptr_t;
constexpr int TRUE=1,WM_NULL=0,TH32CS_SNAPTHREAD=4,INVALID_HANDLE_VALUE=-1;
struct THREADENTRY32 {DWORD dwSize=0,th32OwnerProcessID=0,th32ThreadID=0;};
std::vector<DWORD> messages,signals;int closes=0;unsigned threadIndex=0;bool snapshotOk=true;
template<class F>bool EnumWindows(F f,LPARAM arg){for(unsigned pid:{10u,20u,99u})f(pid,arg);return true;}
void GetWindowThreadProcessId(HWND hwnd,DWORD* pid){*pid=hwnd;}
bool PostMessageW(HWND hwnd,int message,int,int){assert(message==WM_NULL);messages.push_back(hwnd);return true;}
HANDLE CreateToolhelp32Snapshot(int,int){return snapshotOk?1:-1;}
void fill(THREADENTRY32* entry){entry->th32OwnerProcessID=threadIndex==0?10:threadIndex==1?20:99;entry->th32ThreadID=entry->th32OwnerProcessID;}
bool Thread32First(HANDLE,THREADENTRY32* entry){threadIndex=0;fill(entry);return true;}
bool Thread32Next(HANDLE,THREADENTRY32* entry){if(++threadIndex==3)return false;fill(entry);return true;}
bool PostThreadMessageW(DWORD tid,int message,int,int){assert(message==WM_NULL);messages.push_back(tid);return true;}
bool CloseHandle(HANDLE){++closes;return true;}
namespace Shared::GlassStop {
// NUDGE
// SHARED
}
namespace Shared::InjectProcessMap {bool SignalUnloadRequest(DWORD pid){signals.push_back(pid);return true;}}
namespace GlassInstallLog {template<class... T>void LineFormat(const wchar_t*,T...) {}}
struct ModuleHolder {DWORD pid;std::wstring exe,modules;};
// WATCH
int main(){
 assert(Shared::GlassStop::WakeProcessQueues(0)==0&&messages.empty());
 assert(Shared::GlassStop::WakeProcessQueues(10)==2&&messages==std::vector<DWORD>({10,10}));
 messages.clear();WakeHolderQueues({{10,L"GlassSetup.exe",L"GlassCompat.dll"},{20,L"setup.tmp",L"UICompat.dll"},{0,L"",L""}});
 assert(signals==std::vector<DWORD>({10,20})&&messages==std::vector<DWORD>({10,10,20,20}));
 snapshotOk=false;assert(Shared::GlassStop::WakeProcessQueues(20)==1); // Window wake survives thread snapshot failure.
 std::cout<<"PASS: actual cooperative holder wake: scanned PIDs only, own Setup/tmp included, bitness-neutral queue messages, no force unload/close/termination, snapshot failure safe\n";
}
'''.replace('// NUDGE',fn(stop,'void NudgeProcess(')).replace('// SHARED',fn(stop,'unsigned WakeProcessQueues(')).replace('// WATCH',fn(watch,'void WakeHolderQueues('))
loop=watch[watch.index('bool WaitUntilClearOrTimeout('):]
assert loop.index('MergeHolder(lastHolders, row.pid')<loop.index('WakeHolderQueues(lastHolders)')
assert 'GetTickCount64()-lastWake>=5000' in loop
assert 'holder.pid==GetCurrentProcessId()?1:0' in loop
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';p.write_text(h);exe=Path(d)/'test'
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
