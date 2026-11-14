"""Compile the actual second-instance early-exit path with fake window APIs."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/main.cpp').read_text()
a=s.index('    if(GetLastError()==ERROR_ALREADY_EXISTS&&!IsControllerChild())');b=s.index('    INITCOMMONCONTROLSEX controls',a)
assert 'AlreadyRunning' not in s
assert s.index('FindWindowW(L"GlassSetupWindow"')<s.index('    if (!gInstaller.Create(show))')
h=r'''
#include <cassert>
using HWND=int;using DWORD=unsigned;
constexpr int ERROR_ALREADY_EXISTS=183,SW_RESTORE=9,SW_SHOW=5;
bool second=true,child=false,minimized=true;int foundAfter=0,finds=0,shown=0,foreground=0,closed=0,sleeps=0;
int GetLastError(){return second?183:0;}bool IsControllerChild(){return child;}
HWND FindWindowW(const wchar_t*,void*){return finds++>=foundAfter?1:0;}
void GetWindowThreadProcessId(int,DWORD* pid){*pid=42;}
void AllowSetForegroundWindow(DWORD pid){assert(pid==42);}
bool IsIconic(int){return minimized;}
void ShowWindowAsync(int,int mode){assert(mode==(minimized?SW_RESTORE:SW_SHOW));++shown;}
void SetForegroundWindow(int){++foreground;}void CloseHandle(int){++closed;}void CoUninitialize(){}
void Sleep(int value){assert(value==50);++sleeps;}
namespace InstallLog{void Shutdown(){}}
int Launch(){int instanceMutex=1;
// PATH
return 7;}
void reset(){second=true;child=false;minimized=true;foundAfter=finds=shown=foreground=closed=sleeps=0;}
int main(){
 reset();assert(Launch()==0&&shown==1&&foreground==1&&closed==1);
 reset();minimized=false;foundAfter=3;assert(Launch()==0&&shown==1&&sleeps==3);
 reset();foundAfter=99;assert(Launch()==0&&shown==0&&sleeps==40&&closed==1);
 reset();child=true;assert(Launch()==7&&finds==0); // authorized handoff is unaffected
 reset();second=false;assert(Launch()==7&&finds==0);
}
'''.replace('// PATH',s[a:b])
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';p.write_text(h);exe=Path(d)/'test'
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: actual duplicate launch: activate/restore, bounded startup race, silent exit, no error page, handoff exempt')
