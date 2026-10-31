#!/usr/bin/env python3
"""Exercise real task-action checks with a fake Task Scheduler tree.

The CommandLineToArgvW stub models quoted arguments; test real COM/SCM on Windows.
"""
from pathlib import Path
import os
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[2]
src = (repo / "GlassSetup/src/InstallSupport/GlassInstallRemoval.cpp").read_text(encoding="utf-8")
engine = (repo / "GlassSetup/src/InstallEngine.cpp").read_text(encoding="utf-8")
unins = (repo / "GlassSetup/GlassUnins/main.cpp").read_text(encoding="utf-8")

def function(text, token):
    start = text.index(token); brace = text.index("{", start); depth = 0
    for pos in range(brace, len(text)):
        if text[pos] == "{": depth += 1
        if text[pos] == "}":
            depth -= 1
            if depth == 0: return text[start:pos + 1]
    raise ValueError(token)

args = function(src, "bool ExpectedPlatformArguments(")
action = function(src, "bool ExpectedAction(")
validate = function(src, "bool ValidatePlatformTask(")
for name in ("bool InstallEngine::CapturePriorHostState(", "bool InstallEngine::StopGlassPlatform(",
             "bool InstallEngine::RestorePriorHosts(", "bool InstallEngine::LaunchAndVerifySelectedProducts("):
    assert "ValidatePlatformTask(" in function(engine, name)
for name in ("bool InstallEngine::CapturePriorHostState(", "bool InstallEngine::StopRunningHosts(",
             "bool InstallEngine::RestorePriorHosts(", "bool InstallEngine::LaunchAndVerifySelectedProducts("):
    assert "ValidateOpenGlassPaths(" in function(engine, name)
assert "QueryFullProcessImageNameW(process,0,image.data(),&size)" in function(src, "bool ValidateOpenGlassPaths(")
assert "ValidatePlatformTask(" in function(unins, "int RunOperation(")
assert "ValidateOpenGlassPaths(" in function(unins, "int RunOperation(")

prefix = r'''
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cwchar>
#include <iostream>
#include <string>
#include <vector>
using LONG=long;using HRESULT=int;using BSTR=wchar_t*;using LPWSTR=wchar_t*;
#define _wcsicmp wcscasecmp
#define FAILED(x) ((x)<0)
constexpr HRESULT S_OK=0,E_FAIL=-1;constexpr int VT_I4=3,TASK_ENUM_HIDDEN=1,IID_IExecAction=1;
enum TASK_ACTION_TYPE{TASK_ACTION_EXEC=0,TASK_ACTION_COM_HANDLER=1};
struct VARIANT{int vt=0;LONG lVal=0;};void VariantInit(VARIANT* v){*v={};}
namespace GP {enum class PlatformArch{X64,X86};
 const wchar_t* GlassAutorunTaskName(PlatformArch a){return a==PlatformArch::X64?L"Task64":L"Task86";}}
bool pathSafe=true;
namespace GlassInstallUtil {
 bool IsSafeInstallPath(const std::wstring&){return pathSafe;}
 std::wstring TrimTrailingSlashes(std::wstring s){while(!s.empty() && s.back()==L'\\')s.pop_back();return s;}
 std::wstring RundllPathForTaskArch(GP::PlatformArch a){return a==GP::PlatformArch::X64?L"C:\\Windows\\System32\\rundll32.exe":L"C:\\Windows\\SysWOW64\\rundll32.exe";}
}
bool SamePath(const std::wstring& a,const std::wstring& b){return !a.empty() && _wcsicmp(a.c_str(),b.c_str())==0;}
struct Bstr {BSTR value=nullptr;~Bstr(){std::free(value);}BSTR* out(){return &value;}};
BSTR dup(const std::wstring& s){return wcsdup(s.c_str());}
static int lastArgc=0;
LPWSTR* CommandLineToArgvW(const wchar_t* text,int* count){
 std::vector<std::wstring> words;std::wstring word;bool quoted=false,started=false;
 for(const wchar_t* p=text;*p;++p){if(*p==L'"'){quoted=!quoted;started=true;continue;}
  if((*p==L' ' || *p==L'\t') && !quoted){if(started){words.push_back(word);word.clear();started=false;}continue;}
  word+=*p;started=true;}
 if(started)words.push_back(word);*count=static_cast<int>(words.size());lastArgc=*count;
 auto** result=static_cast<LPWSTR*>(std::malloc(words.size()*sizeof(LPWSTR)));
 for(int i=0;i<*count;++i)result[i]=dup(words[i]);return result;
}
void LocalFree(LPWSTR* args){for(int i=0;i<lastArgc;++i)std::free(args[i]);std::free(args);}
namespace Microsoft::WRL {template<class T>struct ComPtr{T* ptr=nullptr;T** GetAddressOf(){return &ptr;}T* Get(){return ptr;}
 T* operator->(){return ptr;}explicit operator bool()const{return ptr!=nullptr;}};}
using Microsoft::WRL::ComPtr;
struct Scenario {bool connected=true,exists=true,definitionOk=true,instanceOk=true;int actions=1,instances=1;
 TASK_ACTION_TYPE type=TASK_ACTION_EXEC;std::wstring name=L"Task86",exec=L"C:\\Windows\\SysWOW64\\rundll32.exe",
 args=L"\"C:\\Program Files (x86)\\GlassPlatform\\GlassCompat.dll\",Main /start";} s;
struct IExecAction{HRESULT get_Path(BSTR* out){*out=dup(s.exec);return S_OK;}HRESULT get_Arguments(BSTR* out){*out=dup(s.args);return S_OK;}} exec;
struct IAction {HRESULT get_Type(TASK_ACTION_TYPE* out){*out=s.type;return S_OK;}
 HRESULT QueryInterface(int,void** out){*out=&exec;return S_OK;}} act;
struct IActionCollection {HRESULT get_Count(LONG* out){*out=s.actions;return S_OK;}HRESULT get_Item(int,IAction** out){*out=&act;return S_OK;}} actionList;
struct ITaskDefinition {HRESULT get_Actions(IActionCollection** out){*out=&actionList;return S_OK;}} definition;
struct IRunningTaskCollection{HRESULT get_Count(LONG* out){*out=s.instances;return S_OK;}} running;
struct IRegisteredTask {
 HRESULT get_Name(BSTR* out){*out=dup(s.name);return S_OK;}
 HRESULT get_Definition(ITaskDefinition** out){if(!s.definitionOk)return E_FAIL;*out=&definition;return S_OK;}
 HRESULT GetInstances(int,IRunningTaskCollection** out){if(!s.instanceOk)return E_FAIL;*out=&running;return S_OK;}
} task;
struct IRegisteredTaskCollection {
 HRESULT get_Count(LONG* out){*out=s.exists?1:0;return S_OK;}
 HRESULT get_Item(VARIANT,IRegisteredTask** out){*out=&task;return S_OK;}
} taskList;
struct ITaskFolder {HRESULT GetTasks(int,IRegisteredTaskCollection** out){*out=&taskList;return S_OK;}} root;
struct ITaskService{} service;
struct ComGuard {bool ok()const{return s.connected;}};
bool Connect(ComPtr<ITaskService>& out){out.ptr=&service;return s.connected;}
bool GetFolder(ITaskService*,const wchar_t*,ComPtr<ITaskFolder>& out){out.ptr=&root;return s.connected;}
std::wstring Problem(bool,const wchar_t* ru,const wchar_t*,const std::wstring& detail={}){return std::wstring(ru)+L" "+detail;}
'''
main = r'''
int main(){
 const std::wstring x86=L"C:\\Program Files (x86)\\GlassPlatform";std::wstring error;
 auto check=[&](bool existed,bool running){return ValidatePlatformTask(GP::PlatformArch::X86,x86,existed,running,true,error);};
 auto reset=[](){s=Scenario{};pathSafe=true;};
 reset();assert(check(true,false));assert(check(true,true));
 reset();pathSafe=false;assert(!check(true,false));
 reset();s.exists=false;assert(check(false,false));assert(!check(true,false));
 reset();s.instances=0;assert(!check(true,true));
 reset();s.instances=2;assert(!check(true,true));
 reset();s.instanceOk=false;assert(!check(true,true));
 reset();s.definitionOk=false;assert(!check(true,false));
 reset();s.actions=2;assert(!check(true,false));
 reset();s.type=TASK_ACTION_COM_HANDLER;assert(!check(true,false));
 reset();s.exec=L"D:\\VS_PROJECT\\Debug\\rundll32.exe";assert(!check(true,false));
 reset();s.args=L"\"D:\\VS_PROJECT\\Build\\x86\\Debug\\GlassCompat.dll\",Main /start";assert(!check(true,false));
 reset();s.args=L"\"C:\\Program Files (x86)\\GlassPlatform\\GlassCompat.dll\",BadEntry /start";assert(!check(true,false));
 reset();s.args+=L" D:\\VS_PROJECT\\Debug\\extra.dll";assert(!check(true,false));
 reset();s.args=L"\"C:\\Program Files (x86)\\GlassPlatform\\GlassCompat.dll\",Main";assert(!check(true,false));
 reset();s.connected=false;assert(!check(false,false));
 reset();s.exec=L"C:\\fixed\\OpenGlassHost.exe";s.args=L"";
 assert(ExpectedAction(&definition,false,L"C:\\fixed\\OpenGlassHost.exe"));
 s.args=L"D:\\VS_PROJECT\\Debug\\extra.dll";assert(!ExpectedAction(&definition,false,L"C:\\fixed\\OpenGlassHost.exe"));
 s.args=L"";s.exec=L"D:\\VS_PROJECT\\Debug\\OpenGlassHost.exe";
 assert(!ExpectedAction(&definition,false,L"C:\\fixed\\OpenGlassHost.exe"));
 reset();s.name=L"Task64";s.exec=L"C:\\Windows\\System32\\rundll32.exe";
 s.args=L"\"C:\\Program Files\\GlassPlatform\\GlassCompat.dll\",Main /start";
 assert(ValidatePlatformTask(GP::PlatformArch::X64,L"C:\\Program Files\\GlassPlatform",true,true,false,error));
 std::cout<<"PASS: real task validator: full action, args, arch, running instances and changed paths\n";
}
'''
with tempfile.TemporaryDirectory(prefix="glass-fixed-task-") as directory:
    cpp = Path(directory) / "task.cpp"; exe = Path(directory) / "task"
    cpp.write_text(prefix + args + "\n" + action + "\n" + validate + "\n" + main, encoding="utf-8")
    compiler = os.environ.get("CXX", "g++")
    subprocess.run([compiler, "-std=c++17", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
                    str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("PASS: Setup/Unins check task/service paths before changes, start and rollback")
