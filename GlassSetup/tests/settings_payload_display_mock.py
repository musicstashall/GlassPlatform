"""Production Settings installed-payload projection; fake version resources only."""
from pathlib import Path
import shutil, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassProductInspect.cpp').read_text()
a=s.index('static void ReadSettingsPayloadVersion(');b=s.index('\nReport Run(',a)
assert 'ReadSettingsPayloadVersion(report.settings64,roots.x64)' in s
assert 'ReadSettingsPayloadVersion(report.settings32,roots.x86)' in s
assert 'L"GlassSettings.exe"' in s[s.index('const wchar_t* payloadFile='):]
code=r'''
#include <string>
#include <map>
#include <cassert>
struct ProductReport {bool installed=false;std::wstring installedPackageVersion,installedPayloadVersion;};
std::map<std::wstring,std::wstring> versions;
std::wstring PayloadFileVersion(const std::wstring& path){return versions[path];}
// PROJECTION
int main(){
 versions[L"x64\\GlassSettings\\GlassSettings.exe"]=L"2.0.1.410";
 versions[L"x86\\GlassSettings\\GlassSettings.exe"]=L"2.0.1.407";
 ProductReport x64{true,L"2.0.0.108",{}},x86=x64;
 ReadSettingsPayloadVersion(x64,L"x64");ReadSettingsPayloadVersion(x86,L"x86");
 assert(x64.installedPayloadVersion==L"2.0.1.410"&&x86.installedPayloadVersion==L"2.0.1.407");
 assert(x64.installedPackageVersion==L"2.0.0.108"&&x86.installedPackageVersion==L"2.0.0.108");
 ProductReport absent;ReadSettingsPayloadVersion(absent,L"x64");assert(absent.installedPayloadVersion.empty());
 ProductReport missing{true,L"2.0.0.108",{}};ReadSettingsPayloadVersion(missing,L"missing");assert(missing.installedPayloadVersion.empty());
}
'''.replace('// PROJECTION',s[a:b])
with tempfile.TemporaryDirectory(prefix='GlassSettingsPayload-') as temp:
 p=Path(temp)/'test.cpp';p.write_text(code);exe=Path(temp)/'test'
 subprocess.run([shutil.which('g++'),'-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: production Settings payload projection: distinct x64/x86 FileVersions, unchanged MSI identity, absent/missing version stays unknown')
