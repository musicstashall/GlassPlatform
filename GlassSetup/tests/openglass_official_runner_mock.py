"""Compile the actual official-Inno launcher with fake Win32 APIs.
No process is launched. This is not Windows/Authenticode/Inno acceptance.
"""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassOpenGlassPackage.cpp').read_text()
runner=s[s.index('bool RunInstaller('):s.rfind('\n}')]
h=r'''
#include <cassert>
#include <string>
#include <stdexcept>
using DWORD=unsigned;using HANDLE=int;
constexpr unsigned ERROR_INSTALL_FAILURE=1603,STARTF_USESHOWWINDOW=1,SW_HIDE=0,CREATE_NO_WINDOW=0x8000000,INFINITE=~0u,WAIT_OBJECT_0=0;
constexpr bool FALSE=false;
struct STARTUPINFOW{unsigned cb=0,dwFlags=0,wShowWindow=0;};
struct PROCESS_INFORMATION{HANDLE hProcess=0,hThread=0;};
struct Package{std::wstring setup,version;};
bool trusted=true,safe=true,createOk=true,waitOk=true,exitOk=true;
unsigned creates=0,closes=0,code=0;std::wstring actualVersion=L"3.0.2.3749",command,app,working,order;
void Require(bool ok,const char* error){if(!ok)throw std::runtime_error(error);}
namespace GlassMaintenanceCache{bool TrustedDirectory(const std::wstring&){return trusted;}bool TrustedPath(const std::wstring&,bool){return trusted;}}
namespace GlassInstallUtil{bool IsSafeInstallPath(const std::wstring&){return safe;}}
namespace GlassReleaseClient{std::wstring Wide(const std::string& s){return {s.begin(),s.end()};}}
struct File{HANDLE h=1;};
void Pin(File&,const std::wstring&){order+=L"pin;";}
unsigned VerifyPublisher(const std::wstring&,HANDLE){order+=L"verify;";return 1;}
void RemoveInternetZone(const std::wstring&){order+=L"unblock;";}
std::wstring SetupFileVersion(const std::wstring&){return actualVersion;}
bool CreateProcessW(const wchar_t* exe,wchar_t* cmd,void*,void*,bool inherit,unsigned flags,void*,const wchar_t* cwd,STARTUPINFOW* startup,PROCESS_INFORMATION* process){
 ++creates;app=exe;command=cmd;working=cwd;assert(!inherit&&flags==CREATE_NO_WINDOW);
 assert(startup->dwFlags==STARTF_USESHOWWINDOW&&startup->wShowWindow==SW_HIDE);
 process->hProcess=10;process->hThread=11;return createOk;
}
unsigned WaitForSingleObject(HANDLE h,unsigned timeout){assert(h==10&&timeout==INFINITE);return waitOk?WAIT_OBJECT_0:~0u;}
bool GetExitCodeProcess(HANDLE h,DWORD* out){assert(h==10);*out=code;return exitOk;}
void CloseHandle(HANDLE h){assert(h==10||h==11);++closes;}
// RUNNER
void reset(){trusted=safe=createOk=waitOk=exitOk=true;creates=closes=code=0;order.clear();actualVersion=L"3.0.2.3749";}
int main(){
 Package package{L"C:\\Protected\\OpenGlassSetup.Signed.exe",L"3.0.2.3749"};std::wstring error;DWORD result=0;
 for(unsigned exit:{0u,3010u,2u}){reset();code=exit;assert(RunInstaller(package,L"D:\\Other Folder\\OG",result,error));assert(result==exit&&closes==2&&creates==1);
  assert(app==package.setup&&working==L"C:\\Protected");
  assert(command==L"\"C:\\Protected\\OpenGlassSetup.Signed.exe\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /NOICONS /MERGETASKS=\"!desktopicon\" /SP- /RESTARTEXITCODE=3010 /DIR=\"D:\\Other Folder\\OG\" /LOG=\"C:\\Protected\\OpenGlass-install.log\"");
  assert(order==L"pin;verify;unblock;pin;verify;");
 }
 for(auto dir:{L"C:\\",L"relative",L"C:\\Bad\" /TASKS=x",L"C:\\Bad\nPath",L"C:\\OG\\"}){reset();assert(!RunInstaller(package,dir,result,error)&&creates==0);}
 reset();trusted=false;assert(!RunInstaller(package,L"C:\\OG",result,error)&&creates==0);
 reset();actualVersion=L"9.0.0.0";assert(!RunInstaller(package,L"C:\\OG",result,error)&&creates==0);
 reset();createOk=false;assert(!RunInstaller(package,L"C:\\OG",result,error)&&closes==0);
 reset();waitOk=false;assert(!RunInstaller(package,L"C:\\OG",result,error)&&closes==2);
 reset();exitOk=false;assert(!RunInstaller(package,L"C:\\OG",result,error)&&closes==2);
}
'''.replace('// RUNNER',runner)
with tempfile.TemporaryDirectory(prefix='OgOfficial-') as d:
 p=Path(d)/'test.cpp';exe=Path(d)/'test';p.write_text(h)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
prepare=s[s.index('static bool PrepareImpl('):s.index('bool Prepare(const std::wstring& workspace')]
assert prepare.index('if(!extractPayload&&!GlassSetupSourceLayout::directory.empty())') < prepare.index('Metadata(cancel,metadata,error)')
assert 'StageAttachedInstaller(local,setup,cancel,package,created)' in prepare
assert prepare.index('if(!extractPayload) {\n            const auto actual=SetupFileVersion(setup)') < prepare.index('GlassInnoExtract::Extract(')
assert 'TerminateProcess' not in runner and 'ShellExecute' not in runner
solution=(root/'GlassProject.sln').read_text()
assert 'OpenGlass-x64.wixproj' not in solution and '{D1E2F3A4-B5C6-4789-0ABC-DEF123456789}' not in solution
assert not (root/'GlassSetup/msi/OpenGlass-x64.wixproj').exists()
print('PASS: actual official installer launcher command/trust/unblock/wait/raw exits/handles; official acquisition precedence and retired MSI graph source contracts')
