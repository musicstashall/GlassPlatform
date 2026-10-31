#!/usr/bin/env python3
"""Exercise the real validated IRegisteredTask::Stop adapter with COM-shaped mocks."""
from pathlib import Path
import os
import subprocess
import tempfile

repo=Path(__file__).resolve().parents[2]
source=(repo/'GlassSetup/src/InstallSupport/GlassInstallRemoval.cpp').read_text(encoding='utf-8')
def function(token):
    start=source.index(token);brace=source.index('{',start);depth=0
    for i in range(brace,len(source)):
        if source[i]=='{':depth+=1
        if source[i]=='}':
            depth-=1
            if not depth:return source[start:i+1]
    raise ValueError(token)
impl=function('bool StopValidatedPlatformTask(')
assert 'result!=S_OK' in impl and 'result==S_FALSE' in impl
prefix=r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
using DWORD=std::uint32_t;using HRESULT=std::int32_t;using LONG=long;
constexpr HRESULT S_OK=0,S_FALSE=1,E_FAIL=static_cast<HRESULT>(0x80004005u);
constexpr DWORD ERROR_CANCELLED=1223,ERROR_ACCESS_DENIED=5,ERROR_GEN_FAILURE=31;
#define FAILED(hr) ((hr)<0)
struct Fake{bool valid=true,com=true,safe=true,connect=true,folder=true,task=true,definition=true,
 action=true,instances=true;LONG count=1;HRESULT result=S_OK;int stops=0;DWORD error=0;std::wstring expected;} f;
void SetLastError(DWORD e){f.error=e;}
namespace GP {enum class PlatformArch{X64,X86};const wchar_t* GlassAutorunTaskName(PlatformArch){return L"Glass task";}}
struct ITaskService{};struct ITaskDefinition{};
struct IRunningTaskCollection{HRESULT get_Count(LONG* n){*n=f.count;return S_OK;}};
struct IRegisteredTask{
 HRESULT get_Definition(ITaskDefinition** p){static ITaskDefinition d;*p=f.definition?&d:nullptr;return f.definition?S_OK:E_FAIL;}
 HRESULT GetInstances(int,IRunningTaskCollection** p){static IRunningTaskCollection c;*p=f.instances?&c:nullptr;return f.instances?S_OK:E_FAIL;}
 HRESULT Stop(int flags){assert(flags==0);++f.stops;return f.result;}
};
struct ITaskFolder{HRESULT GetTask(const wchar_t*,IRegisteredTask** p){static IRegisteredTask t;*p=f.task?&t:nullptr;return f.task?S_OK:E_FAIL;}};
template<class T>struct ComPtr{T* p=nullptr;T* Get(){return p;}T** GetAddressOf(){return &p;}
 T* operator->(){return p;}explicit operator bool()const{return p!=nullptr;}};
struct ComGuard{bool ok(){return f.com;}};struct Bstr{const wchar_t* value;explicit Bstr(const wchar_t* p):value(p){}};
bool Connect(ComPtr<ITaskService>& p){static ITaskService x;p.p=f.connect?&x:nullptr;return f.connect;}
bool GetFolder(ITaskService*,const wchar_t*,ComPtr<ITaskFolder>& p){static ITaskFolder x;p.p=f.folder?&x:nullptr;return f.folder;}
bool Cancelled(const std::atomic_bool* p){return p && p->load();}
bool ExpectedAction(ITaskDefinition*,bool,const std::wstring& expected,GP::PlatformArch){f.expected=expected;return f.action;}
std::wstring Problem(bool,const wchar_t*,const wchar_t* en,const wchar_t*,unsigned=0){return en;}
namespace GlassInstallLog{template<class... T>void LineFormat(const wchar_t*,T...) {}}
namespace GlassInstallUtil{
 std::wstring TrimTrailingSlashes(std::wstring s){while(!s.empty() && s.back()==L'\\')s.pop_back();return s;}
 bool IsSafeInstallPath(const std::wstring&){return f.safe;}
}
namespace GlassInstallRemoval{
 bool ValidatePlatformTask(GP::PlatformArch,const std::wstring&,bool,bool,bool,std::wstring& e){
  if(!f.valid)e=L"prior validation failed";return f.valid;}
'''
main=r'''
}
int main(){auto reset=[](){f=Fake{};};bool attempted=false;std::wstring detail;std::atomic_bool cancel{false};
 auto run=[&](){return GlassInstallRemoval::StopValidatedPlatformTask(GP::PlatformArch::X64,L"D:\\Glass\\",&cancel,attempted,false,detail);};
 reset();assert(run() && attempted && f.stops==1 && f.expected==L"D:\\Glass\\GlassCompat.dll");
 reset();f.result=S_FALSE;assert(!run() && attempted && f.stops==1 && f.error==ERROR_ACCESS_DENIED && !detail.empty());
 reset();f.result=E_FAIL;assert(!run() && attempted && f.stops==1 && f.error==ERROR_GEN_FAILURE);
 reset();f.valid=false;assert(!run() && !attempted && f.stops==0 && detail==L"prior validation failed");
 reset();f.com=false;assert(!run() && !attempted && f.stops==0);
 reset();f.safe=false;assert(!run() && !attempted && f.stops==0);
 reset();f.action=false;assert(!run() && !attempted && f.stops==0);
 for(LONG n:{0L,2L}){reset();f.count=n;assert(!run() && !attempted && f.stops==0);}
 reset();cancel=true;assert(!run() && !attempted && f.stops==0 && f.error==ERROR_CANCELLED);
 std::cout<<"PASS: real COM stop adapter validates task/action/instance and rejects S_FALSE\n";
}
'''
with tempfile.TemporaryDirectory(prefix='glass-scheduler-stop-') as d:
    cpp=Path(d)/'test.cpp';exe=Path(d)/'test';cpp.write_text(prefix+impl+main,encoding='utf-8')
    subprocess.run([os.environ.get('CXX','g++'),'-std=c++17','-g','-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer','-no-pie',str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
