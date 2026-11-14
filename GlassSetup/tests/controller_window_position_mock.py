"""Actual parent-window lookup; fake HWND enumeration, PID, class and geometry."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassMaintenanceLaunch.cpp').read_text()
a=s.index('bool GetControllerParentWindowRect(');b=s.index('\nbool BeginControllerChild(',a)
fn=s[a:b]
h=r'''
#include <cwchar>
#include <cassert>
#include <vector>
using DWORD=unsigned;using HWND=int;using LPARAM=long long;using BOOL=int;
constexpr BOOL TRUE=1,FALSE=0;
struct RECT{long left,top,right,bottom;};
struct Item{unsigned pid;bool minimized;const wchar_t* name;RECT rect;bool readable=true;};
std::vector<Item> windows;bool validated=true;unsigned parent=42;
struct {struct{int value=1;}parent;}controllerChild;
bool IsControllerChild(){return validated;}
unsigned GetProcessId(int){return parent;}
bool IsIconic(int w){return windows[w-1].minimized;}
void GetWindowThreadProcessId(int w,unsigned* out){*out=windows[w-1].pid;}
int GetClassNameW(int w,wchar_t* out,int){std::wcscpy(out,windows[w-1].name);return std::wcslen(out);}
bool GetWindowRect(int w,RECT* out){*out=windows[w-1].rect;return windows[w-1].readable;}
void EnumWindows(BOOL (*callback)(HWND,LPARAM),LPARAM arg){for(unsigned i=1;i<=windows.size();++i)if(!callback(i,arg))break;}
// FN
int main(){
 for(auto x:{-1800,0,234,2400}){
  windows={{99,false,L"GlassSetupWindow",{1,2,100,100}},
           {42,false,L"OtherWindow",{1,2,100,100}},
           {42,false,L"GlassSetupWindow",{x,-120,x+1000,480}}};
  RECT r{};assert(GetControllerParentWindowRect(r)&&r.left==x&&r.top==-120);
  validated=false;assert(!GetControllerParentWindowRect(r));validated=true;
  windows.back().rect.left+=33;assert(GetControllerParentWindowRect(r)&&r.left==x+33);
  windows.back().minimized=true;assert(!GetControllerParentWindowRect(r));
 }
 windows.clear();RECT r{};assert(!GetControllerParentWindowRect(r));parent=0;assert(!GetControllerParentWindowRect(r));
}
'''.replace('// FN',fn)
main=(root/'GlassSetup/src/main.cpp').read_text()
create=main[main.index('        RECT parentBounds{};'):main.index('        ShowWindow(window, show);')]
assert create.index('GetControllerParentWindowRect')<create.index('CreateWindowExW(')
assert create.rindex('GetControllerParentWindowRect')<create.index('SetWindowPos(')
assert 'finalX=parentBounds.left;finalY=parentBounds.top;' in create
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';p.write_text(h);exe=Path(d)/'test'
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: parent PID/class/validated-handoff filter, moved and negative coordinates; inheritance before create/show and after DPI sizing (fake Win32 APIs)')
