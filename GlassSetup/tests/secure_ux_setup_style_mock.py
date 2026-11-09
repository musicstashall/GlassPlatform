"""Actual Setup style delegation: trusted OS-native helper, never x86 style application on x64."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallSupport/GlassInstallUtil.cpp').read_text()
a=s.index('bool PrepareSecureUxRemovalVisualStyle(');b=s.index('bool QuerySecureUxPendingFileOperations(',a);method=s[a:b]
assert 'InstalledSecureUxThemeNativeDll' in method and 'SetSystemVisualStyle' not in method
h=r'''
#include <cassert>
#include <cwchar>
#include <string>
#include <iostream>
#include <cstdio>
using DWORD=unsigned;using WORD=unsigned short;
constexpr DWORD MAXDWORD=~0u;constexpr WORD IMAGE_FILE_MACHINE_AMD64=0x8664,IMAGE_FILE_MACHINE_I386=0x14c;
namespace GlassShared::GlassPlatformTask {enum class PlatformArch{X64,X86};}
bool os64=true,missing=false,pe=true,path=true,launch=true,start=true,complete=true;DWORD code=0;int calls=0;
bool IsOs64Bit(){return os64;}
std::wstring InstalledSecureUxThemeNativeDll(GlassShared::GlassPlatformTask::PlatformArch arch,const std::wstring&,const std::wstring&,const char* entry){assert(std::string(entry)=="GlassSettings_SecureUxThemePrepareRemovalV2W");assert((arch==GlassShared::GlassPlatformTask::PlatformArch::X64)==os64);return missing?L"":L"trusted.dll";}
bool PeMatches(const std::wstring&,WORD machine,const char* entry){assert(machine==(os64?IMAGE_FILE_MACHINE_AMD64:IMAGE_FILE_MACHINE_I386));assert(std::string(entry)=="GlassSettings_SecureUxThemePrepareRemovalV2W");return pe;}
std::wstring NativeRundll64(){assert(os64);return path?L"native64.exe":L"";}
std::wstring RundllPathForTaskArch(GlassShared::GlassPlatformTask::PlatformArch arch){assert(!os64&&arch==GlassShared::GlassPlatformTask::PlatformArch::X86);return path?L"native32.exe":L"";}
bool RunHiddenCommand(const std::wstring& cmd,DWORD* out,int timeout,bool* started,bool* completed){++calls;assert(timeout==45000&&cmd.find(L"trusted.dll\",GlassSettings_SecureUxThemePrepareRemovalV2W")!=std::wstring::npos);*out=code;*started=start;*completed=complete;return launch;}
namespace GlassInstallLog{template<class... T>void LineFormat(const wchar_t*,T...) {}}
struct SecureUxThemeDeactivateRun {bool ran=false;};
template<size_t N,class... T>int swprintf_s(wchar_t (&buf)[N],const wchar_t* fmt,T... args){return swprintf(buf,N,fmt,args...);}
// METHOD
int main(){std::wstring detail;SecureUxThemeDeactivateRun run;
for(bool arch:{false,true}){os64=arch;
 for(int failure=0;failure<8;++failure){missing=failure==0;pe=failure!=1;path=failure!=2;launch=failure!=3;start=failure!=4;complete=failure!=5;code=failure==6?12:failure==7?16:0;calls=0;run.ran=true;
 assert(!PrepareSecureUxRemovalVisualStyle(L"old installed x64",L"old installed x86",&run,&detail));assert(!run.ran&&!detail.empty());assert(calls==(failure>=3?1:0));}
 missing=false;pe=path=launch=start=complete=true;code=0;assert(PrepareSecureUxRemovalVisualStyle(L"",L"",&run,&detail)&&run.ran);
}
std::cout<<"PASS: actual Setup style helper: x64/x86 OS routing, trusted Native/export, launch/timeout/exit failures, no in-process style application\n";
}
'''.replace('// METHOD',method)
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';p.write_text(h);exe=Path(d)/'test'
 subprocess.run(['g++','-std=c++17','-fsanitize=address,undefined','-fno-pie','-no-pie',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
