#!/usr/bin/env python3
"""Run the real module-scan implementation against a deterministic Win32 model.

Covers both native and WOW64 branches, but does not replace a Windows VM test.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "GlassShared/src/InjectModuleScan.cpp").read_text(encoding="utf-8")
start = source.index("namespace Shared::InjectModuleScan\n{")
end = source.index("    namespace\n    {\n        constexpr DWORD kStickyMagic", start)
scanner = source[start:end] + "\n}\n"
prefix = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cwchar>
#include <iostream>
#include <string>
#include <vector>
using DWORD=std::uint32_t; using ULONG=std::uint32_t; using BOOL=int;
constexpr BOOL TRUE=1,FALSE=0; constexpr DWORD ERROR_NO_MORE_FILES=18,ERROR_INVALID_PARAMETER=87,
 ERROR_ACCESS_DENIED=5,ERROR_BAD_LENGTH=24,WAIT_TIMEOUT=258,WAIT_OBJECT_0=0;
constexpr DWORD TH32CS_SNAPPROCESS=1,TH32CS_SNAPMODULE=2,TH32CS_SNAPMODULE32=4,
 PROCESS_QUERY_LIMITED_INFORMATION=0x1000,SYNCHRONIZE=0x100000;
#define _wcsicmp wcscasecmp
#define _TRUNCATE static_cast<std::size_t>(-1)
int wcsncpy_s(wchar_t* dest,std::size_t size,const wchar_t* src,std::size_t){
 if(!size)return 1;std::wcsncpy(dest,src,size-1);dest[size-1]=L'\0';return 0;
}
struct PROCESSENTRY32W { DWORD dwSize=0,th32ProcessID=0; wchar_t szExeFile[260]{}; };
struct MODULEENTRY32W { DWORD dwSize=0; wchar_t szModule[260]{}; };
struct Process {
 DWORD pid;std::wstring name;std::vector<std::wstring> modules;
#ifdef _WIN64
 bool wow=false;
#else
 bool wow=true;
#endif
 bool alive=true, accessible=true, snapshotFails=false, nextFails=false;
 int badLength=0;
};
struct FakeHandle { int kind; std::size_t index=0; Process* process=nullptr; };
using HANDLE=FakeHandle*;
#define INVALID_HANDLE_VALUE reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(-1))
static std::vector<Process> processes;
static DWORD lastError=0;static bool processSnapshotFails=false,processNextFails=false,selfWow=true;
DWORD GetLastError(){return lastError;}
DWORD GetCurrentProcessId(){return 1;}
HANDLE GetCurrentProcess(){return nullptr;}
void Sleep(DWORD){}
Process* byPid(DWORD pid){for(auto& p:processes)if(p.pid==pid)return &p;return nullptr;}
HANDLE CreateToolhelp32Snapshot(DWORD flags,DWORD pid){
 if(flags&TH32CS_SNAPPROCESS){if(processSnapshotFails){lastError=ERROR_ACCESS_DENIED;return INVALID_HANDLE_VALUE;}return new FakeHandle{1};}
 Process* p=byPid(pid);
 if(!p || !p->alive){lastError=ERROR_INVALID_PARAMETER;return INVALID_HANDLE_VALUE;}
 if(p->badLength>0){--p->badLength;lastError=ERROR_BAD_LENGTH;return INVALID_HANDLE_VALUE;}
 if(p->snapshotFails){lastError=ERROR_ACCESS_DENIED;return INVALID_HANDLE_VALUE;}
 return new FakeHandle{2,0,p};
}
BOOL Process32FirstW(HANDLE h,PROCESSENTRY32W* out){
 h->index=0;if(processes.empty()){lastError=ERROR_NO_MORE_FILES;return FALSE;}
 out->th32ProcessID=processes[0].pid;wcsncpy_s(out->szExeFile,260,processes[0].name.c_str(),_TRUNCATE);return TRUE;
}
BOOL Process32NextW(HANDLE h,PROCESSENTRY32W* out){
 if(processNextFails){lastError=ERROR_ACCESS_DENIED;return FALSE;}
 if(++h->index>=processes.size()){lastError=ERROR_NO_MORE_FILES;return FALSE;}
 const auto& p=processes[h->index];out->th32ProcessID=p.pid;wcsncpy_s(out->szExeFile,260,p.name.c_str(),_TRUNCATE);return TRUE;
}
BOOL Module32FirstW(HANDLE h,MODULEENTRY32W* out){
 h->index=0;if(h->process->modules.empty()){lastError=ERROR_NO_MORE_FILES;return FALSE;}
 wcsncpy_s(out->szModule,260,h->process->modules[0].c_str(),_TRUNCATE);return TRUE;
}
BOOL Module32NextW(HANDLE h,MODULEENTRY32W* out){
 if(h->process->nextFails){lastError=ERROR_ACCESS_DENIED;return FALSE;}
 if(++h->index>=h->process->modules.size()){lastError=ERROR_NO_MORE_FILES;return FALSE;}
 wcsncpy_s(out->szModule,260,h->process->modules[h->index].c_str(),_TRUNCATE);return TRUE;
}
HANDLE OpenProcess(DWORD,BOOL,DWORD pid){
 auto* p=byPid(pid);if(!p || !p->alive){lastError=ERROR_INVALID_PARAMETER;return nullptr;}
 if(!p->accessible){lastError=ERROR_ACCESS_DENIED;return nullptr;}return new FakeHandle{3,0,p};
}
BOOL IsWow64Process(HANDLE h,BOOL* out){*out=h?h->process->wow:selfWow;return TRUE;}
DWORD WaitForSingleObject(HANDLE h,DWORD){return h->process->alive?WAIT_TIMEOUT:WAIT_OBJECT_0;}
BOOL CloseHandle(HANDLE h){delete h;return TRUE;}
namespace Shared::SystemHelper { ULONG g_buildNumber=19045; }
namespace Shared::InjectModuleScan {
 enum ModuleBits : std::uint32_t {ModuleCompat=1u,ModuleCore=2u,ModuleWnd=4u,ModuleExplorer=8u,ModuleTaskbar=16u,ModuleUICompat=32u};
 constexpr DWORD kMaxResults=512,kExeNameChars=64;
 struct Result {DWORD pid;std::uint32_t moduleBits;wchar_t exeName[kExeNameChars];};
}
'''
main = r'''
using namespace Shared::InjectModuleScan;
void reset(){processes.clear();lastError=0;processSnapshotFails=false;processNextFails=false;selfWow=true;}
bool strict(Result* rows,DWORD cap,DWORD& count){return ScanForSetup(nullptr,0,rows,cap,&count);}
int main(){
 Result rows[4]{};DWORD count=0;
 reset();processes.push_back({2,L"explorer.exe",{L"ExplorerCompat.dll",L"TaskbarCompat.dll"}});
 assert(strict(rows,4,count));assert(count==1);assert((rows[0].moduleBits&ModuleExplorer)!=0);
 assert((rows[0].moduleBits&ModuleTaskbar)!=0);
 // The regular Settings scan retains its Windows 10 behavior.
 assert(Scan(nullptr,0,rows,4,&count));assert(count==0);
 reset();processes.push_back({2,L"explorer.exe",{L"GlassCompat.dll"}});processes[0].snapshotFails=true;
 assert(!strict(rows,4,count));assert(Scan(nullptr,0,rows,4,&count));
 reset();processes.push_back({2,L"explorer.exe",{L"GlassCompat.dll"}});processes[0].nextFails=true;
 assert(!strict(rows,4,count)); // a partially read list is not a complete scan
 reset();processes.push_back({2,L"explorer.exe",{L"GlassCore.dll"}});processes[0].badLength=3;
 assert(strict(rows,4,count));assert(count==1); // transient ERROR_BAD_LENGTH retried
 reset();processes.push_back({2,L"explorer.exe",{L"GlassCore.dll"}});processes[0].alive=false;
 assert(strict(rows,4,count));assert(count==0); // exited between process and module snapshots
 reset();processes.push_back({2,L"protected.exe",{L"GlassCore.dll"}});processes[0].accessible=false;processes[0].snapshotFails=true;
 assert(strict(rows,4,count));assert(count==0); // unrelated protected OS processes are not fatal
 { const DWORD tracked=2; assert(!ScanForSetup(&tracked,1,rows,4,&count)); } // a map-tracked PID is different
 reset();processes.push_back({2,L"a.exe",{L"GlassCore.dll"}});processes.push_back({3,L"b.exe",{L"WndCompat.dll"}});
 assert(!strict(rows,1,count));assert(Scan(nullptr,0,rows,1,&count));assert(count==1);
 reset();processes.push_back({2,L"a.exe",{L"GlassCore.dll"}});processNextFails=true;
 assert(!strict(rows,4,count));
 reset();processSnapshotFails=true;assert(!strict(rows,4,count));
 reset();processes.push_back({2,L"GlassSettings.exe",{L"GlassCore.dll"}});
 assert(strict(rows,4,count));assert(count==0); // Setup lists the application independently
#ifndef _WIN64
 reset();processes.push_back({2,L"explorer.exe",{L"ExplorerCompat.dll"}});processes[0].wow=false;
 assert(strict(rows,4,count));assert(count==0); // native helper must scan this explorer
 processes[0].accessible=false;assert(!strict(rows,4,count));
#else
 reset();processes.push_back({2,L"wow-app.exe",{L"GlassCore.dll"}});processes[0].wow=true;
 assert(strict(rows,4,count));assert(count==0); // x86 Native monitors WOW64
 assert(Scan(nullptr,0,rows,4,&count));assert(count==1); // existing Settings behavior unchanged
#endif
 std::cout<<"PASS: real scan: Explorer safety, architecture split, failures, retries, limits\n";
}
'''

with tempfile.TemporaryDirectory(prefix="glass-scan-test-") as directory:
    cpp = Path(directory) / "scan.cpp"
    cpp.write_text(prefix + scanner + main, encoding="utf-8")
    compiler = os.environ.get("CXX", "g++")
    for native in (False, True):
        exe = Path(directory) / ("native" if native else "wow64")
        cmd = [compiler, "-std=c++17", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        if native:
            cmd.append("-D_WIN64")
        subprocess.run(cmd + [str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
