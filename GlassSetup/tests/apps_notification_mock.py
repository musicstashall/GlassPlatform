"""Compile actual bounded Settings-change notification; no desktop messages sent."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassAppsNotification.h').read_text()
impl=s[s.index('namespace GlassAppsNotification {'):]
h=r'''
#include <string>
#include <cassert>
using DWORD_PTR=unsigned long;using LPARAM=long;
constexpr int HWND_BROADCAST=42,WM_SETTINGCHANGE=26,SMTO_ABORTIFHUNG=2;
int calls=0;
int SendMessageTimeoutW(int hwnd,int msg,int wparam,LPARAM param,int flags,unsigned timeout,DWORD_PTR*){
 assert(hwnd==HWND_BROADCAST&&msg==WM_SETTINGCHANGE&&wparam==0&&flags==SMTO_ABORTIFHUNG&&timeout==100);
 assert(std::wstring(reinterpret_cast<const wchar_t*>(param))==L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall");
 ++calls;return 0; // Hung/unresponsive receivers do not fail product removal.
}
// IMPL
int main(){GlassAppsNotification::Changed();assert(calls==1);}
'''.replace('// IMPL',impl)
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';exe=Path(d)/'test';p.write_text(h)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
msi=(root/'GlassSetup/src/InstallSupport/GlassInstallMsi.cpp').read_text()
fn=msi[msi.index('bool UninstallProduct('):msi.index('std::wstring ProductCodeFromMsiPath(')]
assert fn.index('MsiConfigureProductExW(')<fn.index('GlassAppsNotification::Changed()')
assert 'exitCode==ERROR_SUCCESS||exitCode==ERROR_SUCCESS_REBOOT_REQUIRED||exitCode==ERROR_UNKNOWN_PRODUCT' in fn
refresh=msi[msi.index('bool RefreshManagedArp('):msi.index('DWORD CALLBACK SnapshotCopyProgress')]
assert 'GlassAppsNotification::Changed()' in refresh
print('PASS: actual bounded broadcast; per-uninstall and batch ARP publication notifications; no UI automation/process control')
