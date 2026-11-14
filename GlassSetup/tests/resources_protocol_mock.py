"""Actual read-only acquired MSI capability check with fake MSI database APIs."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassInstallMsi.cpp').read_text()
fn=s[s.index('bool SupportsResourcesBeforeOpenGlass('):s.index('std::wstring ProductCodeFromMsiPath(')]
h=r'''
#include <string>
#include <vector>
#include <cwchar>
#include <cassert>
#include <iterator>
using MSIHANDLE=unsigned;using DWORD=unsigned;
constexpr unsigned ERROR_SUCCESS=0,ERROR_NO_MORE_ITEMS=259,MSIDBOPEN_READONLY=0;
int handles=0,fail=0;unsigned indexRow=0;std::vector<std::wstring> rows;
unsigned MsiOpenDatabaseW(const wchar_t*,unsigned,MSIHANDLE* h){if(fail==1)return 5;*h=1;++handles;return 0;}
unsigned MsiDatabaseOpenViewW(MSIHANDLE,const wchar_t* sql,MSIHANDLE* h){assert(std::wstring(sql)==L"SELECT `Condition` FROM `LaunchCondition`");if(fail==2)return 5;*h=2;++handles;return 0;}
unsigned MsiViewExecute(MSIHANDLE,unsigned){return fail==3?5:0;}
unsigned MsiViewFetch(MSIHANDLE,MSIHANDLE* h){if(fail==4)return 5;if(indexRow==rows.size())return ERROR_NO_MORE_ITEMS;*h=3+indexRow++;++handles;return 0;}
unsigned MsiRecordGetStringW(MSIHANDLE h,unsigned,const wchar_t* out,DWORD* n){if(fail==5)return 5;const auto& text=rows[h-3];if(text.size()>=*n)return 234;std::wcscpy(const_cast<wchar_t*>(out),text.c_str());*n=text.size();return 0;}
void MsiCloseHandle(MSIHANDLE){--handles;}
// FN
bool check(){indexRow=0;const bool ok=SupportsResourcesBeforeOpenGlass(L"verified.msi");assert(handles==0);return ok;}
int main(){
 rows={L"Installed OR OPENGLASSDLL"};assert(!check());
 rows={L"Installed OR GLASS_OG_INSTALL_PENDING"};assert(!check());
 rows={L"GLASS_OG_INSTALL_PENDING = 1 OR Installed OR OPENGLASSDLL"};assert(check());
 for(fail=1;fail<=5;++fail){assert(!check());}
 fail=0;
 rows={};assert(!check());rows={std::wstring(5000,L'x')};assert(!check());
 rows={L"VersionNT64",L"GLASS_OG_INSTALL_PENDING = 1 OR Installed"};assert(check());
}
'''.replace('// FN',fn)
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';exe=Path(d)/'test';p.write_text(h)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
ps=(root/'GlassSetup/scripts/New-ReleaseCatalog.ps1').read_text()
assert "StartsWith('GLASS_OG_INSTALL_PENDING = 1 OR '" in ps
assert ps.index('Outdated Resources MSI')<ps.index('$asset = New-Asset $files[$id]')
print('PASS: actual Resources protocol inspection, old/malformed/empty MSI refusal, new protocol, API failures/handle cleanup and catalog publication guard')
