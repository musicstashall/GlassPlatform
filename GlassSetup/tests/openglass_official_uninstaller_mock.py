#!/usr/bin/env python3
"""Compile the actual official uninstall runner; no Windows processes executed."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassOpenGlassUninstall.cpp').read_text()
runner=s[s.index('bool RunUninstaller('):s.rfind('\n}')]
h=r'''
#include <cassert>
#include <string>
#include <stdexcept>
using DWORD=unsigned;using HANDLE=int;
constexpr unsigned ERROR_INSTALL_FAILURE=1603,STARTF_USESHOWWINDOW=1,SW_HIDE=0,CREATE_NO_WINDOW=0x8000000,INFINITE=~0u,WAIT_OBJECT_0=0;
constexpr bool FALSE=false;
struct STARTUPINFOW{unsigned cb=0,dwFlags=0,wShowWindow=0;};
struct PROCESS_INFORMATION{HANDLE hProcess=0,hThread=0;};
bool queryOk=true,same=true,trusted=true,signedOk=true,pinOk=true,createOk=true,waitOk=true,exitOk=true;
unsigned code=0,creates=0,closes=0,pins=0,livePins=0;
std::wstring app,command,cwd;
namespace GlassOpenGlassInstall{
struct Record{bool installed=true;std::wstring directory=L"D:\\Official OG",uninstaller=L"D:\\Official OG\\unins000.exe";};
bool Query(Record&,std::wstring&){return queryOk;}
bool Same(const Record&,const Record&){return same;}
}
void Require(bool ok,const char* error){if(!ok)throw std::runtime_error(error);}
namespace GlassMaintenanceCache{bool TrustedDirectory(const std::wstring&){return trusted;}bool TrustedPath(const std::wstring&,bool){return trusted;}}
namespace GlassReleaseClient{std::wstring Wide(const std::string& s){return {s.begin(),s.end()};}}
struct File{HANDLE h=0;~File(){if(h)--livePins;}};
void Pin(File& f,const std::wstring& p){Require(pinOk,"pin failed");++pins;++livePins;f.h=1;assert(p==L"D:\\Official OG\\unins000.exe"||p==L"D:\\Official OG\\unins000.dat");}
unsigned VerifyPublisher(const std::wstring&,HANDLE){Require(signedOk,"signature failed");return 1;}
bool CreateProcessW(const wchar_t* exe,wchar_t* cmd,void*,void*,bool inherit,unsigned flags,void*,const wchar_t* work,STARTUPINFOW* startup,PROCESS_INFORMATION* process){
 ++creates;assert(livePins==0&&pins==2);app=exe;command=cmd;cwd=work;
 assert(!inherit&&flags==CREATE_NO_WINDOW&&startup->dwFlags==STARTF_USESHOWWINDOW&&startup->wShowWindow==SW_HIDE);
 process->hProcess=10;process->hThread=11;return createOk;
}
unsigned WaitForSingleObject(HANDLE h,unsigned t){assert(h==10&&t==INFINITE);return waitOk?WAIT_OBJECT_0:~0u;}
bool GetExitCodeProcess(HANDLE h,DWORD* out){assert(h==10);*out=code;return exitOk;}
void CloseHandle(HANDLE h){assert(h==10||h==11);++closes;}
// RUNNER
void reset(){queryOk=same=trusted=signedOk=pinOk=createOk=waitOk=exitOk=true;code=creates=closes=pins=livePins=0;}
int main(){
 GlassOpenGlassInstall::Record record;std::wstring error;DWORD result=0;
 for(unsigned exit:{0u,3010u,2u}){reset();code=exit;assert(RunUninstaller(record,result,error)&&result==exit&&closes==2&&livePins==0);
 assert(app==record.uninstaller&&cwd==record.directory);
 assert(command==L"\"D:\\Official OG\\unins000.exe\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /RESTARTEXITCODE=3010 /NODELETECONFIG");}
 for(bool* flag:{&queryOk,&same,&trusted,&signedOk,&pinOk}){reset();*flag=false;assert(!RunUninstaller(record,result,error)&&creates==0&&livePins==0&&!error.empty());}
 reset();record.installed=false;assert(!RunUninstaller(record,result,error)&&creates==0);record.installed=true;
 reset();createOk=false;assert(!RunUninstaller(record,result,error)&&closes==0&&livePins==0);
 reset();waitOk=false;assert(!RunUninstaller(record,result,error)&&closes==2);
 reset();exitOk=false;assert(!RunUninstaller(record,result,error)&&closes==2);
}
'''.replace('// RUNNER',runner)
with tempfile.TemporaryDirectory() as tmp:
    cpp=Path(tmp)/'runner.cpp';exe=Path(tmp)/'runner';cpp.write_text(h)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
for forbidden in ['RemoveInternetZone(', 'DeleteFileW(', 'TerminateProcess(', 'ShellExecute']:
    assert forbidden not in runner
print('PASS: actual official uninstall runner: registration/trust/signature/pins/command/config preservation/raw exits/handles')
