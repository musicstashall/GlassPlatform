#!/usr/bin/env python3
"""Exercise Setup's real static Native preflight without executing a DLL.

These deterministic mocks do not replace an MSVC build or Windows VM test.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "GlassSetup/src/InstallEngine.cpp").read_text(encoding="utf-8")

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

preflight = function("bool InstallEngine::VerifyEmbeddedNativePayload(")
worker = source[source.index("DWORD WINAPI InstallEngine::WorkerThread("):source.index("bool InstallEngine::Start(")]
assert worker.index("ExtractPayload(") < worker.index("VerifyEmbeddedNativePayload()") < worker.index("CreateRollbackSnapshot()")
assert worker.index("VerifyEmbeddedNativePayload()") < worker.index("StopRunningHosts()") < worker.index("InstallGlassSettingsEarly()")
assert worker.index("InstallGlassSettingsEarly()") < worker.index("ReleaseLockedModules()")
assert "LoadLibrary" not in preflight and "ScanGlassModules" not in preflight

prefix = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
using WORD=std::uint16_t;
constexpr WORD IMAGE_FILE_MACHINE_I386=0x14c, IMAGE_FILE_MACHINE_AMD64=0x8664;
constexpr int LANG_RUSSIAN=25;
#define PRIMARYLANGID(x) (x)
int GetUserDefaultUILanguage(){return LANG_RUSSIAN;}
namespace Shared::InjectModuleScan {constexpr char kNativeInstallerProtocolExport[]="GlassSettings_InstallerScanProtocolV1";}
namespace GP {enum class PlatformArch{X86,X64};}
namespace InstallLog {template<class... T>void LineFormat(const wchar_t*,T...) {}}
struct Mock {
 bool x86Present=true,x86Protocol=true,x64Present=true,x64Protocol=true,cancelDuringX86=false;
 int x86Extracts=0,x64Extracts=0;std::vector<WORD> checked;
 std::atomic_bool* cancelled=nullptr;
} fake;
namespace GlassInstallUtil {
 std::wstring EmbeddedNativeDllX86(){++fake.x86Extracts;return fake.x86Present?L"bundled86.dll":L"";}
 std::wstring NativeDllForArch(GP::PlatformArch arch,const std::wstring&,const std::wstring&){
  assert(arch==GP::PlatformArch::X64);++fake.x64Extracts;return fake.x64Present?L"bundled64.dll":L"";
 }
 bool PeMatches(const std::wstring& path,WORD machine,const char* marker){
  assert(std::string(marker)==Shared::InjectModuleScan::kNativeInstallerProtocolExport);
  fake.checked.push_back(machine);
  if(path==L"bundled86.dll"){
   assert(machine==IMAGE_FILE_MACHINE_I386);
   if(fake.cancelDuringX86 && fake.cancelled)fake.cancelled->store(true);
   return fake.x86Protocol;
  }
  assert(path==L"bundled64.dll" && machine==IMAGE_FILE_MACHINE_AMD64);return fake.x64Protocol;
 }
}
class InstallEngine {
public:
 bool scanNativeX64_=true;std::atomic_bool cancel{false};std::wstring x64Dir_,x86Dir_,error_;
 bool IsCancelRequested()const{return cancel.load();}
 void SetFailure(const std::wstring& message,unsigned){error_=message;}
 bool VerifyEmbeddedNativePayload() noexcept;
};
'''
main = r'''
int main(){
 auto reset=[](){fake=Mock{};};
 reset();{InstallEngine e;assert(e.VerifyEmbeddedNativePayload());assert(e.error_.empty());
  assert(fake.x86Extracts==1 && fake.x64Extracts==1 && fake.checked.size()==2);}
 reset();{InstallEngine e;e.scanNativeX64_=false;assert(e.VerifyEmbeddedNativePayload());
  assert(fake.x64Extracts==0 && fake.checked.size()==1);}
 reset();{InstallEngine e;fake.x86Present=false;assert(!e.VerifyEmbeddedNativePayload());
  assert(!e.error_.empty() && e.error_.find(L"x86")!=std::wstring::npos && fake.x64Extracts==0);}
 reset();{InstallEngine e;fake.x86Protocol=false;assert(!e.VerifyEmbeddedNativePayload());
  assert(!e.error_.empty() && fake.checked.size()==1 && fake.x64Extracts==0);}
 reset();{InstallEngine e;fake.x64Present=false;assert(!e.VerifyEmbeddedNativePayload());
  assert(e.error_.find(L"x64")!=std::wstring::npos && fake.checked.size()==1);}
 reset();{InstallEngine e;fake.x64Protocol=false;assert(!e.VerifyEmbeddedNativePayload());
  assert(!e.error_.empty() && fake.checked.size()==2);}
 reset();{InstallEngine e;e.cancel.store(true);assert(!e.VerifyEmbeddedNativePayload());
  assert(e.error_.empty() && fake.checked.empty() && fake.x64Extracts==0);}
 reset();{InstallEngine e;fake.x86Protocol=false;fake.cancelDuringX86=true;fake.cancelled=&e.cancel;
  assert(!e.VerifyEmbeddedNativePayload());assert(e.error_.empty() && fake.x64Extracts==0);}
 reset();{InstallEngine e;fake.cancelDuringX86=true;fake.cancelled=&e.cancel;
  assert(!e.VerifyEmbeddedNativePayload());assert(e.error_.empty() && fake.x64Extracts==0);}
 std::cout<<"PASS: static preflight x86/x64, old/missing DLLs, ordering and cancellation\n";
}
'''

with tempfile.TemporaryDirectory(prefix="glass-native-preflight-") as directory:
    cpp = Path(directory) / "test.cpp"
    exe = Path(directory) / "test"
    cpp.write_text(prefix + preflight + "\n" + main, encoding="utf-8")
    compiler = os.environ.get("CXX", "g++")
    subprocess.run([compiler, "-std=c++17", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-no-pie", str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
