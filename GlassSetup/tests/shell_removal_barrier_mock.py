"""Shell removal precedes MSI mutation; actual cache notifier uses no-wait delivery."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
engine=(root/'GlassSetup/src/InstallEngine.cpp').read_text()
seq=engine[engine.index('bool InstallEngine::RunMsiSequence('):]
assert seq.index('InvokeShellExtRegistration(false')<seq.index('RunOneMsi(')<seq.index('GlassMaintenanceExecutor::RunPhase(')
assert 'selection_.Removes(scanNativeX64_?0u:1u)' in seq
row=engine[engine.index('bool InstallEngine::RemoveManagedProduct('):engine.index('bool InstallEngine::RunMsiSequence(')]
assert 'InvokeShellExtRegistration' not in row
ops=(root/'GlassSettings/Native/ShellExtensionOps.cpp').read_text()
a=ops.index('extern "C" __declspec(dllexport) void WINAPI GlassSettings_NotifyShellCachesAfterRegistrationChange()');b=ops.index('\nextern "C"',a+1)
fn=ops[a:b].replace('__declspec(dllexport)','').replace('WINAPI','').replace('MAX_PATH','260')
h=r'''
#include <cassert>
#define SUCCEEDED(x) ((x)>=0)
constexpr int SHCNE_ASSOCCHANGED=1,SHCNE_UPDATEDIR=2,SHCNF_IDLIST=0,SHCNF_PATH=1,SHCNF_FLUSHNOWAIT=0x2000,CSIDL_DESKTOPDIRECTORY=0,SHGFP_TYPE_CURRENT=0;
int calls=0;
void SHChangeNotify(int event,unsigned flags,const void*,const void*){assert(flags&SHCNF_FLUSHNOWAIT);assert(event==1||event==2);++calls;}
int SHGetFolderPathW(void*,int,void*,int,wchar_t* out){out[0]=L'C';out[1]=0;return 0;}
// FN
int main(){GlassSettings_NotifyShellCachesAfterRegistrationChange();assert(calls==2);}
'''.replace('// FN',fn)
shell=(root/'GlassShellExt/DesktopShellMenu.cpp').read_text()
assert 'SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT' in shell
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';p.write_text(h);exe=Path(d)/'test'
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: pre-MSI shell removal barrier and actual async shell-cache notifier; native DLL notifier also no-wait')
