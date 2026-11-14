#!/usr/bin/env python3
"""Exercise actual OpenGlass process/service/Task Scheduler probes with a fake COM tree."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "GlassSetup/src/InstallSupport/GlassInstallUtil.cpp").read_text(encoding="utf-8")
start = source.index("namespace {\nusing Microsoft::WRL::ComPtr;")
end = source.index("bool QueryOpenGlassFiles(", start)
implementation = source[start:end]
prefix = r'''
#include <cassert>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <iostream>
#include <string>
#include <vector>
using DWORD=std::uint32_t;using BYTE=unsigned char;using LONG=long;using BOOL=int;using HRESULT=std::int32_t;
using VARIANT_BOOL=short;using BSTR=wchar_t*;using SC_HANDLE=void*;
#define _wcsicmp wcscasecmp
constexpr BOOL TRUE=1,FALSE=0;constexpr VARIANT_BOOL VARIANT_FALSE=0,VARIANT_TRUE=-1;
constexpr HRESULT S_OK=0,S_FALSE=1,E_FAIL=-1,E_OUTOFMEMORY=-2,RPC_E_CHANGED_MODE=-3;
constexpr DWORD ERROR_SUCCESS=0,ERROR_GEN_FAILURE=31,ERROR_SERVICE_DOES_NOT_EXIST=1060,ERROR_ACCESS_DENIED=5,
 ERROR_CANCELLED=1223,ERROR_INVALID_DATA=13;
constexpr int COINIT_MULTITHREADED=0,CLSCTX_INPROC_SERVER=1,CLSID_TaskScheduler=1,IID_ITaskService=2,
 FACILITY_WIN32=7,TASK_ENUM_HIDDEN=1,VT_I4=3,SC_MANAGER_CONNECT=1,SERVICE_QUERY_STATUS=2,
 SC_STATUS_PROCESS_INFO=0,SERVICE_STOPPED=1,SERVICE_RUNNING=4,SERVICE_STOP_PENDING=3;
bool SUCCEEDED(HRESULT hr){return hr>=0;}bool FAILED(HRESULT hr){return hr<0;}
int HRESULT_FACILITY(HRESULT){return 0;}DWORD HRESULT_CODE(HRESULT){return 0;}
struct VARIANT {int vt=0;LONG lVal=0;};void VariantInit(VARIANT* v){*v={};}
struct SERVICE_STATUS_PROCESS {DWORD dwCurrentState=SERVICE_STOPPED;};
static DWORD lastError=0;void SetLastError(DWORD n){lastError=n;}DWORD GetLastError(){return lastError;}
BSTR SysAllocString(const wchar_t* value){if(!value)return nullptr;auto n=std::wcslen(value)+1;
 auto* out=static_cast<wchar_t*>(std::malloc(n*sizeof(wchar_t)));if(out)std::wmemcpy(out,value,n);return out;}
void SysFreeString(BSTR value){std::free(value);}
namespace Microsoft::WRL {
 template<class T>struct ComPtr{T* value=nullptr;T** GetAddressOf(){return &value;}T* Get()const{return value;}
  T* operator->()const{return value;}explicit operator bool()const{return value!=nullptr;}};
}
struct IRunningTaskCollection{LONG count=0;bool fail=false;HRESULT get_Count(LONG* out){if(fail)return E_FAIL;*out=count;return S_OK;}};
struct IRunningTask{};
struct IRegisteredTask{
 std::wstring name;VARIANT_BOOL enabled=VARIANT_TRUE;IRunningTaskCollection instances;IRunningTask running;
 bool failName=false,failEnabled=false,failInstances=false,failStop=false,disableOnSecondCheck=false;int checks=0,runCalls=0,stopCalls=0;
 HRESULT get_Name(BSTR* out){if(failName)return E_FAIL;*out=SysAllocString(name.c_str());return *out?S_OK:E_OUTOFMEMORY;}
 HRESULT get_Enabled(VARIANT_BOOL* out){if(failEnabled)return E_FAIL;if(++checks==2 && disableOnSecondCheck)enabled=VARIANT_FALSE;*out=enabled;return S_OK;}
 HRESULT GetInstances(LONG flags,IRunningTaskCollection** out){assert(flags==0);if(failInstances)return E_FAIL;*out=&instances;return S_OK;}
 HRESULT Run(VARIANT,IRunningTask** out){++runCalls;if(enabled==VARIANT_FALSE)return E_FAIL;instances.count=1;*out=&running;return S_OK;}
 HRESULT Stop(LONG flags){assert(flags==0);++stopCalls;if(failStop)return E_FAIL;instances.count=0;return S_OK;}
};
struct ITaskFolder;
struct IRegisteredTaskCollection{
 std::vector<IRegisteredTask*> items;bool fail=false;
 HRESULT get_Count(LONG* out){if(fail)return E_FAIL;*out=static_cast<LONG>(items.size());return S_OK;}
 HRESULT get_Item(VARIANT v,IRegisteredTask** out){if(fail || v.lVal<1 || std::size_t(v.lVal)>items.size())return E_FAIL;*out=items[v.lVal-1];return S_OK;}
};
struct ITaskFolderCollection{
 std::vector<ITaskFolder*> items;bool fail=false;
 HRESULT get_Count(LONG* out){if(fail)return E_FAIL;*out=static_cast<LONG>(items.size());return S_OK;}
 HRESULT get_Item(VARIANT v,ITaskFolder** out){if(fail || v.lVal<1 || std::size_t(v.lVal)>items.size())return E_FAIL;*out=items[v.lVal-1];return S_OK;}
};
struct ITaskFolder{
 IRegisteredTaskCollection tasks;ITaskFolderCollection children;bool failTasks=false,failChildren=false;
 HRESULT GetTasks(LONG, IRegisteredTaskCollection** out){if(failTasks)return E_FAIL;*out=&tasks;return S_OK;}
 HRESULT GetFolders(LONG, ITaskFolderCollection** out){if(failChildren)return E_FAIL;*out=&children;return S_OK;}
};
struct ITaskService{
 ITaskFolder* root=nullptr;bool failConnect=false,failRoot=false;
 HRESULT Connect(VARIANT,VARIANT,VARIANT,VARIANT){return failConnect?E_FAIL:S_OK;}
 HRESULT GetFolder(BSTR path,ITaskFolder** out){if(failRoot || !path)return E_FAIL;*out=root;return S_OK;}
};
struct Scenario{
 bool processKnown=true,process=false,scmAvailable=true,serviceExists=false,serviceAccess=true,serviceQuery=true,window=false,createCom=true;
 DWORD serviceState=SERVICE_STOPPED;HRESULT comResult=S_OK;int initCalls=0,uninitCalls=0;
 ITaskFolder root;ITaskService service;
 Scenario(){service.root=&root;}
} scenario;
HRESULT CoInitializeEx(void*,DWORD){++scenario.initCalls;return scenario.comResult;}
void CoUninitialize(){++scenario.uninitCalls;}
HRESULT CoCreateInstance(int,void*,int,int,void** out){if(!scenario.createCom)return E_FAIL;*out=&scenario.service;return S_OK;}
SC_HANDLE OpenSCManagerW(void*,void*,DWORD){if(!scenario.scmAvailable){lastError=ERROR_ACCESS_DENIED;return nullptr;}return reinterpret_cast<void*>(1);}
SC_HANDLE OpenServiceW(SC_HANDLE,const wchar_t*,DWORD){
 if(!scenario.serviceAccess){lastError=ERROR_ACCESS_DENIED;return nullptr;}
 if(!scenario.serviceExists){lastError=ERROR_SERVICE_DOES_NOT_EXIST;return nullptr;}return reinterpret_cast<void*>(2);
}
BOOL CloseServiceHandle(SC_HANDLE){return TRUE;}
BOOL QueryServiceStatusEx(SC_HANDLE,int,BYTE* out,DWORD,DWORD*){
 if(!scenario.serviceQuery){lastError=ERROR_ACCESS_DENIED;return FALSE;}
 reinterpret_cast<SERVICE_STATUS_PROCESS*>(out)->dwCurrentState=scenario.serviceState;return TRUE;
}
void* FindWindowW(const wchar_t*,const wchar_t*){return scenario.window?reinterpret_cast<void*>(1):nullptr;}
namespace GlassInstallUtil {
 struct OpenGlassTaskState{bool found=false,anyDisabled=false,anyRunning=false,multipleTasks=false,multipleRunningInstances=false;};
 bool QueryImageRunning(const wchar_t*,bool& found){found=scenario.process;return scenario.processKnown;}
'''
main = r'''
}
using namespace GlassInstallUtil;
void reset(){scenario=Scenario{};scenario.service.root=&scenario.root;lastError=0;}
int main(){
 bool running=true;OpenGlassTaskState state;
 reset();assert(QueryOpenGlassRunning(running,nullptr));assert(!running);assert(scenario.initCalls==1 && scenario.uninitCalls==1);
 reset();scenario.service.failConnect=true;assert(!QueryOpenGlassRunning(running,nullptr));
 reset();scenario.process=true;scenario.service.failConnect=true;assert(!QueryOpenGlassRunning(running,nullptr));
 reset();scenario.process=true;assert(QueryOpenGlassRunning(running,nullptr) && running); // no managed backend
 reset();scenario.processKnown=false;assert(!QueryOpenGlassRunning(running,nullptr));
 reset();scenario.process=true;scenario.serviceExists=true;scenario.serviceState=SERVICE_STOPPED;
 assert(QueryOpenGlassRunning(running,nullptr) && !running); // stopped managed service is authoritative
 reset();scenario.serviceExists=true;scenario.serviceState=SERVICE_RUNNING;scenario.service.failConnect=true;
 assert(QueryOpenGlassRunning(running,nullptr) && running);
 reset();scenario.serviceAccess=false;assert(!QueryOpenGlassRunning(running,nullptr));
 reset();scenario.serviceExists=true;scenario.serviceState=SERVICE_STOP_PENDING;assert(QueryOpenGlassRunning(running,nullptr) && running);
 reset();scenario.serviceExists=true;scenario.serviceQuery=false;assert(!QueryOpenGlassRunning(running,nullptr));
 reset();ITaskFolder child;IRegisteredTask og;og.name=L"OpenGlass Legacy Host";og.enabled=VARIANT_FALSE;og.instances.count=1;
 child.tasks.items.push_back(&og);scenario.root.children.items.push_back(&child);
 assert(QueryOpenGlassTasks(state));assert(state.found && state.anyDisabled && state.anyRunning);
 assert(QueryOpenGlassRunning(running,&state) && running);
 og.instances.count=0;assert(QueryOpenGlassTasks(state));assert(state.found && state.anyDisabled && !state.anyRunning);
 assert(QueryOpenGlassRunning(running,&state) && !running);
 assert(!RunEnabledOpenGlassTask() && og.runCalls==0 && og.enabled==VARIANT_FALSE);
 og.enabled=VARIANT_TRUE;og.checks=0;assert(RunEnabledOpenGlassTask() && og.runCalls==1 && og.instances.count==1);
 assert(RunEnabledOpenGlassTask() && og.runCalls==1); // already running
 og.instances.count=0;og.checks=0;og.disableOnSecondCheck=true;
 assert(!RunEnabledOpenGlassTask() && og.runCalls==1 && og.enabled==VARIANT_FALSE);
 og.failInstances=true;assert(!QueryOpenGlassTasks(state));
 reset();scenario.root.failChildren=true;assert(!QueryOpenGlassTasks(state));
 reset();ITaskFolder nested;IRegisteredTask legacy,current;legacy.name=L"OpenGlass Legacy Host";current.name=L"OpenGlass Host";
 scenario.root.tasks.items.push_back(&legacy);nested.tasks.items.push_back(&current);scenario.root.children.items.push_back(&nested);
 assert(RunEnabledOpenGlassTask() && current.runCalls==1 && legacy.runCalls==0);
 reset();scenario.comResult=S_FALSE;assert(QueryOpenGlassTasks(state));assert(scenario.initCalls==1 && scenario.uninitCalls==1);
 reset();scenario.comResult=RPC_E_CHANGED_MODE;assert(QueryOpenGlassTasks(state));assert(scenario.uninitCalls==0);
 reset();scenario.window=true;assert(QueryOpenGlassRunning(running,nullptr) && !running); // DWM is not OpenGlass
 reset();scenario.process=true;ITaskFolder stoppedFolder;IRegisteredTask stoppedTask;stoppedTask.name=L"OpenGlass Host";
 stoppedFolder.tasks.items.push_back(&stoppedTask);scenario.root.children.items.push_back(&stoppedFolder);
 assert(QueryOpenGlassRunning(running,nullptr) && !running); // stopped legacy task is authoritative
 bool requested=false;std::atomic_bool cancel=false;
 assert(StopRunningOpenGlassTasks(requested,&cancel) && !requested && stoppedTask.stopCalls==0);
 stoppedTask.instances.count=1;stoppedTask.enabled=VARIANT_FALSE;
 assert(StopRunningOpenGlassTasks(requested,&cancel) && requested && stoppedTask.stopCalls==1);
 assert(stoppedTask.instances.count==0 && stoppedTask.enabled==VARIANT_FALSE);
 stoppedTask.instances.count=2;assert(!StopRunningOpenGlassTasks(requested,&cancel) && !requested);
 assert(stoppedTask.stopCalls==1 && lastError==ERROR_INVALID_DATA);
 stoppedTask.instances.count=1;IRegisteredTask second;second.name=L"OpenGlass Legacy Host";
 scenario.root.tasks.items.push_back(&second);assert(!StopRunningOpenGlassTasks(requested,&cancel));
 assert(stoppedTask.stopCalls==1 && lastError==ERROR_INVALID_DATA);
 scenario.root.tasks.items.clear();cancel.store(true);assert(!StopRunningOpenGlassTasks(requested,&cancel));
 assert(!requested && lastError==ERROR_CANCELLED && stoppedTask.stopCalls==1);
 cancel.store(false);stoppedTask.failStop=true;assert(!StopRunningOpenGlassTasks(requested,&cancel));assert(requested && stoppedTask.stopCalls==2);
 std::cout<<"PASS: real OpenGlass status, stopped-service/task handling and safe legacy-task stop\n";
}
'''
with tempfile.TemporaryDirectory(prefix="glass-og-test-") as directory:
    cpp = Path(directory) / "og.cpp"
    exe = Path(directory) / "og-test"
    cpp.write_text(prefix + implementation + main, encoding="utf-8")
    compiler = os.environ.get("CXX", "g++")
    subprocess.run([compiler, "-std=c++17", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
