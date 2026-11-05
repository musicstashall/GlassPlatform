#!/usr/bin/env python3
"""Exercise the actual preflight on a clean machine with only EXE resources.
No installed Settings resolver, extraction, DLL execution or Windows APIs.
Does not replace an MSVC/WiX build or Windows VM test.
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
for forbidden in ("LoadLibrary", "ScanGlassModules", "NativeDllForArch(", "EmbeddedNativeDllX86(", "ExtractResourceToFile("):
    assert forbidden not in preflight

prefix = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
using WORD=std::uint16_t;using DWORD=std::uint32_t;using UINT=unsigned;using HINSTANCE=void*;
constexpr WORD IMAGE_FILE_MACHINE_I386=0x14c, IMAGE_FILE_MACHINE_AMD64=0x8664;
constexpr int LANG_RUSSIAN=25;
constexpr UINT IDR_UNLOCKER_X86=217,IDR_UNLOCKER_X64=216;
#define PRIMARYLANGID(x) (x)
int GetUserDefaultUILanguage(){return LANG_RUSSIAN;}
namespace Shared::InjectModuleScan {constexpr char kNativeInstallerProtocolExport[]="GlassSettings_InstallerScanProtocolV2";}
namespace InstallLog {template<class... T>void LineFormat(const wchar_t*,T...) {}}
struct Mock {
 bool x86Present=true,x86Protocol=true,x86Shell=true,x64Present=true,x64Protocol=true,x64Shell=true;
 bool x86Machine=true,x64Machine=true;
 int cancelOnCheck=0;DWORD error=0;std::vector<UINT> checked;
 std::atomic_bool* cancelled=nullptr;
} fake;
DWORD GetLastError(){return fake.error;}
namespace GlassInstallUtil {
 constexpr char kInstallerShellExport[]="GlassSettings_ShellExtRegisterSetupV1W";
 bool ResourcePeMatches(HINSTANCE instance,UINT resource,WORD machine,const char* marker){
  assert(instance==reinterpret_cast<HINSTANCE>(1));
  const bool x86=resource==IDR_UNLOCKER_X86;assert(x86 || resource==IDR_UNLOCKER_X64);
  assert(machine==(x86?IMAGE_FILE_MACHINE_I386:IMAGE_FILE_MACHINE_AMD64));
  fake.checked.push_back(resource);
  if(fake.cancelOnCheck==static_cast<int>(fake.checked.size()) && fake.cancelled)fake.cancelled->store(true);
  const bool present=x86?fake.x86Present:fake.x64Present;
  if(!present){fake.error=1814;return false;}
  bool valid=x86?fake.x86Machine:fake.x64Machine;
  if(marker){
   const bool scan=std::string(marker)==Shared::InjectModuleScan::kNativeInstallerProtocolExport;
   assert(scan || std::string(marker)==kInstallerShellExport);
   valid=valid && (scan?(x86?fake.x86Protocol:fake.x64Protocol):(x86?fake.x86Shell:fake.x64Shell));
  }
  fake.error=valid?0:193;return valid;
 }
}
class InstallEngine {
public:
 HINSTANCE instance_=reinterpret_cast<HINSTANCE>(1);
 bool scanNativeX64_=true;std::atomic_bool cancel{false};std::wstring error_;DWORD errorCode=0;
 bool IsCancelRequested()const{return cancel.load();}
 void SetFailure(const std::wstring& message,DWORD code){error_=message;errorCode=code;}
 bool VerifyEmbeddedNativePayload() noexcept;
};
'''
main = r'''
int main(){
 auto reset=[](){fake=Mock{};};
 reset();{InstallEngine e;assert(e.VerifyEmbeddedNativePayload());assert(e.error_.empty());
  assert(fake.checked==std::vector<UINT>({217,217,217,216,216,216}));}
 reset();{InstallEngine e;e.scanNativeX64_=false;assert(e.VerifyEmbeddedNativePayload());
  assert(fake.checked==std::vector<UINT>({217,217,217}));}
 reset();{InstallEngine e;fake.x86Present=false;assert(!e.VerifyEmbeddedNativePayload());
  assert(e.error_.find(L"x86")!=std::wstring::npos&&e.errorCode==1814&&fake.checked.size()==1);}
 reset();{InstallEngine e;fake.x64Present=false;assert(!e.VerifyEmbeddedNativePayload());
  assert(e.error_.find(L"x64")!=std::wstring::npos&&e.errorCode==1814&&fake.checked.size()==4);}
 for(int arch=0;arch<2;++arch)for(int stage=0;stage<3;++stage){
  reset();InstallEngine e;
  if(!arch){if(!stage)fake.x86Machine=false;else if(stage==1)fake.x86Protocol=false;else fake.x86Shell=false;}
  else{if(!stage)fake.x64Machine=false;else if(stage==1)fake.x64Protocol=false;else fake.x64Shell=false;}
  assert(!e.VerifyEmbeddedNativePayload());assert(!e.error_.empty()&&e.errorCode==193);
  assert(fake.checked.size()==static_cast<unsigned>(arch*3+stage+1));
 }
 reset();{InstallEngine e;e.cancel.store(true);assert(!e.VerifyEmbeddedNativePayload());assert(e.error_.empty()&&fake.checked.empty());}
 for(int at=1;at<=6;++at){reset();InstallEngine e;fake.cancelled=&e.cancel;fake.cancelOnCheck=at;
  assert(!e.VerifyEmbeddedNativePayload());assert(e.error_.empty());assert(fake.checked.size()==static_cast<unsigned>(at));}
 reset();{InstallEngine e;fake.cancelled=&e.cancel;fake.cancelOnCheck=2;fake.x86Protocol=false;
  assert(!e.VerifyEmbeddedNativePayload());assert(e.error_.empty()&&fake.checked.size()==2);}
 std::cout<<"PASS: clean-machine embedded-resource preflight, x86/x64 header/exports, missing/corrupt resources, error codes, cancellation; no installed helper or temp extraction\n";
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
