"""Actual conservative reboot guard, fake read-only registry; no Windows/MSI IO."""
from pathlib import Path
import shutil, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassInstallUtil.cpp').read_text()
assert '#include <algorithm>' in s, 'Production TU must declare std::any_of directly'
body=s[s.index('bool QuerySecureUxPendingFileOperations('):s.index('bool QuerySecureUxThemeState(')]
harness=r'''
#include <algorithm>
#include <cassert>
#include <cstring>
#include <cwchar>
#include <string>
#include <iostream>
#include <vector>
using DWORD=unsigned;using LONG=long;using UINT=unsigned;
#define _wcsicmp wcscasecmp
#define _wcsnicmp wcsncasecmp
namespace GlassInstallLog {void Line(const wchar_t*){}template<class... A>void LineFormat(const wchar_t*,A...){} }
bool windowsKnown=true;
UINT GetSystemWindowsDirectoryW(wchar_t* out,UINT){if(!windowsKnown)return 0;std::wcscpy(out,L"C:\\Windows");return 10;}
constexpr int HKEY_LOCAL_MACHINE=1,RRF_RT_REG_MULTI_SZ=32,ERROR_SUCCESS=0,ERROR_FILE_NOT_FOUND=2,ERROR_ACCESS_DENIED=5;
LONG first=0,second=0;int calls=0;DWORD sizeOverride=0;bool overrideSize=false;
std::vector<wchar_t> data;
LONG RegGetValueW(int,const wchar_t*,const wchar_t*,int,void*,void* out,DWORD* bytes){
 ++calls;if(!out){*bytes=overrideSize?sizeOverride:DWORD(data.size()*sizeof(wchar_t));return first;}
 if(second)return second;
 const DWORD size=DWORD(data.size()*sizeof(wchar_t));if(*bytes<size)return 234;
 if(size){std::memcpy(out,data.data(),size);}
 *bytes=size;return 0;
}
// BODY
void reset(){windowsKnown=true;first=second=0;overrideSize=false;calls=0;data={0,0};}
void queue(const std::wstring& source,const std::wstring& destination=L""){
 data.assign(source.begin(),source.end());data.push_back(0);data.insert(data.end(),destination.begin(),destination.end());data.push_back(0);data.push_back(0);
}
int main(){bool pending=false;
 for(const auto* path:{L"\\??\\C:\\Windows\\System32\\SecureUxTheme4.dll",L"C:\\Windows\\SysWOW64\\ThemeUiProxy.dll",L"\\?\\c:\\WINDOWS\\SYSTEM32\\secureuxtheme4.DLL",L"\\SystemRoot\\System32\\SecureUxTheme4.dll"}){
  reset();queue(path);assert(QuerySecureUxPendingFileOperations(pending)&&pending);
 }
 reset();queue(L"C:\\Temp\\source.dll",L"!\\??\\C:\\Windows\\System32\\SecureUxTheme4.dll");assert(QuerySecureUxPendingFileOperations(pending)&&pending);
 reset();queue(L"\\??\\C:\\Windows\\System32\\Unrelated.dll");assert(QuerySecureUxPendingFileOperations(pending)&&!pending);
 reset();queue(L"C:\\Temp\\SecureUxTheme4.dll");assert(QuerySecureUxPendingFileOperations(pending)&&!pending);
 reset();queue(L"\\??\\Volume{unknown}\\Windows\\System32\\SecureUxTheme4.dll");assert(QuerySecureUxPendingFileOperations(pending)&&pending);
 reset();queue(L"C:\\Temp\\unrelated.dll");data.pop_back();
 {const std::wstring next=L"\\??\\C:\\Windows\\SysWOW64\\SecureUxTheme4.dll";data.insert(data.end(),next.begin(),next.end());data.insert(data.end(),{0,0,0});}
 assert(QuerySecureUxPendingFileOperations(pending)&&pending);
 reset();data={L'a',0,0};assert(!QuerySecureUxPendingFileOperations(pending)&&pending);
 reset();windowsKnown=false;assert(!QuerySecureUxPendingFileOperations(pending)&&pending);
 reset();data={0,0,L'a',0,0};assert(!QuerySecureUxPendingFileOperations(pending)&&pending);

 reset();assert(QuerySecureUxPendingFileOperations(pending)&&!pending&&calls==2);
 reset();first=ERROR_FILE_NOT_FOUND;assert(QuerySecureUxPendingFileOperations(pending)&&!pending&&calls==1);
 reset();data={L'a',0,0,0};assert(QuerySecureUxPendingFileOperations(pending)&&!pending);
 reset();first=ERROR_ACCESS_DENIED;assert(!QuerySecureUxPendingFileOperations(pending)&&pending);
 reset();second=ERROR_ACCESS_DENIED;assert(!QuerySecureUxPendingFileOperations(pending)&&pending);
 for(unsigned size:{0u,1u,3u,1048577u}){reset();overrideSize=true;sizeOverride=size;assert(!QuerySecureUxPendingFileOperations(pending)&&pending&&calls==1);}
 reset();data={L'a',L'b'};assert(!QuerySecureUxPendingFileOperations(pending)&&pending);
 reset();data={L'a',0,L'b',0,0};assert(QuerySecureUxPendingFileOperations(pending)&&!pending);
 std::cout<<"PASS: actual read-only reboot guard: SUT source/destination, native/WOW, NT paths, unrelated queues ignored, malformed/type/access/size/race refusal\n";
}
'''.replace('// BODY',body)
compiler=shutil.which('clang++') or shutil.which('g++')
if not compiler:raise SystemExit('C++17 compiler required')
with tempfile.TemporaryDirectory(prefix='GlassPendingRename-') as directory:
 p=Path(directory)/'test.cpp';exe=Path(directory)/'test';p.write_text(harness)
 subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror','-pedantic',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
