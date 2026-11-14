"""Actual final service deletion with SCM seams; not Windows integration."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassInstallRemoval.cpp').read_text()
body=s[s.index('bool DeleteServiceLast('):s.index('\nbool RemoveTask(',s.index('bool DeleteServiceLast('))] if '\nbool RemoveTask(' in s[s.index('bool DeleteServiceLast('):] else ''
# Extract only the function, not the following unrelated task helpers.
a=s.index('bool DeleteServiceLast(');b=s.index('{',a);depth=1;i=b+1
while depth:
 depth+=(s[i]=='{')-(s[i]=='}');i+=1
body=s[a:i]
h=r'''
#include <cassert>
#include <string>
#include <iostream>
using SC_HANDLE=int;using DWORD=unsigned;using BOOL=int;using BYTE=unsigned char;
constexpr unsigned SC_MANAGER_CONNECT=1,SERVICE_QUERY_CONFIG=2,SERVICE_QUERY_STATUS=4,DELETE=8,
 ERROR_SUCCESS=0,ERROR_SERVICE_DOES_NOT_EXIST=1060,SC_STATUS_PROCESS_INFO=0,SERVICE_STOPPED=1,FALSE=0;
struct SERVICE_STATUS_PROCESS {unsigned dwCurrentState=0;};
struct Session {bool serviceExisted=true;std::wstring expectedServiceExe=L"expected",serviceConfiguration=L"original";};
struct State {unsigned error=1060,status=SERVICE_STOPPED;bool scm=true,present=true,matches=true,configRead=true,statusRead=true,deleted=true;
 unsigned deletes=0;}state;
DWORD GetLastError(){return state.error;}
SC_HANDLE OpenSCManagerW(const void*,const void*,unsigned){return state.scm?1:0;}
SC_HANDLE OpenServiceW(SC_HANDLE,const wchar_t*,unsigned){return state.present?2:0;}
void CloseServiceHandle(SC_HANDLE){}
bool QueryServiceStatusEx(SC_HANDLE,unsigned,BYTE* bytes,unsigned,DWORD*){
 reinterpret_cast<SERVICE_STATUS_PROCESS*>(bytes)->dwCurrentState=state.status;return state.statusRead;}
bool DeleteService(SC_HANDLE){++state.deletes;return state.deleted;}
bool ReadServiceConfiguration(SC_HANDLE,const std::wstring&,std::wstring& out){out=state.matches?L"original":L"foreign";return state.configRead;}
std::wstring Problem(bool,const wchar_t*,const wchar_t*,const std::wstring& ={},unsigned=0){return L"error";}
// BODY
int main(){Session s;std::wstring error;
 {state={};assert(DeleteServiceLast(s,false,error));assert(state.deletes==1);}
 {state={};state.present=false;assert(DeleteServiceLast(s,false,error));assert(state.deletes==0);}
 {state={};state.present=false;state.error=5;assert(!DeleteServiceLast(s,false,error));assert(state.deletes==0);}
 {state={};state.matches=false;assert(!DeleteServiceLast(s,false,error));assert(state.deletes==0);}
 {state={};state.status=4;assert(!DeleteServiceLast(s,false,error));assert(state.deletes==0);}
 {state={};s.serviceExisted=false;assert(!DeleteServiceLast(s,false,error));assert(state.deletes==0);s.serviceExisted=true;}
 {state={};state.configRead=false;assert(!DeleteServiceLast(s,false,error));assert(state.deletes==0);}
 {state={};state.deleted=false;assert(!DeleteServiceLast(s,false,error));}
 std::cout<<"PASS: actual final service deletion: already absent 1060 succeeds, changed/new/running/unreadable objects refused\n";
}
'''.replace('// BODY',body)
with tempfile.TemporaryDirectory(prefix='ServiceRemove-') as d:
 p=Path(d)/'test.cpp';exe=Path(d)/'test';p.write_text(h)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
