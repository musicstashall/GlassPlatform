"""Compile actual shared SCM pre-stop/check helper with fake Win32 APIs.
No Windows service, task, process or installation is changed.
"""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'GlassSetup/src/InstallSupport/GlassOpenGlassService.h').read_text()
impl=source[source.index('namespace GlassOpenGlassService {'):]
h=r'''
#include <string>
#include <cassert>
using DWORD=unsigned;using BOOL=bool;using SC_HANDLE=int;using BYTE=unsigned char;
constexpr DWORD ERROR_SUCCESS=0,ERROR_SERVICE_DOES_NOT_EXIST=1060,ERROR_SERVICE_MARKED_FOR_DELETE=1072,
 ERROR_SERVICE_CANNOT_ACCEPT_CTRL=1061,ERROR_INSTALL_PACKAGE_REJECTED=1625,ERROR_INSTALL_FAILURE=1603,
 SC_MANAGER_CONNECT=1,SERVICE_QUERY_STATUS=4,SC_STATUS_PROCESS_INFO=0,SERVICE_STOPPED=1,LANG_RUSSIAN=25;
#define PRIMARYLANGID(x) (x)
struct SERVICE_STATUS_PROCESS {DWORD dwCurrentState=1;};
struct State {DWORD openError=0,queryError=0,stopError=0;bool scm=true,valid=true,stop=true,stopped=true,ru=false;
 int live=0,stops=0,validations=0,logs=0;} s;
DWORD lastError=0;
DWORD GetLastError(){return lastError;}
unsigned GetUserDefaultUILanguage(){return s.ru?25:9;}
SC_HANDLE OpenSCManagerW(void*,void*,DWORD){if(!s.scm){lastError=5;return 0;}++s.live;return 1;}
SC_HANDLE OpenServiceW(SC_HANDLE,const wchar_t*,DWORD rights){assert(rights==SERVICE_QUERY_STATUS);if(s.openError){lastError=s.openError;return 0;}++s.live;return 2;}
void CloseServiceHandle(SC_HANDLE){--s.live;assert(s.live>=0);}
BOOL QueryServiceStatusEx(SC_HANDLE,unsigned,BYTE* data,unsigned,DWORD*){if(s.queryError){lastError=s.queryError;return false;}
 reinterpret_cast<SERVICE_STATUS_PROCESS*>(data)->dwCurrentState=s.stopped?1:4;return true;}
namespace GlassInstallLog {void Line(const wchar_t*){++s.logs;}}
namespace GlassInstallRemoval {bool ValidateOpenGlassPaths(const std::wstring&,void*,bool,std::wstring& e){assert(s.live==0);++s.validations;if(!s.valid)e=L"foreign path";return s.valid;}}
namespace GlassInstallHost {bool StopOpenGlassHost(void*,void*){assert(s.live==0);++s.stops;if(!s.stop){lastError=s.stopError;return false;}s.stopped=true;return true;}}
// IMPL
int main(){std::wstring error;DWORD code=0;bool exists=false,stopped=false;
 s={};assert(GlassOpenGlassService::PrepareRemoval(L"C:\\OG",error,code)&&s.live==0&&s.stops==1);
 s={};s.stopped=false;assert(GlassOpenGlassService::PrepareRemoval(L"C:\\OG",error,code)&&s.stopped&&s.live==0);
 s={};s.openError=1060;assert(GlassOpenGlassService::PrepareRemoval(L"C:\\OG",error,code)&&s.live==0);
 for(bool ru:{false,true}){s={};s.ru=ru;s.openError=1072;
 assert(!GlassOpenGlassService::PrepareRemoval(L"C:\\OG",error,code)&&code==1072&&s.stops==0&&s.validations==0&&s.live==0&&s.logs);
 assert(error.find(L"1072")!=std::wstring::npos);}
 s={};s.valid=false;assert(!GlassOpenGlassService::PrepareRemoval(L"C:\\OG",error,code)&&s.stops==0&&code==1625&&s.live==0);
 for(DWORD failure:{5u,1460u,1072u}){s={};s.stop=false;s.stopError=failure;
 assert(!GlassOpenGlassService::PrepareRemoval(L"C:\\OG",error,code)&&code==failure&&s.live==0);}
 s={};s.stopped=false;assert(!GlassOpenGlassService::Check(true,error,code)&&code==1061&&s.live==0);
 s={};s.queryError=5;assert(!GlassOpenGlassService::Check(false,error,code)&&code==5&&s.live==0);
 s={};s.scm=false;assert(!GlassOpenGlassService::Check(false,error,code)&&code==5&&s.live==0);
 s={};s.openError=1072;assert(GlassOpenGlassService::Probe(exists,stopped)==1072&&s.live==0);
}
'''.replace('// IMPL',impl)
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';exe=Path(d)/'test';p.write_text(h)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
for forbidden in ('DeleteService(', 'StartServiceW(', 'CreateServiceW(', 'ChangeServiceConfig', 'TerminateProcess'):
 assert forbidden not in impl
uninstall=(root/'GlassSetup/src/InstallSupport/GlassOpenGlassUninstall.cpp').read_text()
assert uninstall.index('VerifyPublisher(')<uninstall.index('PrepareRemoval(')<uninstall.index('CreateProcessW(')
engine=(root/'GlassSetup/src/InstallEngine.cpp').read_text()
prepare=engine[engine.index('bool InstallEngine::PrepareOfficialOpenGlass('):engine.index('bool InstallEngine::CommitOfficialOpenGlass(')]
assert 'GlassOpenGlassService::Check(false,serviceError,serviceCode)' in prepare
print('PASS: actual shared OG service helper: 1072, absent/stopped/running, validation, timeout/access refusal, handle closure and vendor boundary')
