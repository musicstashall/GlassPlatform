#!/usr/bin/env python3
"""Test the real watch loop against a timed pipe/process model, not a Windows VM."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "GlassSetup/src/InstallSupport/GlassInstallWatch.cpp").read_text(encoding="utf-8")

def extract(begin, end):
    start = source.index(begin)
    return source[start:source.index(end, start)]

merge_helpers = extract("std::wstring TrimToken(", "std::wstring ModuleNamesFromBits(")
consume = extract("bool ConsumeNextPipeEvent(", "} // namespace")
parser = extract("void MergeModuleTokens(", "bool CollectInProcessHolders(")
watch = extract("bool WaitUntilClearOrTimeout(", "} // namespace GlassInstallWatch")
prefix = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <codecvt>
#include <cstring>
#include <cwchar>
#include <cstdint>
#include <iostream>
#include <locale>
#include <string>
#include <vector>
using DWORD=std::uint32_t;using ULONGLONG=std::uint64_t;using BOOL=int;
#define _wcsicmp wcscasecmp
constexpr BOOL TRUE=1,FALSE=0;constexpr int CP_UTF8=65001,MB_ERR_INVALID_CHARS=8;
constexpr DWORD WAIT_OBJECT_0=0,WAIT_TIMEOUT=258,STILL_ACTIVE=259;
constexpr DWORD PIPE_ACCESS_INBOUND=1,FILE_FLAG_FIRST_PIPE_INSTANCE=2,PIPE_TYPE_BYTE=4,
 PIPE_READMODE_BYTE=8,PIPE_NOWAIT=16,PIPE_REJECT_REMOTE_CLIENTS=32,
 ERROR_PIPE_CONNECTED=535,ERROR_PIPE_LISTENING=536,STARTF_USESHOWWINDOW=1,SW_HIDE=0,
 CREATE_NO_WINDOW=1,SDDL_REVISION_1=1;
struct Handle{int kind;};using HANDLE=Handle*;
#define INVALID_HANDLE_VALUE reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(-1))
using PSECURITY_DESCRIPTOR=void*;
struct SECURITY_ATTRIBUTES{std::size_t nLength;void* lpSecurityDescriptor;BOOL bInheritHandle;};
struct STARTUPINFOW{DWORD cb=0,dwFlags=0;int wShowWindow=0;};
struct PROCESS_INFORMATION{HANDLE hProcess=nullptr,hThread=nullptr;};
struct Packet{ULONGLONG at;std::string bytes;};
struct Scenario{
 ULONGLONG now=0,localUntil=0,exitAt=~ULONGLONG(0);bool helperMissing=false,localFails=false,pipeFails=false;
 bool terminated=false;std::vector<Packet> packets;std::size_t nextPacket=0;std::string available;
} scenario;
Handle pipeHandle{1},processHandle{2},threadHandle{3};
DWORD GetCurrentProcessId(){return 1234;}
ULONGLONG GetTickCount64(){return scenario.now;}
void Sleep(DWORD ms){scenario.now+=ms;}
DWORD GetLastError(){return 0;}
template<std::size_t N,typename... T> int swprintf_s(wchar_t (&out)[N],const wchar_t*,T...){
 const wchar_t* value=L"mock-pipe";std::wcsncpy(out,value,N-1);out[N-1]=0;return 1;
}
int MultiByteToWideChar(int,int,const char* text,int size,wchar_t* out,int outSize){
 try{std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;const auto wide=converter.from_bytes(text,text+size);
  if(!out)return static_cast<int>(wide.size());if(outSize<static_cast<int>(wide.size()))return 0;
  std::copy(wide.begin(),wide.end(),out);return static_cast<int>(wide.size());}catch(...){return 0;}
}
BOOL ConvertStringSecurityDescriptorToSecurityDescriptorW(const wchar_t*,DWORD,void** out,void*){*out=nullptr;return TRUE;}
void* LocalFree(void*){return nullptr;}
HANDLE CreateNamedPipeW(const wchar_t*,DWORD,DWORD,DWORD,DWORD,DWORD,DWORD,void*){return &pipeHandle;}
BOOL ConnectNamedPipe(HANDLE,void*){return TRUE;}
BOOL DisconnectNamedPipe(HANDLE){return TRUE;}
BOOL CloseHandle(HANDLE){return TRUE;}
BOOL CreateProcessW(const wchar_t*,wchar_t*,void*,void*,BOOL,DWORD,void*,const wchar_t*,STARTUPINFOW*,PROCESS_INFORMATION* out){
 out->hProcess=&processHandle;out->hThread=&threadHandle;return TRUE;
}
DWORD WaitForSingleObject(HANDLE h,DWORD timeout){
 if(h==&processHandle && (scenario.terminated || scenario.now>=scenario.exitAt))return WAIT_OBJECT_0;
 (void)timeout;return WAIT_TIMEOUT;
}
BOOL GetExitCodeProcess(HANDLE,DWORD* out){*out=0;return TRUE;}
BOOL TerminateProcess(HANDLE,DWORD){scenario.terminated=true;return TRUE;}
BOOL PeekNamedPipe(HANDLE,void*,DWORD,void*,DWORD* available,void*){
 if(scenario.pipeFails)return FALSE;
 while(scenario.nextPacket<scenario.packets.size() && scenario.packets[scenario.nextPacket].at<=scenario.now)
  scenario.available+=scenario.packets[scenario.nextPacket++].bytes;
 *available=static_cast<DWORD>(scenario.available.size());return TRUE;
}
BOOL ReadFile(HANDLE,void* out,DWORD n,DWORD* read,void*){
 *read=std::min<DWORD>(n,static_cast<DWORD>(scenario.available.size()));
 std::memcpy(out,scenario.available.data(),*read);scenario.available.erase(0,*read);return TRUE;
}
namespace GP {enum class PlatformArch{X64};}
namespace GlassInstallUtil {
 std::wstring NativeDllForArch(GP::PlatformArch,const std::wstring&,const std::wstring&){return scenario.helperMissing?L"":L"helper";}
 std::wstring NativeRundll64(){return L"rundll";}
}
namespace GlassInstallLog {void Line(const wchar_t*){}template<class...A>void LineFormat(const wchar_t*,A...){} }
bool NeedNativeX64Helper(){return true;}
namespace GlassInstallWatch {
 struct ModuleHolder{DWORD pid;std::wstring exe,modules;};
 void MergeModuleTokens(std::wstring&,const std::wstring&);
 bool ParseNativePipeUtf8(const std::string&,std::vector<ModuleHolder>&);
 bool CollectInProcessHolders(std::vector<ModuleHolder>& holders){
  if(scenario.localFails)return false;
  if(scenario.now<scenario.localUntil)holders.push_back({77,L"local.exe",L"GlassCore.dll"});
  return true;
 }
}
'''
main = r'''
using namespace GlassInstallWatch;
bool run(DWORD timeout,std::vector<ModuleHolder>& rows,bool& failed,const std::atomic_bool* cancel=nullptr){
 return WaitUntilClearOrTimeout(timeout,L"x64",L"x86",true,rows,failed,cancel);
}
int main(){
 std::vector<ModuleHolder> rows;bool failed=false;
 // The helper must remain alive after empty frames while local x86 holders exist.
 scenario={};scenario.localUntil=900;scenario.packets={{0,"#empty\n"},{600,"#empty\n"},{900,"#empty\n"}};
 assert(run(3000,rows,failed));assert(!failed && scenario.now>=900);
 // A new incomplete frame supersedes an old empty result and blocks progress.
 scenario={};scenario.localUntil=300;scenario.packets={{0,"#empty\n"},{300,"42\tapp.exe\tGlassCore"},
  {600,".dll\n#end\n"},{900,"#empty\n"}};
 assert(run(3000,rows,failed));assert(!failed && scenario.now>=900);
 // At timeout an unfinished row cannot be mistaken for a successful scan.
 scenario={};scenario.localUntil=300;scenario.packets={{0,"#empty\n"},{300,"42\tapp.exe\tGlassCore"}};
 assert(!run(1200,rows,failed));assert(failed);
 // A complete outstanding holder is returned for the unlock pages.
 scenario={};scenario.packets={{0,"42\tapp.exe\tGlassCore.dll\n#end\n"},{600,"42\tapp.exe\tGlassCore.dll\n#end\n"}};
 assert(!run(1200,rows,failed));assert(!failed && rows.size()==1 && rows[0].pid==42);
 // An exited or silent helper, malformed data, failed local scan, or missing DLL fails closed.
 scenario={};scenario.localUntil=1000;scenario.exitAt=300;scenario.packets={{0,"#empty\n"}};
 assert(!run(1200,rows,failed));assert(failed);
 scenario={};assert(!run(1200,rows,failed));assert(failed);
 scenario={};scenario.packets={{0,"#end\n"}};assert(!run(1200,rows,failed));assert(failed);
 scenario={};scenario.localFails=true;scenario.packets={{0,"#empty\n"}};assert(!run(1200,rows,failed));assert(failed);
 scenario={};scenario.helperMissing=true;assert(!run(1200,rows,failed));assert(failed);
 scenario={};scenario.pipeFails=true;assert(!run(1200,rows,failed));assert(failed);
 scenario={};std::atomic_bool cancelled{true};assert(!run(1200,rows,failed,&cancelled));assert(!failed);
 std::cout<<"PASS: real watch loop: repeated empty frames, partial data, timeout, helper exit, cancellation\n";
}
'''
with tempfile.TemporaryDirectory(prefix="glass-watch-test-") as directory:
    cpp = Path(directory) / "watch.cpp"
    exe = Path(directory) / "watch-test"
    cpp.write_text(prefix + merge_helpers + consume + "namespace GlassInstallWatch {\n" + parser + watch + "\n}\n" + main,
                   encoding="utf-8")
    compiler = os.environ.get("CXX", "g++")
    subprocess.run([compiler, "-std=c++17", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
