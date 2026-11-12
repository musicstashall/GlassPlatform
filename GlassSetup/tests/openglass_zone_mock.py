"""Compile the production ADS-removal helper with fake Win32 stream APIs.
This is not NTFS/SmartScreen/Windows runtime verification.
"""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'GlassSetup/src/InstallSupport/GlassOpenGlassPackage.cpp').read_text(encoding='utf-8')
a=source.index('void RemoveInternetZone(')
b=source.index('std::array<unsigned char,32> Hash(',a)
helper=source[a:b]
harness=r'''
#include <cassert>
#include <string>
#include <stdexcept>
using DWORD=unsigned;
using HANDLE=int;
constexpr HANDLE INVALID_HANDLE_VALUE=-1;
constexpr DWORD ERROR_FILE_NOT_FOUND=2,ERROR_PATH_NOT_FOUND=3,ERROR_ACCESS_DENIED=5;
constexpr unsigned FILE_READ_ATTRIBUTES=128,FILE_SHARE_READ=1,FILE_SHARE_WRITE=2,FILE_SHARE_DELETE=4,OPEN_EXISTING=3,FILE_FLAG_OPEN_REPARSE_POINT=0x200000;
DWORD lastError=0,deleteError=0,probeError=0;
bool streamExists=true,ignoreDeletion=false;
unsigned deleteCalls=0,probeCalls=0,closeCalls=0;
const std::wstring expected=L"C:\\Protected\\OpenGlassSetup.Signed.exe:Zone.Identifier";
DWORD GetLastError(){return lastError;}
bool DeleteFileW(const wchar_t* path){
 assert(std::wstring(path)==expected);++deleteCalls;
 if(deleteError){lastError=deleteError;return false;}
 if(!streamExists){lastError=ERROR_FILE_NOT_FOUND;return false;}
 if(!ignoreDeletion)streamExists=false;return true;
}
HANDLE CreateFileW(const wchar_t* path,unsigned access,unsigned share,void*,unsigned creation,unsigned flags,void*){
 assert(std::wstring(path)==expected);++probeCalls;
 assert(access==FILE_READ_ATTRIBUTES&&share==7&&creation==OPEN_EXISTING&&flags==FILE_FLAG_OPEN_REPARSE_POINT);
 if(probeError){lastError=probeError;return INVALID_HANDLE_VALUE;}
 if(!streamExists){lastError=ERROR_FILE_NOT_FOUND;return INVALID_HANDLE_VALUE;}
 return 10;
}
struct File {HANDLE h=INVALID_HANDLE_VALUE;~File(){if(h!=INVALID_HANDLE_VALUE)++closeCalls;}};
// HELPER
bool succeeds(){try{RemoveInternetZone(L"C:\\Protected\\OpenGlassSetup.Signed.exe");return true;}catch(const std::runtime_error&){return false;}}
void reset(){lastError=deleteError=probeError=0;streamExists=true;ignoreDeletion=false;deleteCalls=probeCalls=closeCalls=0;}
int main(){
 reset();assert(succeeds()&&!streamExists&&deleteCalls==1&&probeCalls==1);
 reset();streamExists=false;assert(succeeds()&&probeCalls==1); // freshly created files need no ADS
 reset();deleteError=ERROR_PATH_NOT_FOUND;streamExists=false;assert(succeeds());
 for(DWORD error:{5u,32u,123u}){reset();deleteError=error;assert(!succeeds()&&probeCalls==0&&streamExists);}
 reset();ignoreDeletion=true;assert(!succeeds()&&closeCalls==1); // do not trust deletion success alone
 reset();probeError=ERROR_ACCESS_DENIED;assert(!succeeds()); // absence probe must fail closed
}
'''.replace('// HELPER',helper)
with tempfile.TemporaryDirectory(prefix='OpenGlassZone-') as temp:
    cpp=Path(temp)/'test.cpp';exe=Path(temp)/'test'
    cpp.write_text(harness,encoding='utf-8')
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
prepare=source[source.index('bool Prepare(const std::wstring& workspace'):]
assert prepare.index('(void)VerifyPublisher(setup,verified.h)') < prepare.index('RemoveInternetZone(setup)')
assert prepare.index('RemoveInternetZone(setup)') < prepare.index('const auto publisher=VerifyPublisher(setup,pinned.h)')
assert prepare.index('RemoveInternetZone(setup)') < prepare.index('GlassInnoExtract::Extract(')
print('PASS: actual OpenGlass Zone.Identifier helper: present/absent/delete failure/persistent stream/inaccessible probe; verification order source contract')
