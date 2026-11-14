"""Actual Settings process selection/WM_CLOSE/wait against Win32 seams.
No real process termination, Windows identity or MSI integration is tested.
"""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassInstallUtil.cpp').read_text()
a=s.index('namespace {\nbool VisitSelectedSettings(');b=s.index('std::wstring NativeRundll64()',a);code=s[a:b]
h=r'''
#include <atomic>
#include <cassert>
#include <cwchar>
#include <iterator>
#include <string>
#include <vector>
#include <iostream>
#define _wcsicmp wcscasecmp
using DWORD=unsigned;using HANDLE=int;using ULONGLONG=unsigned long long;using LPARAM=long long;
constexpr unsigned ERROR_SUCCESS=0,ERROR_INVALID_PARAMETER=87,ERROR_CANCELLED=1223,ERROR_GEN_FAILURE=31,
 ERROR_NO_MORE_FILES=18,ERROR_NOT_ENOUGH_MEMORY=8,ERROR_TIMEOUT=1460,
 TH32CS_SNAPPROCESS=2,PROCESS_QUERY_LIMITED_INFORMATION=1,SYNCHRONIZE=2,FALSE=0,
 WAIT_TIMEOUT=258,WAIT_OBJECT_0=0;
constexpr HANDLE INVALID_HANDLE_VALUE=-1;
struct PROCESSENTRY32W {unsigned dwSize=0,th32ProcessID=0;wchar_t szExeFile[260]{};};
struct Process {DWORD pid;std::wstring path;bool alive=true,readable=true,cooperates=true;};
std::vector<Process> ps;std::vector<DWORD> closed;unsigned index=0,error=0;ULONGLONG now=0;bool snapOk=true;
DWORD GetLastError(){return error;}void SetLastError(DWORD e){error=e;}
HANDLE CreateToolhelp32Snapshot(unsigned,unsigned){return snapOk?100:INVALID_HANDLE_VALUE;}
bool Fill(PROCESSENTRY32W* e){if(index>=ps.size()){error=ERROR_NO_MORE_FILES;return false;}
 e->th32ProcessID=ps[index].pid;std::wcscpy(e->szExeFile,L"GlassSettings.exe");return true;}
bool Process32FirstW(HANDLE,PROCESSENTRY32W* e){index=0;return Fill(e);}
bool Process32NextW(HANDLE,PROCESSENTRY32W* e){++index;return Fill(e);}
HANDLE OpenProcess(unsigned,bool,DWORD pid){return int(pid);}
Process& get(HANDLE p){for(auto& x:ps)if(x.pid==unsigned(p))return x;std::abort();}
DWORD WaitForSingleObject(HANDLE p,unsigned){return get(p).alive?WAIT_TIMEOUT:WAIT_OBJECT_0;}
bool QueryFullProcessImageNameW(HANDLE p,unsigned,wchar_t* out,DWORD*){auto& x=get(p);if(!x.readable){error=5;return false;}std::wcscpy(out,x.path.c_str());return true;}
void CloseHandle(HANDLE){}
bool CloseWindowsForPid(int,LPARAM){return true;}
void EnumWindows(bool(*)(int,LPARAM),LPARAM pid){closed.push_back(unsigned(pid));auto& x=get(int(pid));if(x.cooperates)x.alive=false;}
ULONGLONG GetTickCount64(){return now;}void Sleep(unsigned t){now+=t;}
std::wstring TrimTrailingSlashes(std::wstring s){while(!s.empty()&&s.back()==L'\\')s.pop_back();return s;}
bool SameInstallPath(const std::wstring& a,const std::wstring& b){return _wcsicmp(a.c_str(),b.c_str())==0;}
// CODE
void reset(){ps={{1,L"64\\GlassSettings\\GlassSettings.exe"},{2,L"32\\GlassSettings\\GlassSettings.exe"},{3,L"portable\\GlassSettings.exe"}};
 closed.clear();now=0;error=0;snapOk=true;}
int main(){
 for(unsigned mask:{0u,4u,32u,36u}){reset();assert(CloseSelectedSettingsProcesses(L"64",L"32",mask,nullptr));
  std::vector<DWORD> expected;if(mask&4)expected.push_back(1);if(mask&32)expected.push_back(2);assert(closed==expected);
  bool running=true;assert(QuerySelectedSettingsRunning(L"64",L"32",mask,running));assert(!running);assert(ps[2].alive);}
 {reset();ps[0].cooperates=false;assert(!CloseSelectedSettingsProcesses(L"64",L"32",4,nullptr));assert(error==ERROR_TIMEOUT&&now==8000&&closed.size()==1);}
 {reset();ps[0].readable=false;bool running=false;assert(!QuerySelectedSettingsRunning(L"64",L"32",4,running));assert(closed.empty());}
 {reset();std::atomic_bool cancel{true};assert(!CloseSelectedSettingsProcesses(L"64",L"32",4,&cancel));assert(closed.empty());}
 {reset();snapOk=false;bool running=false;assert(!QuerySelectedSettingsRunning(L"64",L"32",4,running));}
 {reset();bool running=false;assert(QuerySelectedSettingsRunning(L"64",L"32",4,running)&&running);assert(closed.empty());}
 std::cout<<"PASS: actual Settings process scope: 32/64/both/none, foreign same-name process kept, timeout, unknown identity and cancellation\n";
}
'''.replace('// CODE',code)
with tempfile.TemporaryDirectory(prefix='ScopedSettings-') as d:
 p=Path(d)/'test.cpp';exe=Path(d)/'test';p.write_text(h)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
