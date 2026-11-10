"""Execute production transition orchestration with fake Win32 process/FS seams.
Does not claim Windows process/UAC/AuthentiCode acceptance.
"""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassMaintenanceLaunch.cpp').read_text()
def fn(token):
 a=s.index(token);b=s.index('{',a);depth=0
 for i in range(b,len(s)):
  depth+=(s[i]=='{')-(s[i]=='}')
  if depth==0:return s[a:i+1]
 raise ValueError(token)
h=r'''
#include <cassert>
#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <iostream>
using HANDLE=int*;using DWORD=unsigned;using BYTE=unsigned char;using SIZE_T=size_t;using ULONGLONG=unsigned long long;
using LPPROC_THREAD_ATTRIBUTE_LIST=void*;
constexpr int FALSE=0,TRUE=1,EXTENDED_STARTUPINFO_PRESENT=1,PROC_THREAD_ATTRIBUTE_HANDLE_LIST=2,SYNCHRONIZE=4,PROCESS_QUERY_LIMITED_INFORMATION=8,WAIT_OBJECT_0=0;
constexpr DWORD INVALID_FILE_ATTRIBUTES=~0u;
HANDLE INVALID_HANDLE_VALUE=reinterpret_cast<HANDLE>(-1);
struct SECURITY_ATTRIBUTES {size_t size;void* sd;int inherit;};
struct STARTUPINFOEXW {struct {size_t cb;} StartupInfo;LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList;};
struct PROCESS_INFORMATION {HANDLE hProcess=nullptr,hThread=nullptr;};
enum class ControllerTransition { Continue,Transferred,Failed };
bool child=false,lockOk=true,stageOk=true,launchOk=true,readyOk=true,eventOk=true,writeOk=true,attrOk=true,ack=false,launched=false,activeExists=false;
bool finalOk=true;int waitsCount=0;int handles=0,launches=0;std::vector<std::wstring> recordsWritten;std::wstring chosen,command;
std::wstring self=L"external\\GlassSetup.exe";
std::map<std::wstring,ULONGLONG> versions;
std::map<std::wstring,bool> trust;std::wstring unsupported;
std::map<std::wstring,std::string> records;
HANDLE handle(){++handles;return new int(1);}bool CloseHandle(HANDLE h){assert(h&&h!=INVALID_HANDLE_VALUE);--handles;delete h;return true;}
// HANDLE
struct ControllerLock {bool acquired=lockOk;};
bool IsControllerChild(){return child;}
std::wstring ModulePath(){return self;}
ULONGLONG ControllerVersion(const std::wstring& p){return versions[p];}
bool ControllerTrusted(const std::wstring& p){return trust[p];}
bool ControllerSupportsStartupAck(const std::wstring& path){return path!=unsupported;}
bool PathsEqualCanonical(const std::wstring& a,const std::wstring& b){return a==b;}
std::string selectedRecord;
namespace GlassSetupSourceLayout {std::wstring directory;}
namespace GlassInstallLog {void Line(const wchar_t*){}}
namespace GlassMaintenanceCache {
std::string ControllerDigest(const std::wstring&){return std::string(64,'a');}
std::wstring Root(){return L"store";}
std::string ReadControllerRecord(const wchar_t* name=L"Active"){return records[name];}
std::wstring ControllerFileForRecord(const std::string& r,const wchar_t*){return r.empty()?L"":L"store\\"+std::wstring(1,wchar_t(r[0]))+L"\\GlassSetup.exe";}
std::wstring ControllerFile(const wchar_t* name){return ControllerFileForRecord(records[L"Active"],name);}
}
bool StageController(const std::wstring& p,std::string& record){chosen=p;record=selectedRecord;versions[GlassMaintenanceCache::ControllerFileForRecord(record,L"GlassSetup.exe")]=versions[p];return stageOk;}
bool WriteControllerRecord(const wchar_t* name,const std::string& r){
 if(std::wstring(name)==L"Active")assert(!launched||ack);
 recordsWritten.emplace_back(name);if(!writeOk)return false;records[name]=r;return true;
}
HANDLE GetCurrentProcess(){return reinterpret_cast<HANDLE>(99);}
HANDLE CreateEventW(SECURITY_ATTRIBUTES* sa,int,int,void*){assert(sa->inherit);return handle();}
bool DuplicateHandle(HANDLE,HANDLE,HANDLE,HANDLE* out,int,int inherit,int){assert(inherit);*out=handle();return true;}
bool InitializeProcThreadAttributeList(void*,int,int,SIZE_T* size){*size=8;return attrOk;}
void DeleteProcThreadAttributeList(void*){}
bool UpdateProcThreadAttribute(void*,int,int,HANDLE*,SIZE_T size,void*,void*){assert(size==3*sizeof(HANDLE));return attrOk;}
DWORD GetFileAttributesW(const wchar_t*){return activeExists?0:INVALID_FILE_ATTRIBUTES;}
bool CreateProcessW(const wchar_t* path,wchar_t* cmd,void*,void*,int inherit,int flags,void*,const wchar_t*,void*,PROCESS_INFORMATION* pi){
 ++launches;command=cmd;assert(inherit&&flags==EXTENDED_STARTUPINFO_PRESENT&&std::wstring(path).find(L"store\\")==0);
 assert(command.find(L"--controller-child")!=std::wstring::npos&&command.find(L"--source-layout")!=std::wstring::npos);
 if(!launchOk)return false;launched=true;pi->hProcess=handle();pi->hThread=handle();return true;
}
DWORD WaitForMultipleObjects(int,HANDLE*,int,int){if(!readyOk||(++waitsCount==2&&!finalOk))return 258;ack=true;return WAIT_OBJECT_0;}
bool SetEvent(HANDLE){assert(ack&&records[L"Active"]==selectedRecord);return eventOk;}
bool DeleteFileW(const wchar_t*){return true;}
bool RecoverControllerTransition(){return true;}
bool ResetEvent(HANDLE){return true;}
// QUOTE
// TRANSITION
void reset(){assert(handles==0);child=false;lockOk=stageOk=launchOk=readyOk=eventOk=writeOk=attrOk=true;ack=launched=activeExists=false;
 unsupported.clear();finalOk=true;waitsCount=0;self=L"external\\GlassSetup.exe";versions.clear();trust.clear();versions[self]=10;trust[self]=true;
 chosen.clear();records.clear();recordsWritten.clear();launches=0;selectedRecord=std::string(128,'a');GlassSetupSourceLayout::directory.clear();}
int main(){std::wstring error;
 reset();assert(EnsureCurrentController(L"",false,L"",error)==ControllerTransition::Transferred);assert(chosen==self&&ack&&records[L"Active"]==selectedRecord&&handles==0);
 reset();versions[L"download.exe"]=20;trust[L"download.exe"]=true;assert(EnsureCurrentController(L"download.exe",true,L"product",error)==ControllerTransition::Transferred);
 assert(chosen==L"download.exe"&&command.find(L"--modify --product")!=std::wstring::npos);
 reset();records[L"Active"]=std::string(128,'b');auto newer=GlassMaintenanceCache::ControllerFile(L"GlassSetup.exe");versions[newer]=30;trust[newer]=true;selectedRecord=records[L"Active"];
 versions[L"download.exe"]=20;trust[L"download.exe"]=true;assert(EnsureCurrentController(L"download.exe",false,L"",error)==ControllerTransition::Transferred);assert(launches==1); // Reuse installed controller, never downgrade to the download.
 reset();records[L"Active"]=std::string(128,'b');auto equal=GlassMaintenanceCache::ControllerFile(L"GlassSetup.exe");versions[equal]=10;trust[equal]=true;selectedRecord=records[L"Active"];
 assert(EnsureCurrentController(L"",false,L"",error)==ControllerTransition::Transferred);assert(records[L"Active"]==std::string(128,'b')); // Equal version keeps installed controller.
 reset();records[L"Active"]=std::string(128,'b');unsupported=GlassMaintenanceCache::ControllerFile(L"GlassSetup.exe");versions[unsupported]=10;trust[unsupported]=true;
 assert(EnsureCurrentController(L"",false,L"",error)==ControllerTransition::Transferred&&chosen==self); // Equal-version v1 migrated, not launched with a v2-only expectation.
 reset();records[L"Active"]=std::string(128,'b');unsupported=GlassMaintenanceCache::ControllerFile(L"GlassSetup.exe");versions[unsupported]=30;trust[unsupported]=true;
 assert(EnsureCurrentController(L"",false,L"",error)==ControllerTransition::Failed&&launches==0); // No downgrade for protocol migration.
 reset();versions[L"evil.exe"]=99;trust[L"evil.exe"]=false;assert(EnsureCurrentController(L"evil.exe",false,L"",error)==ControllerTransition::Failed);assert(launches==0&&recordsWritten.empty());
 for(int failure=0;failure<6;++failure){reset();if(failure==0)lockOk=false;if(failure==1)stageOk=false;if(failure==2)writeOk=false;if(failure==3)attrOk=false;if(failure==4)launchOk=false;if(failure==5)readyOk=false;
  assert(EnsureCurrentController(L"",false,L"",error)==ControllerTransition::Failed);assert(!error.empty()&&records[L"Active"].empty()&&handles==0);}
 reset();eventOk=false;records[L"Active"]=std::string(128,'b');auto previousPath=GlassMaintenanceCache::ControllerFile(L"GlassSetup.exe");versions[previousPath]=9;trust[previousPath]=true;assert(EnsureCurrentController(L"",false,L"",error)==ControllerTransition::Failed);assert(records[L"Active"]==std::string(128,'b'));
 reset();finalOk=false;assert(EnsureCurrentController(L"",false,L"",error)==ControllerTransition::Failed);assert(records[L"Confirmed"].empty()&&records[L"Rejected"]==selectedRecord);
 reset();child=true;assert(EnsureCurrentController(L"",false,L"",error)==ControllerTransition::Continue&&launches==0&&recordsWritten.empty());
 reset();self=L"store\\a\\GlassSetup.exe";versions[self]=10;trust[self]=true;assert(EnsureCurrentController(L"",false,L"",error)==ControllerTransition::Continue&&launches==0);
 assert(ControllerQuote(L"C:\\")==L"\"C:\\\\\""&&ControllerQuote(L"bad\"arg").empty());
 std::cout<<"PASS: actual controller transition: offline publication, newest compatible source, no downgrade/untrusted launch, ACK-before-Active, launch/timeout/commit failures, one hop, intent and handle cleanup\n";
}
'''.replace('// HANDLE',fn('struct ControllerHandle ')+";").replace('// QUOTE',fn('std::wstring ControllerQuote(')).replace('// TRANSITION',fn('ControllerTransition EnsureCurrentController('))
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';p.write_text(h);exe=Path(d)/'test'
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined','-fno-pie','-no-pie',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
