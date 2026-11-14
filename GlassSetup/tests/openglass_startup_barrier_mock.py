"""Compile actual startup barrier with fake registration, SCM and time."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallEngine.cpp').read_text()
a=s.index('bool InstallEngine::EnsureOpenGlassBeforePlatform()');b=s.index('\nvoid InstallEngine::LaunchSelectedProducts()',a)
fn=s[a:b]
prefix=r'''
#include <string>
#include <cassert>
#include <vector>
using DWORD=unsigned;using ULONGLONG=unsigned long long;using BYTE=unsigned char;using SC_HANDLE=int;
constexpr unsigned SERVICE_DISABLED=4,SERVICE_DEMAND_START=3,SERVICE_RUNNING=4,SERVICE_START_PENDING=2,SERVICE_STOPPED=1;
constexpr unsigned SC_MANAGER_CONNECT=1,SERVICE_QUERY_STATUS=4,SC_STATUS_PROCESS_INFO=0;
constexpr int LANG_RUSSIAN=25;
int GetUserDefaultUILanguage(){return 0;}int PRIMARYLANGID(int v){return v;}
unsigned tick=0,query=0,closed=0,starts=0;bool present=true,known=true,safe=true,exists=true,accept=true,queryOk=true;
unsigned configuredType=2;std::vector<unsigned> states;
ULONGLONG GetTickCount64(){return tick;}void Sleep(unsigned ms){tick+=ms;}
SC_HANDLE OpenSCManagerW(void*,void*,unsigned access){assert(access==SC_MANAGER_CONNECT);return 1;}
SC_HANDLE OpenServiceW(SC_HANDLE,const wchar_t*,unsigned access){assert(access==SERVICE_QUERY_STATUS);return 2;}
void CloseServiceHandle(SC_HANDLE){++closed;}
struct SERVICE_STATUS_PROCESS{unsigned dwCurrentState;};
bool QueryServiceStatusEx(SC_HANDLE,unsigned,BYTE* p,unsigned,DWORD*){++query;if(!queryOk)return false;
 reinterpret_cast<SERVICE_STATUS_PROCESS*>(p)->dwCurrentState=states.empty()?SERVICE_START_PENDING:states[query-1<states.size()?query-1:states.size()-1];return true;}
namespace GlassOpenGlassInstall {struct Record{bool installed;std::wstring directory=L"official";};bool Query(Record& r,std::wstring&){r.installed=present;return known;}}
namespace GlassInstallRemoval {bool ValidateOpenGlassPaths(const std::wstring& p,void*,bool,std::wstring&){assert(p==L"official");return safe;}}
namespace GlassInstallHost {bool QueryOpenGlassServiceStartType(bool& e,DWORD& type){e=exists;type=configuredType;return true;}
 bool StartOpenGlassServicePreservingStartType(DWORD type){assert(type==configuredType);++starts;return accept;}}
namespace InstallLog {void Line(const wchar_t*){}template<class... T>void LineFormat(const wchar_t*,T...) {}}
struct InstallEngine{bool EnsureOpenGlassBeforePlatform() noexcept;};
'''
main=r'''
void reset(){configuredType=2;tick=query=closed=starts=0;present=known=safe=exists=accept=queryOk=true;states.clear();}
int main(){InstallEngine e;
 reset();present=false;assert(e.EnsureOpenGlassBeforePlatform()&&starts==0);
 reset();known=false;assert(!e.EnsureOpenGlassBeforePlatform()&&starts==0);
 reset();safe=false;assert(!e.EnsureOpenGlassBeforePlatform()&&starts==0);
 reset();exists=false;assert(!e.EnsureOpenGlassBeforePlatform()&&starts==0);
 reset();configuredType=SERVICE_DISABLED;assert(!e.EnsureOpenGlassBeforePlatform()&&starts==0);
 reset();accept=false;assert(!e.EnsureOpenGlassBeforePlatform()&&query==0);
 reset();states={SERVICE_START_PENDING,SERVICE_START_PENDING,SERVICE_RUNNING};assert(e.EnsureOpenGlassBeforePlatform()&&query==3&&tick==400&&closed==2);
 reset();states={SERVICE_STOPPED};assert(!e.EnsureOpenGlassBeforePlatform()&&closed==2);
 reset();queryOk=false;assert(!e.EnsureOpenGlassBeforePlatform()&&closed==2);
 reset();assert(!e.EnsureOpenGlassBeforePlatform()&&tick>=15000&&closed==2);
}
'''
launch=s[b:s.index('bool InstallEngine::StopHostsForRollback()',b)]
assert launch.index('EnsureOpenGlassBeforePlatform()')<launch.index('startArch(GP::PlatformArch::X64')
assert 'if(ogReady)' in launch and 'if (ogReady && !RestorePriorHosts(true))' in launch
worker=s[s.index('DWORD WINAPI InstallEngine::WorkerThread('):s.index('DWORD WINAPI InstallEngine::RunGlassWorker(')]
assert worker.index('CommitOfficialOpenGlass(mixedRemoval)')<worker.index('self->LaunchSelectedProducts()')
assert '!self->mixedOpenGlass_)self->LaunchSelectedProducts()' in s
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';p.write_text(prefix+fn+main);exe=Path(d)/'test'
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: actual OG startup barrier: absence, trust gate, start failure, pending/running, stopped, query failure, bounded timeout, handle cleanup; platform/mixed ordering source gates')
