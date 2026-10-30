#!/usr/bin/env python3
"""Exercise exact installer shell paths with fake Win32 and Native APIs.

This verifies routing and command parsing, not file authenticity, a Windows
shell registration, or a real MSVC/WiX build.
"""
from pathlib import Path
import os
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[2]
ops = (repo / 'GlassSettings/Native/ShellExtensionOps.cpp').read_text(encoding='utf-8')
sparse = (repo / 'GlassSettings/Native/ShellSparsePackage.cpp').read_text(encoding='utf-8')
util = (repo / 'GlassSetup/src/InstallSupport/GlassInstallUtil.cpp').read_text(encoding='utf-8')
unins = (repo / 'GlassSetup/GlassUnins/main.cpp').read_text(encoding='utf-8')
engine = (repo / 'GlassSetup/src/InstallEngine.cpp').read_text(encoding='utf-8')
script = (repo / 'GlassSetup/scripts/Verify-InstallerNative.ps1').read_text(encoding='utf-8')

def function(text, token):
    start = text.index(token)
    opening = text.index('{', start)
    depth = 0
    for pos in range(opening, len(text)):
        if text[pos] == '{':
            depth += 1
        elif text[pos] == '}':
            depth -= 1
            if not depth:
                return text[start:pos + 1]
    raise ValueError(token)

read_cmd = function(ops, 'std::wstring ReadRundllCommand(')
parse_cmd = function(ops, 'bool ParseInstallerShellArguments(')
resolve_shell = function(ops, 'bool ResolveBundledGlassShellExtDll(')
resolve_sparse = function(sparse, 'bool ResolveGlassPlatformOutputDirectory(')
invoke = function(util, 'bool InvokeShellExtRegistration(')
operation = function(unins, 'int RunOperation(')
preflight = function(engine, 'bool InstallEngine::VerifyEmbeddedNativePayload(')
assert operation.index('kInstallerShellExport') < operation.index('StopAllForUninstall(')
assert operation.index('kInstallerShellExport') < operation.index('UninstallProduct(')
assert 'kInstallerShellExport' in preflight
assert 'GlassSettings_ShellExtRegisterSetupV1W' in script
assert 'GlassSettings_ShellExtRegisterSetupV1W' in ops
assert '/EXPORT:GlassSettings_ShellExtRegisterSetupV1W=_GlassSettings_ShellExtRegisterSetupV1W@16' in ops
for name in ('ResolveSparseManifestPath', 'ResolveSparseUnregisterStubManifestPath',
             'ResolveSparsePackageDirectory', 'ResolveSparseMsixPath'):
    assert 'exactPlatformRoot' in function(sparse, 'bool ' + name + '(')
for name in ('RegisterSparsePackageQuiet', 'UnregisterSparsePackageQuiet'):
    assert 'exactPlatformRoot' in function(sparse, 'bool ' + name + '(')

prefix = r'''
#include <cassert>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <cstring>
#include <iostream>
#include <set>
#include <string>
#include <vector>
#include <algorithm>
using DWORD=unsigned long; using WORD=unsigned short; using UINT=unsigned;
using LSTATUS=long; using REGSAM=unsigned long; using LPBYTE=unsigned char*;
using LPWSTR=wchar_t*; using HKEY=void*; using HMODULE=void*; using BYTE=unsigned char;
constexpr int CP_ACP=0, ERROR_SUCCESS=0; constexpr DWORD KEY_READ=1,KEY_WOW64_64KEY=2,KEY_WOW64_32KEY=4,REG_SZ=1;
constexpr WORD IMAGE_FILE_MACHINE_AMD64=0x8664,IMAGE_FILE_MACHINE_I386=0x14c;
HKEY const HKEY_LOCAL_MACHINE=reinterpret_cast<HKEY>(1), HKEY_CURRENT_USER=reinterpret_cast<HKEY>(2);
constexpr int MAX_PATH=260;
#define _countof(a) (sizeof(a)/sizeof((a)[0]))
int __ImageBase=0;
struct Fake {
 std::set<std::wstring> files,sparseRoots,unsafe;
 std::wstring nextExe,nextDll,registry,exeDir=L"C:\\Windows\\System32",command;
 int nextCalls=0, registryCalls=0, commandCalls=0, argvLive=0;
 bool os64=true,nativePresent=true,nativeProtocol=true,started=true,completed=true,ran=true,existingShell=false,matchingShell=false;
 DWORD exitCode=0;
} fake;
std::vector<std::wstring> tokenStorage;
int _wcsicmp(const wchar_t* a,const wchar_t* b){
 while(*a&&*b){auto x=std::towlower(*a++),y=std::towlower(*b++);if(x!=y)return x<y?-1:1;}return *a?1:*b?-1:0;
}
int wcscpy_s(wchar_t* out,size_t cch,const wchar_t* value){
 if(!out||!value||std::wcslen(value)>=cch)return 1;std::wcscpy(out,value);return 0;
}
template<size_t N> int wcscpy_s(wchar_t (&out)[N],const wchar_t* value){return wcscpy_s(out,N,value);}
int MultiByteToWideChar(unsigned,DWORD,const char* input,int,wchar_t* out,int cap){
 const int n=static_cast<int>(std::strlen(input))+1;if(!out)return n;if(cap<n)return 0;
 for(int i=0;i<n;i++)out[i]=static_cast<unsigned char>(input[i]);return n;
}
LPWSTR* CommandLineToArgvW(const wchar_t* input,int* count){
 tokenStorage.clear();std::wstring current;bool quotes=false,started=false;
 for(const wchar_t* p=input;;++p){
  if(*p==L'"'){quotes=!quotes;started=true;continue;}
  if(!*p || (!quotes && (*p==L' ' || *p==L'\t'))){
   if(started){tokenStorage.push_back(current);current.clear();started=false;}
   if(!*p)break;
  }else{current.push_back(*p);started=true;}
 }
 *count=static_cast<int>(tokenStorage.size());auto* result=new LPWSTR[*count];
 for(int i=0;i<*count;i++)result[i]=tokenStorage[i].data();++fake.argvLive;return result;
}
void* LocalFree(void* p){delete[] static_cast<LPWSTR*>(p);--fake.argvLive;return nullptr;}
bool PathIsRelativeW(const wchar_t* p){return !((p[0] && p[1]==L':' && p[2]==L'\\') || (p[0]==L'\\'&&p[1]==L'\\'));}
bool PathFileExistsW(const wchar_t* p){return fake.files.count(p)!=0;}
LSTATUS RegOpenKeyExW(HKEY,const wchar_t*,DWORD,REGSAM,HKEY*){++fake.registryCalls;return 2;}
LSTATUS RegQueryValueExW(HKEY,const wchar_t*,void*,DWORD*,LPBYTE,DWORD*){return 2;}
LSTATUS RegCloseKey(HKEY){return 0;}
namespace GlassShared::GlassModulePath {
 enum class ModuleArch{X86,X64}; ModuleArch NativeModuleArch(){return ModuleArch::X64;}
 std::wstring ResolveGlassModulePath(const wchar_t* root,const wchar_t* name,ModuleArch){
  if(!root||!*root)return {};std::wstring p=std::wstring(root)+L"\\"+name;
  return fake.files.count(p)?p:std::wstring{};
 }
 std::wstring ResolveGlassModulePathNextToModule(HMODULE h,const wchar_t*){++fake.nextCalls;return h?fake.nextDll:fake.nextExe;}
 std::wstring GetInstallRootFromPath(const wchar_t* p){std::wstring x=p;auto i=x.find_last_of(L'\\');return i==x.npos?L"":x.substr(0,i);}
}
bool TryCanonicalizePlatformDir(const wchar_t* p,wchar_t* out,size_t cch){
 return p&&fake.sparseRoots.count(p)&&wcscpy_s(out,cch,p)==0;
}
std::wstring TryReadRegistryInstallDir(){++fake.registryCalls;return fake.registry;}
bool ResolveExeOutputDirectory(wchar_t* out,size_t cch){return wcscpy_s(out,cch,fake.exeDir.c_str())==0;}
namespace GlassShared::GlassPlatformTask {enum class PlatformArch{X64,X86};}
using namespace GlassShared::GlassPlatformTask;
namespace GlassInstallLog {void Line(const wchar_t*){} template<class... T>void LineFormat(const wchar_t*,T...) {}}
bool HasExistingShellRegistrationKeys(){return fake.existingShell;}
bool HasMatchingLegacyShellRegistration(const std::wstring&){return fake.matchingShell;}
namespace GlassInstallUtil {
 bool InvokeShellExtRegistration(bool,const std::wstring&,const std::wstring&,bool*,bool* = nullptr) noexcept;
 constexpr char kInstallerShellExport[]="GlassSettings_ShellExtRegisterSetupV1W";
 bool IsOs64Bit(){return fake.os64;}
 std::wstring TrimTrailingSlashes(std::wstring s){while(!s.empty()&&(s.back()==L'\\'||s.back()==L'/'))s.pop_back();return s;}
 bool IsSafeInstallPath(const std::wstring& p){return !fake.unsafe.count(p);}
 std::wstring NativeDllForArch(PlatformArch,const std::wstring&,const std::wstring&){return fake.nativePresent?L"protected-temp\\Native.dll":L"";}
 bool PeMatches(const std::wstring&,WORD machine,const char* name){
  assert(std::string(name)==kInstallerShellExport);
  assert(machine==(fake.os64?IMAGE_FILE_MACHINE_AMD64:IMAGE_FILE_MACHINE_I386));return fake.nativeProtocol;
 }
 std::wstring System64ExePath(const wchar_t*){return L"C:\\Windows\\System32\\regsvr32.exe";}
 std::wstring NativeRundll64(){return L"C:\\Windows\\System32\\rundll32.exe";}
 std::wstring RundllPathForTaskArch(PlatformArch){return L"C:\\Windows\\System32\\rundll32.exe";}
 bool RunHiddenCommand(const std::wstring& cmd,DWORD* exit,DWORD,bool* started=nullptr,bool* completed=nullptr){
  fake.command=cmd;++fake.commandCalls;if(exit)*exit=fake.exitCode;
  if(started)*started=fake.started;if(completed)*completed=fake.completed;return fake.ran;
 }
}
'''
main = r'''
int main(){
 auto reset=[](){fake=Fake{};};bool unregister=false;std::wstring root;
 // Quoted/custom/Unicode names survive parsing; absent, relative or extra arguments fail.
 reset();wchar_t one[]=L"/register /installroot \"D:\\My Apps\\Стекло\"";
 assert(ParseInstallerShellArguments(one,unregister,root) && !unregister && root==L"D:\\My Apps\\Стекло");
 wchar_t two[]=L"/unregister /installroot \"\\\\server\\share\\Glass\"";
 assert(ParseInstallerShellArguments(two,unregister,root) && unregister && root==L"\\\\server\\share\\Glass");
 for(auto s:{L"/register",L"/register /installroot \"relative\\Glass\"",L"/register /installroot \"D:\\Glass\" /extra",L"/oops /installroot \"D:\\Glass\""}){
  std::wstring x=s;assert(!ParseInstallerShellArguments(x.data(),unregister,root));assert(root.empty());
 }
 assert(fake.argvLive==0);
 // The ANSI compatibility path allocates space for the terminating character.
 const std::string ansi="/register /installroot \"D:\\Glass\"";
 std::vector<wchar_t> bytes((ansi.size()+sizeof(wchar_t))/sizeof(wchar_t)+1,L'\0');
 std::memcpy(bytes.data(),ansi.c_str(),ansi.size()+1);
 assert(ParseInstallerShellArguments(bytes.data(),unregister,root) && root==L"D:\\Glass" && fake.argvLive==0);
 // No fallback to rundll32, temp or a second registry value when an exact root is supplied.
 reset();fake.nextExe=L"C:\\Windows\\System32\\GlassShellExt.dll";fake.nextDll=L"C:\\Windows\\Temp\\GlassShellExt.dll";
 fake.files.insert(L"D:\\Apps\\GlassShellExt.dll");wchar_t output[512]{};
 assert(ResolveBundledGlassShellExtDll(output,512,L"D:\\Apps") && std::wstring(output)==L"D:\\Apps\\GlassShellExt.dll");
 assert(!ResolveBundledGlassShellExtDll(output,512,L"E:\\Missing") && fake.nextCalls==0 && fake.registryCalls==0);
 assert(ResolveBundledGlassShellExtDll(output,512,nullptr) && std::wstring(output)==fake.nextExe && fake.nextCalls==1);
 reset();fake.nextExe=L"C:\\Elsewhere\\GlassShellExt.dll";fake.sparseRoots.insert(L"D:\\Apps");
 assert(ResolveGlassPlatformOutputDirectory(output,512,L"D:\\Apps") && std::wstring(output)==L"D:\\Apps");
 assert(!ResolveGlassPlatformOutputDirectory(output,512,L"E:\\Missing") && fake.nextCalls==0 && fake.registryCalls==0);
 // Invoke always passes the selected architecture's root, with a versioned export.
 reset();bool changed=false;assert(GlassInstallUtil::InvokeShellExtRegistration(true,L"D:\\64 Apps",L"E:\\32",&changed));
 assert(changed && fake.commandCalls==1 && fake.command.find(L"GlassSettings_ShellExtRegisterSetupV1W /register /installroot \"D:\\64 Apps\"")!=std::wstring::npos);
 reset();assert(GlassInstallUtil::InvokeShellExtRegistration(false,L"D:\\64",L"E:\\32",nullptr));
 assert(fake.command.find(L"/unregister /installroot \"D:\\64\"")!=std::wstring::npos);
 reset();fake.nativeProtocol=false;assert(!GlassInstallUtil::InvokeShellExtRegistration(false,L"D:\\64",L"E:\\32",nullptr) && fake.commandCalls==0);
 reset();assert(!GlassInstallUtil::InvokeShellExtRegistration(false,L"D:\\Bad\"Name",L"E:\\32",nullptr) && fake.commandCalls==0);
 // On x86 an old/missing Native helper falls back only to regsvr32 at the selected root.
 reset();fake.os64=false;fake.nativeProtocol=false;fake.files.insert(L"E:\\32\\GlassShellExt.dll");
 assert(GlassInstallUtil::InvokeShellExtRegistration(false,L"",L"E:\\32",&changed));
 assert(fake.command.find(L"/u /s \"E:\\32\\GlassShellExt.dll\"")!=std::wstring::npos);
 reset();fake.os64=false;fake.nativePresent=false;assert(!GlassInstallUtil::InvokeShellExtRegistration(false,L"",L"E:\\32",nullptr));
 reset();fake.exitCode=0x47505245;assert(GlassInstallUtil::InvokeShellExtRegistration(true,L"D:\\64",L"E:\\32",&changed) && !changed);
 bool conflict=false;
 reset();fake.exitCode=0x47504346;assert(!GlassInstallUtil::InvokeShellExtRegistration(true,L"D:\\64",L"E:\\32",&changed,&conflict));
 assert(conflict && !changed); // A preserved conflict must not be unregistered during rollback.
 reset();fake.exitCode=0x47504346;assert(!GlassInstallUtil::InvokeShellExtRegistration(false,L"D:\\64",L"E:\\32",&changed,&conflict));
 assert(conflict && !changed);
 reset();fake.os64=false;fake.nativePresent=false;fake.existingShell=true;fake.matchingShell=false;
 fake.files.insert(L"E:\\32\\GlassShellExt.dll");
 assert(!GlassInstallUtil::InvokeShellExtRegistration(true,L"",L"E:\\32",&changed,&conflict) && conflict && !changed && fake.commandCalls==0);
 reset();fake.os64=false;fake.nativePresent=false;fake.existingShell=true;fake.matchingShell=true;
 fake.files.insert(L"E:\\32\\GlassShellExt.dll");
 assert(GlassInstallUtil::InvokeShellExtRegistration(true,L"",L"E:\\32",&changed,&conflict) && !conflict && !changed && fake.commandCalls==0);
 reset();fake.os64=false;fake.nativePresent=false;fake.existingShell=true;fake.matchingShell=false;
 fake.files.insert(L"E:\\32\\GlassShellExt.dll");
 assert(!GlassInstallUtil::InvokeShellExtRegistration(false,L"",L"E:\\32",&changed,&conflict) && conflict && !changed && fake.commandCalls==0);
 reset();fake.os64=false;fake.nativePresent=false;fake.existingShell=true;fake.matchingShell=true;
 fake.files.insert(L"E:\\32\\GlassShellExt.dll");
 assert(GlassInstallUtil::InvokeShellExtRegistration(false,L"",L"E:\\32",&changed,&conflict) && changed && fake.commandCalls==1);
 std::cout<<"PASS: exact selected root, preserved conflicts, verified legacy registration and compatible x86 fallback\n";
}
'''
with tempfile.TemporaryDirectory(prefix='glass-shell-install-root-') as directory:
    cpp = Path(directory) / 'test.cpp'
    exe = Path(directory) / 'test'
    code = prefix + '\n' + read_cmd + '\n' + parse_cmd + '\n' + resolve_shell + '\n' + resolve_sparse
    code += '\nnamespace GlassInstallUtil {\n' + invoke + '\n}\n' + main
    cpp.write_text(code, encoding='utf-8')
    subprocess.run([os.environ.get('CXX', 'g++'), '-std=c++17', '-g', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-no-pie', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
