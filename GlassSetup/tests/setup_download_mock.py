"""Execute the production Setup preparation routine with failure-injected fake Win32 APIs.
No network, certificate store, EXE execution, or platform operations. Not WinTrust integration.
"""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'GlassSetup/src/InstallSupport/GlassPackageAcquisition.cpp').read_text()
a=source.index('bool PrepareSetup(')
b=source.index('\nbool FetchCatalog(',a)
function=source[a:b]
main=(root/'GlassSetup/src/main.cpp').read_text()
check=main[main.index('    void ContinueVersionCheck()'):main.index('#ifdef _DEBUG',main.index('    void ContinueVersionCheck()'))]
assert 'FetchLatestSetupVersion' not in main and 'Page::Update' not in main
assert 'GlassPackageAcquisition::FetchCatalog' in check and 'GlassPackageAcquisition::PrepareSetup' in check
assert 'actualVersion.fields!=expectedVersion.fields' in check
assert 'ShellExecute' not in check and 'CreateProcess' not in check  # unfinished handoff must not be improvised
assert 'versionCheckCancel_.store(true' in main
assert check.count('InstallLog::Line(diagnostic.c_str())')==2
assert 'InstallLog::Line(diagnostic)' not in check
assert 'update-panel' not in (root/'GlassSetup/res/slideshow.html').read_text()
prefix=r'''
#include <cassert>
#include <atomic>
#include <string>
#include <cstdint>
#include "GlassSetup/src/InstallSupport/GlassPackageCatalog.h"
using HANDLE=int; using LONG=long;
constexpr int INVALID_HANDLE_VALUE=-1,GENERIC_READ=1,FILE_SHARE_READ=2,OPEN_EXISTING=3,FILE_FLAG_OPEN_REPARSE_POINT=4,MOVEFILE_WRITE_THROUGH=5;
constexpr int WTD_UI_NONE=1,WTD_CHOICE_FILE=2,WTD_STATEACTION_VERIFY=3,WTD_STATEACTION_CLOSE=4,WTD_CACHE_ONLY_URL_RETRIEVAL=8,WTD_REVOCATION_CHECK_NONE=16,ERROR_SUCCESS=0;
struct GUID{};const GUID WINTRUST_ACTION_GENERIC_VERIFY_V2{};
struct WINTRUST_FILE_INFO{unsigned cbStruct=0;const wchar_t* pcwszFilePath=nullptr;HANDLE hFile=0;};
struct WINTRUST_DATA{unsigned cbStruct=0;int dwUIChoice=0,dwUnionChoice=0;WINTRUST_FILE_INFO* pFile=nullptr;int dwStateAction=0,dwProvFlags=0;};
bool trusted=true,created=true,download=true,flush=true,opened=true,hash=true,signature=true,publisher=true,moved=true;
int handles=0,deletes=0,trustCalls=0,moves=0,gets=0;std::atomic<bool> cancel{false};
void CloseHandle(HANDLE h){assert(h==1);--handles;}
HANDLE CreateFileW(const wchar_t*,int,int,void*,int,int,void*){if(!opened)return -1;++handles;return 1;}
bool FlushFileBuffers(HANDLE){return flush;}
bool DeleteFileW(const wchar_t*){++deletes;return true;}
bool MoveFileExW(const wchar_t*,const wchar_t*,int){++moves;return moved;}
LONG WinVerifyTrust(void*,GUID*,WINTRUST_DATA* data){++trustCalls;return data->dwStateAction==WTD_STATEACTION_CLOSE||signature?0:1;}
namespace GlassMaintenanceCache{bool TrustedDirectory(const std::wstring&){return trusted;}}
namespace GlassProductInspect{bool FileHasSameSignerAsSetup(const std::wstring&){return publisher;}}
namespace GlassReleaseClient{
 HANDLE CreatePrivateFile(const std::wstring&){if(!created)return -1;++handles;return 1;}
 bool Get(const std::wstring&,HANDLE,std::string*,std::uint64_t limit,const std::atomic<bool>&,std::wstring&){++gets;assert(limit==1024);return download;}
 bool VerifyHash(HANDLE,std::uint64_t,const std::string&,const std::atomic<bool>&,std::wstring&){return hash;}
 std::wstring Wide(const std::string& text){return {text.begin(),text.end()};}
}
using GlassReleaseClient::Wide;
struct Handle{HANDLE h=-1;~Handle(){if(h!=-1)CloseHandle(h);}};
bool Fail(std::wstring& error,const std::wstring& text){error=text;return false;}
'''
tests=r'''
int main(){
 GlassPackageCatalog::Asset asset;asset.downloadable=true;asset.size=1024;asset.file="GlassSetup.exe";
 asset.url="https://github.com/musicstashall/GlassPlatform/releases/download/v1/GlassSetup.exe";asset.sha256=std::string(64,'a');
 std::wstring prepared,error;
 for(int i=0;i<12;++i){
  trusted=created=download=flush=opened=hash=signature=publisher=moved=true;cancel=false;handles=deletes=trustCalls=moves=gets=0;
  auto candidate=asset;
  if(i==0)trusted=false;if(i==1)created=false;if(i==2)download=false;if(i==3)flush=false;
  if(i==4)opened=false;if(i==5)hash=false;if(i==6)signature=false;if(i==7)publisher=false;
  if(i==8)moved=false;if(i==9)cancel=true;if(i==10)candidate.url="https://evil.invalid/GlassSetup.exe";
  prepared=L"stale";error=L"stale";
  const bool ok=PrepareSetup(candidate,L"private-workspace",cancel,prepared,error);
  assert(ok==(i==11));assert(handles==0);assert(trustCalls==0||trustCalls==2);
  if(ok){assert(prepared==L"private-workspace\\GlassSetup.exe");assert(error.empty());assert(moves==1);}
  else {assert(prepared.empty());assert(!error.empty());}
  if(i==0||i==9||i==10)assert(gets==0&&moves==0);
  if(i==5||i==6||i==7)assert(moves==0&&deletes==1);
 }
}
'''
with tempfile.TemporaryDirectory(prefix='GlassSetupDownload-') as temp:
    path=Path(temp)/'test.cpp';path.write_text(prefix+function+tests)
    exe=Path(temp)/'test'
    subprocess.run(['g++','-std=c++17','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-I'+str(root),str(path),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('PASS: production Setup preparation: metadata/ACL/create/download/flush/open/hash/AuthentiCode/publisher/move/cancel gates, handle cleanup; Checking route and no old Update page. Fake APIs, not Windows integration.')
