"""Production Setup/controller FileVersion readers; fake version APIs, not Windows."""
from pathlib import Path
import shutil, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
main=(root/'GlassSetup/src/main.cpp').read_text()
launch=(root/'GlassSetup/src/InstallSupport/GlassMaintenanceLaunch.cpp').read_text()
def fn(s,token):
 a=s.index(token);b=s.index('{',a);i=b+1;depth=1
 while depth:
  depth+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[a:i]
assert 'result.self = SelfFileVersion();' in main
assert 'TryParseVersion(ReadSetupFileVersion(result.preparedSetup),actualVersion)' in main
assert 'Downloaded Setup FileVersion differs from release catalog' in main
assert 'result.self = SelfProductVersion();' not in main
script=(root/'GlassSetup/scripts/New-ReleaseCatalog.ps1').read_text()
for part in ('FileMajorPart','FileMinorPart','FileBuildPart','FilePrivatePart'):
 assert part in script
assert 'GetVersionInfo($exe).ProductVersion' not in script
assert "New-Asset $files[$id] $properties.ProductVersion" in script
assert "New-Asset 'GlassSetup.exe' $version" in script
h=r'''
#include <string>
#include <vector>
#include <cassert>
#include <cstdint>
#include <cwchar>
using DWORD=unsigned;using UINT=unsigned;using BYTE=unsigned char;using ULONGLONG=unsigned long long;
struct VS_FIXEDFILEINFO {DWORD dwSignature=0xfeef04bd,dwFileVersionMS=2<<16,dwFileVersionLS=109,dwProductVersionMS=2<<16,dwProductVersionLS=108;};
#define HIWORD(v) ((v)>>16)
#define LOWORD(v) ((v)&65535)
VS_FIXEDFILEINFO info;
bool exists=true,read=true,query=true,shortInfo=false,oversize=false;
DWORD GetFileVersionInfoSizeW(const wchar_t*,DWORD*){return !exists?0:oversize?1024*1024+1:sizeof(info);}
bool GetFileVersionInfoW(const wchar_t*,int,DWORD,void*){return read;}
bool VerQueryValueW(void*,const wchar_t*,void** out,UINT* length){*out=&info;*length=shortInfo?1:sizeof(info);return query;}
DWORD GetModuleFileNameW(void*,wchar_t* out,DWORD){std::wcscpy(out,L"setup.exe");return 9;}
// READERS
int main(){
 assert(SelfFileVersion()==L"2.0.0.109");const auto previous=ControllerVersion(L"setup.exe");
 // A Setup-only rebuild wins even though bundle ProductVersion stays 108.
 info.dwFileVersionLS=110;
 assert(SelfFileVersion()==L"2.0.0.110"&&ControllerVersion(L"setup.exe")>previous);
 info.dwProductVersionLS=999;
 assert(SelfFileVersion()==L"2.0.0.110");
 for(int n=0;n<7;++n){
  exists=read=query=true;shortInfo=oversize=false;info.dwSignature=0xfeef04bd;
  if(n==0)exists=false;if(n==1)read=false;if(n==2)query=false;
  if(n==3)shortInfo=true;if(n==4)oversize=true;if(n==5)info.dwSignature=0;if(n==6){info.dwFileVersionMS=0;info.dwFileVersionLS=0;}
  assert(ReadSetupFileVersion(L"setup.exe").empty()&&ControllerVersion(L"setup.exe")==0);
 }
}
'''.replace('// READERS','\n'.join([fn(main,'std::wstring ReadSetupFileVersion('),fn(main,'std::wstring SelfFileVersion('),fn(launch,'ULONGLONG ControllerVersion(')]))
with tempfile.TemporaryDirectory(prefix='GlassSetupFileVersion-') as temp:
 p=Path(temp)/'test.cpp';p.write_text(h);exe=Path(temp)/'test'
 subprocess.run([shutil.which('g++'),'-std=c++17','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: production Setup and controller numeric FileVersion; constant/independent ProductVersion, malformed resources; catalog generator contract')
