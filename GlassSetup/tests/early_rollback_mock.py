#!/usr/bin/env python3
"""Exercise real rollback with only the early Settings MSI changed.

Win32, MSI, and filesystem operations are mocked; real Windows remains required.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "GlassSetup/src/InstallSupport/GlassInstallRollback.cpp").read_text(encoding="utf-8")

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

rollback = function("bool ExecuteRollback(")
prefix = r'''
#include <cassert>
#include <cstdint>
#include <cwchar>
#include <functional>
#include <iostream>
#include <string>
#include <vector>
using DWORD=std::uint32_t;using UINT=unsigned;using uint64_t=std::uint64_t;
#define _wcsicmp wcscasecmp
constexpr DWORD INVALID_FILE_ATTRIBUTES=0xffffffffu,FILE_ATTRIBUTE_DIRECTORY=0x10,FILE_ATTRIBUTE_REPARSE_POINT=0x400,
 ERROR_FILE_NOT_FOUND=2,ERROR_PATH_NOT_FOUND=3,ERROR_GEN_FAILURE=31,ERROR_NOT_SUPPORTED=50,ERROR_DISK_FULL=112;
struct Fake {
 bool os64=true,settingDirExists=true,settingReparse=false,uninstallOk=true,registryOk=true,otherFilesLocked=false;
 DWORD attrError=ERROR_FILE_NOT_FOUND;std::vector<std::wstring> copies,uninstalls,installs;int registryCalls=0;
} fake;
DWORD GetLastError(){return fake.attrError;}
DWORD GetFileAttributesW(const wchar_t*){
 if(!fake.settingDirExists)return INVALID_FILE_ATTRIBUTES;
 return FILE_ATTRIBUTE_DIRECTORY | (fake.settingReparse?FILE_ATTRIBUTE_REPARSE_POINT:0);
}
int DeleteFileW(const wchar_t*){return 1;}
namespace GlassInstallRollback {
 enum class RestoreScope { AllProducts, SettingsX86, SettingsX64 };
 struct ProductSnapshot{std::wstring productCode,installDir,cachedMsiPath,displayVersion;};
 struct SessionState {
  std::wstring workspaceDir=L"workspace",x64Dir=L"root64",x86Dir=L"root86",openGlassDir=L"rootOG";
  std::wstring backupX64=L"backup64",backupX86=L"backup86",backupOpenGlass=L"backupOG";
  std::vector<ProductSnapshot> oldProducts;std::vector<std::wstring> installedThisSession,modifiedProducts;
  bool hadPriorInstall=true;unsigned copyError=0;
 };
 using CopyProgressFn=std::function<void(int)>;
 bool RestoreRegistryValues(const SessionState&){++fake.registryCalls;return fake.registryOk;}
}
namespace GlassInstallUtil {
 bool IsOs64Bit(){return fake.os64;}
 std::wstring TrimTrailingSlashes(std::wstring value){while(!value.empty()&&(value.back()==L'\\'||value.back()==L'/'))value.pop_back();return value;}
}
namespace GlassInstallLog { template<class... T>void LineFormat(const wchar_t*,T...){} void Line(const wchar_t*){} }
namespace GlassInstallMsi {
 constexpr wchar_t kProductPlatformX64[]=L"p64",kProductPlatformX86[]=L"p86",kProductSettingsX64[]=L"gs64",
  kProductSettingsX86[]=L"gs86",kProductOpenGlass[]=L"og";
 bool UninstallProduct(const wchar_t* code){fake.uninstalls.emplace_back(code);return fake.uninstallOk;}
 bool IsProductInstalled(const wchar_t*){return false;}
 std::wstring QueryDisplayVersion(const wchar_t*){return L"old";}
 bool PrepareRestoreMsiCopy(const std::wstring&,const std::wstring&){return true;}
 std::wstring BuildInstallPropertyString(const std::wstring&,bool){return L"props";}
 UINT InstallProduct(const std::wstring& path,const std::wstring&,void*){fake.installs.push_back(path);return 0;}
 bool MsiSuccess(UINT code){return code==0;}
 void PatchArpUninstallString(const std::wstring&,const std::wstring&){}
}
uint64_t DirBytes(const std::wstring&){return 10;}
bool CopyTree(const std::wstring& from,const std::wstring& to,const std::wstring&,uint64_t& copied,uint64_t,
 const GlassInstallRollback::CopyProgressFn& onProgress,unsigned&){
 fake.copies.push_back(from+L" -> "+to);copied+=10;if(onProgress)onProgress(5000);
 return !(fake.otherFilesLocked && to.find(L"GlassSettings")==std::wstring::npos);
}
'''
main = r'''
int main(){
 using namespace GlassInstallRollback;
 auto reset=[](){fake={};};
 for(bool x64:{false,true}){
  reset();fake.os64=!x64;fake.otherFilesLocked=true;SessionState s;
  const std::wstring code=x64?L"gs64":L"gs86";
  s.installedThisSession={code};s.modifiedProducts={code};
  s.oldProducts.push_back({code,x64?L"root64":L"root86",L"old.msi",L"old"});
  s.oldProducts.push_back({L"p64",L"root64",L"platform.msi",L"old"});
  int last=0;assert(ExecuteRollback(s,[&](int p){last=p;},x64?RestoreScope::SettingsX64:RestoreScope::SettingsX86));
  assert(fake.uninstalls.size()==1 && fake.uninstalls[0]==code);
  assert(fake.copies.size()==1 && fake.copies[0]==(x64?L"backup64\\GlassSettings -> root64\\GlassSettings"
                                                       :L"backup86\\GlassSettings -> root86\\GlassSettings"));
  assert(fake.installs.size()==1 && fake.installs[0].find(code)!=std::wstring::npos);
  assert(fake.registryCalls==1 && last==10000);
 }
 {reset();SessionState s;fake.settingDirExists=false;int last=0;
  assert(ExecuteRollback(s,[&](int p){last=p;},RestoreScope::SettingsX64));assert(fake.copies.empty() && fake.registryCalls==1 && last==10000);}
 {reset();SessionState s;fake.settingDirExists=false;fake.attrError=ERROR_GEN_FAILURE;
  assert(!ExecuteRollback(s,nullptr,RestoreScope::SettingsX64));assert(s.copyError==ERROR_GEN_FAILURE && fake.copies.empty());}
 {reset();SessionState s;fake.settingReparse=true;
  assert(!ExecuteRollback(s,nullptr,RestoreScope::SettingsX64));assert(s.copyError==ERROR_NOT_SUPPORTED && fake.copies.empty());}
 {reset();SessionState s;assert(ExecuteRollback(s,nullptr,RestoreScope::AllProducts));
  assert(fake.copies.size()==3 && fake.registryCalls==1);}
 {reset();SessionState s;fake.otherFilesLocked=true;assert(!ExecuteRollback(s,nullptr,RestoreScope::AllProducts));assert(fake.copies.size()==3);}
 {reset();SessionState s;s.installedThisSession={L"gs64"};fake.uninstallOk=false;
  assert(!ExecuteRollback(s,nullptr,RestoreScope::SettingsX64));assert(fake.uninstalls.size()==1 && fake.registryCalls==1);}
 std::cout<<"PASS: real rollback: only Settings restored during unlock, full snapshot afterward, errors retained\n";
}
'''
with tempfile.TemporaryDirectory(prefix="glass-rollback-test-") as directory:
    cpp = Path(directory) / "rollback.cpp"
    exe = Path(directory) / "rollback"
    cpp.write_text(prefix + "\nnamespace GlassInstallRollback {\n" + rollback + "\n}\n" + main, encoding="utf-8")
    compiler = os.environ.get("CXX", "g++")
    subprocess.run([compiler, "-std=c++17", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
