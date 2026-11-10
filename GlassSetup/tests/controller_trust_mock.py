"""Actual controller execution trust gate: publisher match alone is insufficient."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassMaintenanceLaunch.cpp').read_text()
a=s.index('bool ControllerTrusted(');b=s.index('ULONGLONG ControllerVersion(',a)
h=r'''
#include <cassert>
#include <string>
#include <iostream>
using DWORD=unsigned;using LONG=long;
constexpr int SCS_32BIT_BINARY=0,SCS_64BIT_BINARY=6,ERROR_SUCCESS=0,WTD_UI_NONE=1,WTD_CHOICE_FILE=2,WTD_STATEACTION_VERIFY=3,WTD_STATEACTION_CLOSE=4,WTD_CACHE_ONLY_URL_RETRIEVAL=8,WTD_REVOCATION_CHECK_NONE=16;
struct GUID{};GUID WINTRUST_ACTION_GENERIC_VERIFY_V2;
struct WINTRUST_FILE_INFO {size_t cbStruct=0;const wchar_t* pcwszFilePath=nullptr;};
struct WINTRUST_DATA {size_t cbStruct=0;int dwUIChoice=0,dwUnionChoice=0;WINTRUST_FILE_INFO* pFile=nullptr;int dwStateAction=0,dwProvFlags=0;};
bool binaryOk=true,matching=true,os64=true;DWORD kind=SCS_32BIT_BINARY;LONG status=0;int verified=0,closed=0;
bool GetBinaryTypeW(const wchar_t*,DWORD* out){*out=kind;return binaryOk;}
namespace GlassProductInspect {bool FileHasSameSignerAsSetup(const std::wstring&){return matching;}}
namespace GlassInstallUtil {bool IsOs64Bit(){return os64;}}
namespace GlassInstallLog {template<class... T>void LineFormat(const wchar_t*,T...) {}}
LONG WinVerifyTrust(void*,GUID*,WINTRUST_DATA* data){if(data->dwStateAction==WTD_STATEACTION_CLOSE){++closed;return 0;}
 assert(data->pFile&&data->dwUIChoice==WTD_UI_NONE&&data->dwUnionChoice==WTD_CHOICE_FILE);
 assert(data->dwProvFlags==(WTD_CACHE_ONLY_URL_RETRIEVAL|WTD_REVOCATION_CHECK_NONE));++verified;return status;}
using BYTE=unsigned char;using HMODULE=void*;
constexpr int LOAD_LIBRARY_AS_DATAFILE=1,LOAD_LIBRARY_AS_IMAGE_RESOURCE=2,IDR_CONTROLLER_PROTOCOL=312,RT_RCDATA=10;
#define MAKEINTRESOURCEW(x) x
BYTE protocolBytes[]={2,0};bool hasProtocol=true,loadable=true;int unloads=0;unsigned protocolSize=2;
HMODULE LoadLibraryExW(const wchar_t*,void*,int flags){assert(flags==3);return loadable?reinterpret_cast<void*>(1):nullptr;}
void* FindResourceW(HMODULE,int,int){return hasProtocol?reinterpret_cast<void*>(1):nullptr;}
void* LoadResource(HMODULE,void*){return protocolBytes;}void* LockResource(void* p){return p;}
unsigned SizeofResource(HMODULE,void*){return protocolSize;}void FreeLibrary(HMODULE){++unloads;}
// SOURCE
int main(){
 assert(ControllerSupportsStartupAck(L"candidate.exe")&&unloads==1);
 hasProtocol=false;assert(!ControllerSupportsStartupAck(L"old.exe")&&unloads==2);hasProtocol=true;
 protocolBytes[0]=1;assert(!ControllerSupportsStartupAck(L"old.exe"));protocolBytes[0]=2;
 protocolSize=3;assert(!ControllerSupportsStartupAck(L"bad.exe"));protocolSize=2;loadable=false;assert(!ControllerSupportsStartupAck(L"bad.exe"));
 assert(ControllerTrusted(L"candidate.exe")&&verified==1&&closed==1);
 status=-1;assert(!ControllerTrusted(L"candidate.exe")&&verified==2&&closed==2);
 status=0;matching=false;assert(!ControllerTrusted(L"candidate.exe")&&verified==2);matching=true;
 binaryOk=false;assert(!ControllerTrusted(L"candidate.exe"));binaryOk=true;
 kind=99;assert(!ControllerTrusted(L"candidate.exe"));kind=SCS_64BIT_BINARY;os64=false;assert(!ControllerTrusted(L"candidate.exe"));
 os64=true;assert(ControllerTrusted(L"candidate.exe"));assert(!ControllerTrusted(L""));
 std::cout<<"PASS: actual controller execution gate: compatible binary, expected publisher AND successful Authenticode, bounded offline chain verification and provider cleanup\n";
}
'''.replace('// SOURCE',s[a:b])
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';p.write_text(h);exe=Path(d)/'test'
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
