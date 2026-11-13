"""Actual service-config parsing; safe upstream unquoted path only in path validation."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassInstallRemoval.cpp').read_text()
a=s.index('bool ReadServiceConfiguration(');b=s.index('// Back up all registry',a)
fn=s[a:b]
h=r'''
#include <string>
#include <vector>
#include <cwchar>
#include <cassert>
using DWORD=unsigned;using BYTE=unsigned char;using SC_HANDLE=int;
constexpr unsigned ERROR_INSUFFICIENT_BUFFER=122,ERROR_FILE_NOT_FOUND=2,ERROR_PATH_NOT_FOUND=3,SERVICE_WIN32_OWN_PROCESS=16,INVALID_FILE_ATTRIBUTES=~0u;
struct QUERY_SERVICE_CONFIGW{unsigned dwServiceType=16,dwStartType=2,dwErrorControl=1,dwTagId=0;
 const wchar_t *lpBinaryPathName=nullptr,*lpServiceStartName=L"LocalSystem",*lpLoadOrderGroup=L"",*lpDisplayName=L"OpenGlassHost",*lpDependencies=nullptr;};
QUERY_SERVICE_CONFIGW config;std::wstring command;unsigned lastError=122,attrError=2;bool prefixExists=false;
unsigned GetLastError(){return lastError;}
bool QueryServiceConfigW(SC_HANDLE,QUERY_SERVICE_CONFIGW* out,unsigned,unsigned* bytes){*bytes=sizeof(config);lastError=122;if(!out)return false;*out=config;out->lpBinaryPathName=command.c_str();return true;}
bool SamePath(const std::wstring& a,const std::wstring& b){return wcscasecmp(a.c_str(),b.c_str())==0;}
int _wcsicmp(const wchar_t* a,const wchar_t* b){return wcscasecmp(a,b);}
unsigned GetFileAttributesW(const wchar_t*){lastError=attrError;return prefixExists?0:INVALID_FILE_ATTRIBUTES;}
// FN
int main(){std::wstring sig;const std::wstring expected=L"C:\\Program Files\\GlassPlatform\\OpenGlass\\OpenGlassHost.exe";
 command=expected+L" \t";assert(ReadServiceConfiguration(1,expected,sig,true));assert(!ReadServiceConfiguration(1,expected,sig));
 command=L"\""+expected+L"\" ";assert(ReadServiceConfiguration(1,expected,sig));
 command=expected;prefixExists=true;assert(!ReadServiceConfiguration(1,expected,sig,true));prefixExists=false;
 attrError=5;assert(!ReadServiceConfiguration(1,expected,sig,true));attrError=2;
 command=expected+L" -foreign";assert(!ReadServiceConfiguration(1,expected,sig,true));
 command=L"C:\\Other\\OpenGlassHost.exe";assert(!ReadServiceConfiguration(1,expected,sig,true));
 command=expected;config.lpServiceStartName=L"OtherAccount";assert(!ReadServiceConfiguration(1,expected,sig,true));config.lpServiceStartName=L"LocalSystem";
 config.dwServiceType=32;assert(!ReadServiceConfiguration(1,expected,sig,true));
}
'''.replace('// FN',fn)
validation=s[s.index('bool ValidateOpenGlassPaths('):]
assert 'ReadServiceConfiguration(service,expected,signature,true)' in validation
assert 'matches=known && SamePath(image.data(),expected);' in validation
engine=(root/'GlassSetup/src/InstallEngine.cpp').read_text()
prep=engine[engine.index('bool InstallEngine::PrepareOfficialOpenGlass('):engine.index('bool InstallEngine::CommitOfficialOpenGlass(')]
assert 'ValidateOpenGlassPaths(before.directory,nullptr,ru,error)' in prep
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';p.write_text(h);exe=Path(d)/'test'
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: actual service parser: quoted/unquoted+trailing-space, exact opt-in only, ambiguous prefix, denied prefix probe, arguments/foreign path/account/type refusal; early path preflight and running image guard retained')
