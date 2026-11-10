"""Exercise actual cache-parent waiting and post-MSI cleanup guards with API seams.
Not a Settings refresh, process/file-lock, SCM or Windows Installer runtime test.
"""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/GlassUnins/main.cpp').read_text()
wait=s[s.index('    const auto cachedLauncher='):s.index('\nstruct Selection {')].rstrip()
# The slice includes the final brace of RelaunchFromPrivateTemp.
a=s.index('            const bool serviceKnown=',s.index('if (product==P::Product::OpenGlass)'))
b=s.index('            if (!pathsSafe) {',a)
guard=s[a:b]
harness=r'''
#include <cassert>
namespace GlassMaintenanceLifetime {bool HasProducts(){return true;}bool RetireUnused(){return true;}}
#include <string>
#include <iostream>
using DWORD=unsigned long;
constexpr DWORD INFINITE=0xffffffff,WAIT_OBJECT_0=0,ERROR_SUCCESS=0;
struct State{bool cache=true,same=true,waitOk=true,service=true,tasks=true,serviceKnown=true,tasksKnown=true,pathsSafe=true;
 int waits=0,deleted=0,closed=0;DWORD exit=17;}s;
DWORD GetLastError(){return 5;}
DWORD WaitForSingleObject(int,DWORD){++s.waits;return s.waitOk?WAIT_OBJECT_0:0xffffffff;}
bool GetExitCodeProcess(int,DWORD* out){*out=s.exit;return true;}
void CloseHandle(int){++s.closed;}
void DeleteFileW(const wchar_t*){++s.deleted;}
void RemoveDirectoryW(const wchar_t*){}
namespace GlassMaintenanceCache {std::wstring CurrentFile(const wchar_t*){return s.cache?L"cache\\GlassUnins.exe":L"";}}
namespace GlassInstallUtil {
 bool SameInstallPath(const std::wstring&,const std::wstring&){return s.same;}
 struct OpenGlassTaskState {bool found=false;};
 bool QueryOpenGlassTasks(OpenGlassTaskState& out){out.found=s.tasks;return s.tasksKnown;}
}
namespace GlassInstallHost {bool QueryOpenGlassServiceStartType(bool& exists,DWORD&){exists=s.service;return s.serviceKnown;}}
namespace GlassInstallRemoval {bool ValidateOpenGlassPaths(const std::wstring&,const void*,bool,std::wstring&){return s.pathsSafe;}}
void LogLine(const std::wstring&){}bool IsRu(){return false;}
int Handoff(bool silent){struct {bool silent;}a{silent};struct {int hProcess=1;}pi;
 const wchar_t* executable=L"source.exe";std::wstring dest=L"temp\\unins.exe",temp=L"temp";
// WAIT
bool Guard(bool oldService,bool oldTasks){
 const bool ogServiceExisted=oldService;GlassInstallUtil::OpenGlassTaskState ogTasks{oldTasks},currentTasks;
 std::wstring backendError,openGlassDir_=L"expected-root";bool serviceExists=false;DWORD serviceType=0;
// GUARD
 return pathsSafe;
}
int main(){
 {s={};assert(Handoff(false)==17);assert(s.waits==1&&s.closed==1&&s.deleted==1);}
 {s={};s.same=false;assert(Handoff(false)==0);assert(s.waits==0&&s.deleted==0&&s.closed==1);}
 {s={};s.cache=false;assert(Handoff(false)==0);assert(s.waits==0&&s.deleted==0);}
 {s={};s.same=false;assert(Handoff(true)==17);assert(s.waits==1);}
 {s={};s.waitOk=false;assert(Handoff(false)==1);}
 {s={};assert(Guard(true,true));}
 {s={};s.service=false;assert(Guard(true,true));}
 {s={};s.service=s.tasks=false;assert(Guard(true,true));}
 {s={};assert(!Guard(false,true));assert(!Guard(true,false));}
 {s={};s.pathsSafe=false;assert(!Guard(true,true));}
 {s={};s.serviceKnown=false;assert(!Guard(true,true));}
 {s={};s.tasksKnown=false;assert(!Guard(true,true));}
 std::cout<<"PASS: actual Unins cache-parent wait/exit propagation/local unlock; cleanup allows disappearance, refuses new/foreign/unknown backends\n";
}
'''.replace('// WAIT',wait).replace('// GUARD',guard)
with tempfile.TemporaryDirectory(prefix='UninsHandoff-') as d:
 p=Path(d)/'test.cpp';exe=Path(d)/'test';p.write_text(harness)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
