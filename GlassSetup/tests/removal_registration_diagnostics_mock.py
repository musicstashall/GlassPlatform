"""Actual read-only removal postcheck; no registry deletion or MSI configuration."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassInstallMsi.cpp').read_text()
a=s.index('static void LogRemovalRegistration(');b=s.index('bool UninstallProduct(',a)
fn=s[a:b]
h=r'''
#include <string>
#include <cassert>
#include <initializer_list>
using HKEY=void*;constexpr HKEY HKEY_LOCAL_MACHINE=nullptr;
constexpr unsigned MSIINSTALLCONTEXT_ALL=7,KEY_QUERY_VALUE=1,KEY_WOW64_64KEY=256,KEY_WOW64_32KEY=512;
unsigned enumeration=259,opened=0,closed=0,logged=0,openError=0;
unsigned MsiEnumProductsExW(const wchar_t*,const wchar_t* sid,unsigned mode,unsigned i,wchar_t*,void*,void*,void*){
 assert(std::wstring(sid)==L"S-1-1-0"&&mode==7&&i==0);return enumeration;}
int MsiQueryProductStateW(const wchar_t*){return -1;}
long RegOpenKeyExW(HKEY,const wchar_t* path,unsigned,unsigned view,HKEY* key){
 assert(std::wstring(path)==L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\known-product");
 assert(view==(KEY_QUERY_VALUE|KEY_WOW64_64KEY)||view==(KEY_QUERY_VALUE|KEY_WOW64_32KEY));
 ++opened;if(openError)return openError;*key=reinterpret_cast<void*>(1);return 0;}
void RegCloseKey(HKEY){++closed;}
namespace GlassInstallLog {template<class... T>void LineFormat(const wchar_t*,T...){++logged;}}
// FN
int main(){
 for(unsigned result:{0u,259u,5u})for(unsigned error:{0u,2u,5u}){
  enumeration=result;openError=error;opened=closed=logged=0;
  LogRemovalRegistration(L"known-product");assert(opened==2&&closed==(error?0u:2u)&&logged==3);
 }
}
'''.replace('// FN',fn)
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';exe=Path(d)/'test';p.write_text(h)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: actual post-removal all-context/ARP-view diagnostics, absence/access errors and handle cleanup; no mutation APIs')
