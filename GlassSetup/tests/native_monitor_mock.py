#!/usr/bin/env python3
"""Exercise the real Native export and Setup's in-process loader using Win32 mocks.

This does not load an actual Windows DLL or replace an MSVC/Windows test.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
native = (root / "GlassSettings/Native/InjectProcessMapNative.cpp").read_text(encoding="utf-8")
watch = (root / "GlassSetup/src/InstallSupport/GlassInstallWatch.cpp").read_text(encoding="utf-8")

def function(source, token):
    start = source.index(token)
    brace = source.index("{", start)
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{": depth += 1
        if source[pos] == "}":
            depth -= 1
            if depth == 0: return source[start:pos + 1]
    raise ValueError(token)

def section(source, start, end):
    pos = source.index(start)
    return source[pos:source.index(end, pos)]

export = function(native, "__declspec(dllexport) BOOL WINAPI GlassSettings_ScanGlassModules(")
export_prefix = r'''
#include <cassert>
#include <cstdint>
#include <iostream>
#define __declspec(x)
#define WINAPI
using DWORD=std::uint32_t;using BOOL=int;
constexpr BOOL TRUE=1,FALSE=0;
namespace Shared::InjectModuleScan {
 constexpr DWORD kNativeLocalInstallerScan=0x80000000u,kNativeInstallerScanProtocol=0x31495347u;
 struct Result {DWORD pid=0;};
 int normalCalls=0, strictCalls=0;bool normalResult=true,strictResult=true;
 bool Scan(const DWORD*,DWORD,Result*,DWORD capacity,DWORD* count){++normalCalls;assert(capacity==4);*count=1;return normalResult;}
 bool ScanForSetup(const DWORD*,DWORD,Result*,DWORD capacity,DWORD* count){++strictCalls;assert(capacity==4);*count=1;return strictResult;}
}
int mergeCalls=0;bool mergeResult=true;
bool MergeOppositeX64Scan(Shared::InjectModuleScan::Result*,DWORD,DWORD*,const DWORD*,DWORD){++mergeCalls;return mergeResult;}
void* GetCurrentProcess(){return nullptr;}
BOOL IsWow64Process(void*,BOOL* wow){*wow=TRUE;return TRUE;}
'''
export_main = r'''
int main(){
 using namespace Shared::InjectModuleScan;
 Result rows[4]{};DWORD count=0;
 assert(GlassSettings_ScanGlassModules(nullptr,0,nullptr,kNativeLocalInstallerScan,&count));
 assert(count==kNativeInstallerScanProtocol && normalCalls==0 && strictCalls==0 && mergeCalls==0);
 assert(!GlassSettings_ScanGlassModules(nullptr,0,nullptr,0,&count));
 assert(!GlassSettings_ScanGlassModules(nullptr,0,rows,kNativeLocalInstallerScan,&count));
 assert(!GlassSettings_ScanGlassModules(nullptr,0,rows,4,nullptr));
 assert(GlassSettings_ScanGlassModules(nullptr,0,rows,4|kNativeLocalInstallerScan,&count));
 assert(strictCalls==1 && normalCalls==0 && mergeCalls==0 && count==1);
 strictResult=false;
 assert(!GlassSettings_ScanGlassModules(nullptr,0,rows,4|kNativeLocalInstallerScan,&count));
 assert(mergeCalls==0);
 assert(GlassSettings_ScanGlassModules(nullptr,0,rows,4,&count));
 assert(normalCalls==1);
#ifndef _WIN64
 assert(mergeCalls==1);
#else
 assert(mergeCalls==0);
#endif
 std::cout<<"PASS: existing Native export: capability handshake, strict local mode and legacy scan behavior\n";
}
'''

globals_part = section(watch, "#ifdef GLASS_SETUP_NATIVE_MONITOR\nusing NativeScan", "\nstd::wstring TrimToken")
local_scan = section(watch, "bool ScanLocalNative(", "\nbool ScanInjectMapInto(")
loader = section(watch, "bool BeginLocalNativeMonitor(", "\n#endif\n\nvoid MergeModuleTokens(")
loader_prefix = r'''
#include <cassert>
#include <cstdint>
#include <cwchar>
#include <iostream>
#include <iterator>
#include <string>
#define GLASS_SETUP_NATIVE_MONITOR 1
#define WINAPI
#define _wcsicmp wcscasecmp
using DWORD=std::uint32_t;using BOOL=int;using FARPROC=void(*)();using HMODULE=void*;
constexpr BOOL FALSE=0,TRUE=1;
constexpr DWORD LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR=1,LOAD_LIBRARY_SEARCH_SYSTEM32=2;
namespace Shared::InjectModuleScan {
 constexpr DWORD kNativeLocalInstallerScan=0x80000000u,kNativeInstallerScanProtocol=0x31495347u;
 constexpr char kNativeInstallerProtocolExport[]="GlassSettings_InstallerScanProtocolV1";
 struct Result {DWORD pid=0;};
}
struct Fake {
 bool path=true,load=true,identify=true,wrongLoadedPath=false,scanExport=true,unloadExport=true,marker=true,wrongMarker=false,capable=true,wrongProtocol=false,scanSucceeds=true,overCount=false;
 int frees=0,strictScans=0,handshakes=0,unloadCalls=0;DWORD flags=0;
} fake;
BOOL scanImpl(const DWORD*,DWORD,void* results,DWORD cap,DWORD* count){
 using namespace Shared::InjectModuleScan;
 if(!results){++fake.handshakes;if(!fake.capable)return FALSE;*count=fake.wrongProtocol?0:kNativeInstallerScanProtocol;return TRUE;}
 ++fake.strictScans;assert(cap==(4|kNativeLocalInstallerScan));*count=fake.overCount?5:1;return fake.scanSucceeds;
}
BOOL unloadImpl(DWORD pid){++fake.unloadCalls;return pid==42;}
namespace GlassInstallUtil { std::wstring EmbeddedNativeDllX86(){return fake.path?L"trusted-temp/Native.dll":L"";} }
namespace GlassInstallLog { void Line(const wchar_t*){} template<class... T>void LineFormat(const wchar_t*,T...){} }
DWORD GetLastError(){return 5;}
HMODULE LoadLibraryExW(const wchar_t*,void*,DWORD flags){fake.flags=flags;return fake.load?reinterpret_cast<HMODULE>(1):nullptr;}
DWORD GetModuleFileNameW(HMODULE,wchar_t* out,DWORD capacity){
 if(!fake.identify)return 0;
 const wchar_t* path=fake.wrongLoadedPath?L"other/Native.dll":L"trusted-temp/Native.dll";
 const auto length=std::wcslen(path);assert(length<capacity);std::wcscpy(out,path);return static_cast<DWORD>(length);
}
BOOL FreeLibrary(HMODULE){++fake.frees;return TRUE;}
FARPROC GetProcAddress(HMODULE,const char* name){
 std::string n(name);
 if(n=="GlassSettings_ScanGlassModules@20" && fake.scanExport) return reinterpret_cast<FARPROC>(&scanImpl);
 if(n=="_GlassSettings_ForceUnloadProcess@4" && fake.unloadExport) return reinterpret_cast<FARPROC>(&unloadImpl);
 if(n=="GlassSettings_InstallerScanProtocolV1" && fake.marker)
  return fake.wrongMarker?reinterpret_cast<FARPROC>(&unloadImpl):reinterpret_cast<FARPROC>(&scanImpl);
 return nullptr;
}
'''
loader_main = r'''
int main(){
 using namespace Shared::InjectModuleScan;
 auto reset=[](){fake={};};
 reset();fake.path=false;assert(!GlassInstallWatch::BeginLocalNativeMonitor());assert(fake.frees==0);
 reset();fake.load=false;assert(!GlassInstallWatch::BeginLocalNativeMonitor());assert(fake.frees==0);
 reset();fake.identify=false;assert(!GlassInstallWatch::BeginLocalNativeMonitor());assert(fake.frees==1 && fake.handshakes==0);
 reset();fake.wrongLoadedPath=true;assert(!GlassInstallWatch::BeginLocalNativeMonitor());assert(fake.frees==1 && fake.handshakes==0);
 reset();fake.scanExport=false;assert(!GlassInstallWatch::BeginLocalNativeMonitor());assert(fake.frees==1);
 reset();fake.unloadExport=false;assert(!GlassInstallWatch::BeginLocalNativeMonitor());assert(fake.frees==1);
 reset();fake.marker=false;assert(!GlassInstallWatch::BeginLocalNativeMonitor());assert(fake.frees==1 && fake.handshakes==0);
 reset();fake.wrongMarker=true;assert(!GlassInstallWatch::BeginLocalNativeMonitor());assert(fake.frees==1 && fake.handshakes==0);
 reset();fake.capable=false;assert(!GlassInstallWatch::BeginLocalNativeMonitor());assert(fake.frees==1 && fake.strictScans==0);
 reset();fake.wrongProtocol=true;assert(!GlassInstallWatch::BeginLocalNativeMonitor());assert(fake.frees==1 && fake.strictScans==0);
 reset();assert(GlassInstallWatch::BeginLocalNativeMonitor());assert(fake.handshakes==1);
 assert(fake.flags==(LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32));
 assert(GlassInstallWatch::BeginLocalNativeMonitor());assert(fake.handshakes==1);
 Result rows[4]{};DWORD count=99;
 assert(ScanLocalNative(nullptr,0,rows,4,&count));assert(count==1 && fake.strictScans==1);
 fake.overCount=true;assert(!ScanLocalNative(nullptr,0,rows,4,&count));fake.overCount=false;
 fake.scanSucceeds=false;assert(!ScanLocalNative(nullptr,0,rows,4,&count));
 assert(!GlassInstallWatch::ForceUnloadInLocalNative(0));
 assert(GlassInstallWatch::ForceUnloadInLocalNative(42));assert(fake.unloadCalls==1);
 GlassInstallWatch::EndLocalNativeMonitor();assert(fake.frees==1);
 assert(!ScanLocalNative(nullptr,0,rows,4,&count));assert(!GlassInstallWatch::ForceUnloadInLocalNative(42));
 GlassInstallWatch::EndLocalNativeMonitor();assert(fake.frees==1);
 std::cout<<"PASS: Setup's x86 Native loader: trusted path, export aliases, old DLL rejection, local scan and unload\n";
}
'''

compiler = os.environ.get("CXX", "g++")
with tempfile.TemporaryDirectory(prefix="glass-native-test-") as directory:
    directory = Path(directory)
    common = [compiler, "-std=c++17", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    for native64 in (False, True):
        cpp = directory / ("export64.cpp" if native64 else "export32.cpp")
        exe = cpp.with_suffix("")
        cpp.write_text(export_prefix + export + "\n" + export_main, encoding="utf-8")
        subprocess.run(common + (["-D_WIN64"] if native64 else []) + [str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    cpp = directory / "loader.cpp"
    exe = directory / "loader"
    cpp.write_text(loader_prefix + "\nnamespace {\n" + globals_part + "\n" + local_scan +
                   "\n}\nnamespace GlassInstallWatch {\n" + loader + "\n}\n" + loader_main, encoding="utf-8")
    subprocess.run(common + [str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
