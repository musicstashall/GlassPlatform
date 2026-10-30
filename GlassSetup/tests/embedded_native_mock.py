#!/usr/bin/env python3
"""Exercise embedded Native extraction and cleanup against a fake secure filesystem."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "GlassSetup/src/InstallSupport/GlassInstallUtil.cpp").read_text(encoding="utf-8")

def section(begin, end):
    pos = source.index(begin)
    return source[pos:source.index(end, pos)]

def function(token):
    start = source.index(token)
    brace = source.index("{", start)
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{": depth += 1
        if source[pos] == "}":
            depth -= 1
            if depth == 0: return source[start:pos + 1]
    raise ValueError(token)

globals_part = section("std::mutex g_unlockerMutex;", "// Fail closed:")
x64 = function("std::wstring EnsureExtractedUnlockerX64(")
configure = function("void ConfigureEmbeddedUnlocker(")
x86 = function("std::wstring EmbeddedNativeDllX86(")
cleanup = function("void CleanupEmbeddedUnlocker(")
prefix = r'''
#include <cassert>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>
using DWORD=std::uint32_t;using UINT=unsigned;using WORD=unsigned short;using BOOL=int;using HINSTANCE=void*;
constexpr BOOL FALSE=0;constexpr WORD IMAGE_FILE_MACHINE_AMD64=0x8664,IMAGE_FILE_MACHINE_I386=0x014c;
constexpr int MOVEFILE_DELAY_UNTIL_REBOOT=4;const wchar_t* RT_RCDATA=L"RCDATA";
struct Fake {
 bool mkdirOk=true,extractOk=true,peOk=true,deleteOk=true,removeOk=true,scheduleOk=true;
 int dirs=0,extracts=0,deleted=0,removed=0,scheduled=0,logs=0;
 std::vector<UINT> resources;std::vector<WORD> machines;std::vector<std::wstring> paths;
} fake;
DWORD GetLastError(){return 5;}
BOOL DeleteFileW(const wchar_t* p){++fake.deleted;fake.paths.push_back(p);return fake.deleteOk;}
BOOL RemoveDirectoryW(const wchar_t* p){++fake.removed;fake.paths.push_back(p);return fake.removeOk;}
BOOL MoveFileExW(const wchar_t* p,const wchar_t*,int flags){assert(flags==MOVEFILE_DELAY_UNTIL_REBOOT);++fake.scheduled;fake.paths.push_back(p);return fake.scheduleOk;}
namespace GlassInstallLog { template<class... T>void LineFormat(const wchar_t*,T...){++fake.logs;} }
namespace GlassInstallUtil {
 bool CreateSecureTempDirectory(const wchar_t* prefix,std::wstring& out){
  if(!fake.mkdirOk)return false;++fake.dirs;out=std::wstring(L"secure/")+prefix+std::to_wstring(fake.dirs);return true;
 }
 bool ExtractResourceToFile(HINSTANCE instance,UINT id,const wchar_t*,const std::wstring&){
  assert(instance);++fake.extracts;fake.resources.push_back(id);return fake.extractOk;
 }
 bool PeMatches(const std::wstring&,WORD machine,const char*){fake.machines.push_back(machine);return fake.peOk;}
'''
main = r'''
int main(){
 using namespace GlassInstallUtil;
 auto reset=[](){fake={};};
 reset();ConfigureEmbeddedUnlocker(nullptr,216,217);assert(EmbeddedNativeDllX86().empty());assert(fake.dirs==0);
 reset();ConfigureEmbeddedUnlocker(reinterpret_cast<HINSTANCE>(1),216,0);assert(EmbeddedNativeDllX86().empty());
 reset();ConfigureEmbeddedUnlocker(reinterpret_cast<HINSTANCE>(1),216,217);fake.mkdirOk=false;
 assert(EmbeddedNativeDllX86().empty());assert(fake.extracts==0);
 reset();ConfigureEmbeddedUnlocker(reinterpret_cast<HINSTANCE>(1),216,217);fake.extractOk=false;
 assert(EmbeddedNativeDllX86().empty());assert(fake.extracts==1 && fake.deleted==1 && fake.removed==1);
 reset();ConfigureEmbeddedUnlocker(reinterpret_cast<HINSTANCE>(1),216,217);fake.peOk=false;
 assert(EmbeddedNativeDllX86().empty());assert(fake.deleted==1 && fake.removed==1 && fake.machines[0]==IMAGE_FILE_MACHINE_I386);
 reset();ConfigureEmbeddedUnlocker(reinterpret_cast<HINSTANCE>(1),216,217);
 const auto path=EmbeddedNativeDllX86();assert(!path.empty() && path.find(L"GlassUnlocker86")!=std::wstring::npos);
 assert(fake.resources.size()==1 && fake.resources[0]==217 && fake.machines[0]==IMAGE_FILE_MACHINE_I386);
 assert(EmbeddedNativeDllX86()==path && fake.extracts==1 && fake.dirs==1);
 assert(!EnsureExtractedUnlockerX64().empty());assert(fake.resources.back()==216 && fake.machines.back()==IMAGE_FILE_MACHINE_AMD64);
 CleanupEmbeddedUnlocker();assert(fake.deleted==2 && fake.removed==2 && fake.scheduled==0);
 CleanupEmbeddedUnlocker();assert(fake.deleted==2 && fake.removed==2);
 reset();ConfigureEmbeddedUnlocker(reinterpret_cast<HINSTANCE>(1),216,217);assert(!EmbeddedNativeDllX86().empty());
 fake.deleteOk=false;fake.removeOk=false;CleanupEmbeddedUnlocker();assert(fake.scheduled==2 && fake.logs==2);
 std::cout<<"PASS: embedded Native DLL extraction, caching, cleanup and deferred-deletion handling\n";
}
'''
with tempfile.TemporaryDirectory(prefix="glass-embedded-test-") as directory:
    cpp = Path(directory) / "embedded.cpp"
    exe = Path(directory) / "embedded"
    cpp.write_text(prefix + "\nnamespace {\n" + globals_part + x64 + "\n}\n" + configure + "\n" + x86 + "\n" + cleanup + "\n}\n" + main,
                   encoding="utf-8")
    compiler = os.environ.get("CXX", "g++")
    subprocess.run([compiler, "-std=c++17", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-pthread", str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
