"""Execute the actual Native query-state export with fake read-only observations.
Does not load DLLs or exercise Windows registry/event/MSI discovery.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'GlassSettings/Native/SecureUxThemeNative.cpp').read_text()
legacy=source[source.index('    [[nodiscard]] bool HasUnsupportedLegacySecureUxTheme(\n'):source.index('    // Возврат к стандартному стилю')]
query=source[source.index('extern "C" void CALLBACK GlassSettings_SecureUxThemeQueryStateV1W('):source.index('extern "C" void CALLBACK GlassSettings_SecureUxThemeActivateSetupV1W(')]
harness=r'''
#include <cassert>
#include <csetjmp>
#include <iostream>
using DWORD=unsigned;using BOOL=int;using HWND=void*;using HINSTANCE=void*;using LPWSTR=wchar_t*;
#define CALLBACK
#define FALSE 0
#define FAILED(hr) ((hr)<0)
constexpr DWORD kFlgApplicationVerifier=0x100;
constexpr DWORD kSecureUxStateUnavailable=0,kSecureUxStateInactive=1,kSecureUxStateActive=2,
 kSecureUxStateActivationPending=3,kSecureUxStateDeactivationPending=4,kSecureUxStateExternal=5,kSecureUxStateConflict=6;
struct GlassSettings_SecureUxThemeNativeQuery {
 DWORD WinlogonGlobalFlag=0,ExplorerGlobalFlag=0,SettingsGlobalFlag=0,LogonUiGlobalFlag=0,DwmGlobalFlag=0;
 BOOL WinlogonHasLegacyExact=0,ExplorerHasLegacyExact=0,SettingsHasLegacyExact=0,LogonUiHasLegacyExact=0,DwmHasLegacyExact=0,
 WinlogonHasOurToken=0,LoadedEventPresent=0,ComProxyHijacked=0,SystemDll4Present=0,SystemLegacyDllPresent=0,
 ThemeUiProxyPresent=0,OfficialMsiPresent=0;
} observed;
bool wow=false;int queryResult=0;DWORD exitCode=99;std::jmp_buf exitTarget;
bool IsWow64ProcessNow(){return wow;}
int GlassSettings_SecureUxThemeQuery(GlassSettings_SecureUxThemeNativeQuery* q){*q=observed;return queryResult;}
[[noreturn]] void ExitProcess(DWORD code){exitCode=code;std::longjmp(exitTarget,1);}
// SOURCE
DWORD State(){if(setjmp(exitTarget)==0)GlassSettings_SecureUxThemeQueryStateV1W(nullptr,nullptr,nullptr,0);return exitCode;}
int main(){
 assert(State()==kSecureUxStateUnavailable);
 observed.SystemLegacyDllPresent=1;assert(State()==kSecureUxStateUnavailable);
 observed.SystemDll4Present=1;observed.ThemeUiProxyPresent=1;observed.ComProxyHijacked=1;
 assert(State()==kSecureUxStateInactive);
 observed.OfficialMsiPresent=1;assert(State()==kSecureUxStateInactive);
 observed.WinlogonHasOurToken=1;assert(State()==kSecureUxStateInactive);
 observed.WinlogonGlobalFlag=kFlgApplicationVerifier;assert(State()==kSecureUxStateActivationPending);
 observed.LoadedEventPresent=1;assert(State()==kSecureUxStateActive);
 observed.WinlogonGlobalFlag=0;assert(State()==kSecureUxStateDeactivationPending);
 observed={};observed.SystemLegacyDllPresent=1;observed.WinlogonHasLegacyExact=1;
 assert(State()==kSecureUxStateUnavailable);
 observed.WinlogonGlobalFlag=kFlgApplicationVerifier;assert(State()==kSecureUxStateExternal);
 observed={};queryResult=-1;assert(State()==kSecureUxStateConflict);
 queryResult=0;wow=true;assert(State()==kSecureUxStateConflict);
 std::cout<<"PASS: actual Native export: inert retained DLL/proxy, registered inactive MSI, active/pending, legacy hook, failed query, WOW64 guards\n";
}
'''.replace('// SOURCE',legacy+query)
compiler=shutil.which('clang++') or shutil.which('g++')
if not compiler:raise SystemExit('clang++ or g++ required')
with tempfile.TemporaryDirectory(prefix='GlassSutState-') as directory:
    path=Path(directory)/'state.cpp';exe=Path(directory)/'state'
    path.write_text(harness)
    subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-pedantic',str(path),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
