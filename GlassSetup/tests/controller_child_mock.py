"""Actual child-side validation and commit barrier with mocked Win32 handles."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassMaintenanceLaunch.cpp').read_text()
def fn(token):
 a=s.index(token);b=s.index('{',a);d=0
 for i in range(b,len(s)):
  d+=(s[i]=='{')-(s[i]=='}')
  if not d:return s[a:i+1]
 raise ValueError(token)
h=r'''
#include <cassert>
#include <string>
#include <vector>
#include <cwchar>
#include <cstdint>
#include <cstdlib>
#include <iostream>
using HANDLE=void*;using DWORD=unsigned;using ULONGLONG=unsigned long long;using LPWSTR=wchar_t*;
constexpr int FALSE=0,WAIT_TIMEOUT=258,WAIT_OBJECT_0=0,HANDLE_FLAG_INHERIT=1;
HANDLE INVALID_HANDLE_VALUE=reinterpret_cast<HANDLE>(-1);
bool parentAlive=true,signedParent=true,correctImage=true,readyOk=true,committed=true,validHandles=true;DWORD parentPid=10;int readySignals=0,inheritClears=0;
std::vector<std::wstring> args;
LPWSTR* CommandLineToArgvW(const wchar_t*,int* count){static std::vector<LPWSTR> p;p.clear();for(auto& a:args)p.push_back(a.data());*count=static_cast<int>(p.size());return p.data();}
const wchar_t* GetCommandLineW(){return L"";}void LocalFree(void*){}
bool GetHandleInformation(HANDLE h,DWORD* flags){*flags=HANDLE_FLAG_INHERIT;return validHandles&&(h==reinterpret_cast<HANDLE>(11)||h==reinterpret_cast<HANDLE>(12)||h==reinterpret_cast<HANDLE>(13));}
ULONGLONG _wcstoui64(const wchar_t* text,wchar_t** end,int base){return std::wcstoull(text,end,base);}
bool CloseHandle(HANDLE){return true;}
DWORD GetCurrentProcessId(){return 20;}DWORD ParentProcessId(DWORD){return 10;}
DWORD GetProcessId(HANDLE){return parentPid;}
DWORD WaitForSingleObject(HANDLE,int){return parentAlive?WAIT_TIMEOUT:0;}
bool QueryFullProcessImageNameW(HANDLE,int,wchar_t* out,DWORD*){std::wcscpy(out,L"parent.exe");return true;}
bool ControllerTrusted(const std::wstring&){return signedParent;}
bool SetHandleInformation(HANDLE,int,int){++inheritClears;return true;}
std::wstring ModulePath(){return correctImage?L"canonical.exe":L"temp.exe";}
bool PathsEqualCanonical(const std::wstring& a,const std::wstring& b){return a==b;}
namespace GlassSetupSourceLayout {std::wstring directory;}
namespace GlassMaintenanceCache {
std::wstring ControllerFileForRecord(const std::string& record,const wchar_t*){return record==std::string(128,'a')?L"canonical.exe":L"";}
std::string ReadControllerRecord(const wchar_t* =L"Active"){return committed?std::string(128,'a'):std::string(128,'b');}
}
bool ResetEvent(HANDLE){return true;}
bool IsControllerChild();
bool SetEvent(HANDLE h){assert(h==reinterpret_cast<HANDLE>(12));++readySignals;return readyOk;}
DWORD WaitForMultipleObjects(int count,HANDLE* waits,int,int timeout){assert(count==2&&timeout==30000&&waits[0]==reinterpret_cast<HANDLE>(13));return committed?WAIT_OBJECT_0:258;}
// HANDLE
// STATE
// PARSE
// BEGIN
// COMPLETE
bool IsControllerChild(){return controllerChild.validated;}
// CONFIRM
void reset(){controllerChild.validated=controllerChild.modify=controllerChild.startupAckRequired=false;controllerChild.record.clear();controllerChild.parent.value=controllerChild.ready.value=controllerChild.commit.value=nullptr;
 parentAlive=signedParent=correctImage=readyOk=committed=validHandles=true;parentPid=10;readySignals=inheritClears=0;GlassSetupSourceLayout::directory.clear();
 args={L"setup",L"--controller-child",L"--controller-parent",L"11",L"--controller-ready",L"12",L"--controller-commit",L"13",L"--controller-record",std::wstring(128,'a'),L"--source-layout",L"D:\\original",L"--modify",L"--controller-startup-ack"};}
int main(){std::wstring error;
 reset();assert(BeginControllerChild(error));assert(controllerChild.validated&&controllerChild.modify&&inheritClears==3&&GlassSetupSourceLayout::directory==L"D:\\original");
 assert(CompleteControllerChild(true)&&readySignals==1);assert(ConfirmControllerStartup(true)&&readySignals==2);
 for(int failure=0;failure<8;++failure){reset();
  if(failure==0)parentAlive=false;
  if(failure==1)signedParent=false;
  if(failure==2)parentPid=99;
  if(failure==3)correctImage=false;
  if(failure==4)validHandles=false;
  if(failure==5)args[3]=L"-1";
  if(failure==6)args[9]=std::wstring(128,wchar_t(0x161));
  if(failure==7)args[9]=L"short";
  assert(!BeginControllerChild(error)&&!controllerChild.validated&&readySignals==0);}
 reset();assert(BeginControllerChild(error));assert(!CompleteControllerChild(false)&&readySignals==0);
 reset();assert(BeginControllerChild(error));committed=false;assert(!CompleteControllerChild(true));
 reset();args.pop_back();assert(BeginControllerChild(error)&&!controllerChild.startupAckRequired);assert(CompleteControllerChild(true)&&ConfirmControllerStartup(true)&&readySignals==1); // Validated legacy parent uses v1 barrier.
 reset();args={L"setup"};assert(BeginControllerChild(error)&&!controllerChild.validated&&CompleteControllerChild(true));
 std::cout<<"PASS: actual child handoff: inherited-handle/real-parent/publisher/canonical-image validation, strict record, source hint, READY/COMMIT barrier and timeout refusal\n";
}
'''
state=s[s.index('struct ControllerChildState {'):s.index('struct ControllerLock {')]
h=h.replace('// HANDLE',fn('struct ControllerHandle ')+ ';').replace('// STATE',state).replace('// PARSE',fn('bool ParseControllerHandle(')).replace('// BEGIN',fn('bool BeginControllerChild(')).replace('// COMPLETE',fn('bool CompleteControllerChild(')).replace('// CONFIRM',fn('bool ConfirmControllerStartup('))
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';p.write_text(h);exe=Path(d)/'test'
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
