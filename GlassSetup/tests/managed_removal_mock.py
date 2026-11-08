"""Run actual Setup removal/auxiliary preparation against API seams.
Does not run Windows Installer, Task Scheduler, shell registration or signatures.
"""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallEngine.cpp').read_text()
code=s[s.index('bool InstallEngine::PrepareManagedRemoval()'):s.index('bool InstallEngine::RunMsiSequence()')]
prefix=r'''
#include "GlassProductPlan.h"
#include "GlassProducts.h"
#include <atomic>
#include <cassert>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#define PRIMARYLANGID(x) (x)
#define _wcsicmp wcscasecmp
#include <cwchar>
constexpr unsigned LANG_RUSSIAN=25,ERROR_SUCCESS=0,ERROR_INSTALL_FAILURE=1603,ERROR_INSTALL_USEREXIT=1602,
 ERROR_INSTALL_PACKAGE_REJECTED=1625,ERROR_SUCCESS_REBOOT_REQUIRED=3010,ERROR_SUCCESS_REBOOT_INITIATED=1641,
 INSTALLLOGMODE_VERBOSE=1,INSTALLLOGMODE_ERROR=2,INSTALLLOGMODE_WARNING=4,INSTALLLOGATTRIBUTES_FLUSHEACHLINE=1;
using UINT=unsigned;
namespace GP{enum class PlatformArch{X64,X86};}
constexpr unsigned IMAGE_FILE_MACHINE_AMD64=0x8664,IMAGE_FILE_MACHINE_I386=0x14c;
unsigned GetUserDefaultUILanguage(){return 0;}
struct State{std::map<std::wstring,bool> installed;std::vector<std::wstring> calls,modified;
 std::wstring baselineBad;bool prepare=true,shell=true,conflict=false,contexts=true,cancel=false,msi=true,stillPresent=false;
 bool x86=false,x64=false,og=false,shortcut=false,helper=true;unsigned result=0;}s;
namespace GlassInstallMsi{
 constexpr const wchar_t* kProductPlatformX64=L"p64",*kLegacyPlatformX64=L"old64",*kProductPlatformX86=L"p32",*kLegacyPlatformX86=L"old32",
 *kProductSettingsX64=L"s64",*kProductSettingsX86=L"s32",*kProductOpenGlass=L"og",*kLegacyOpenGlass=L"oldog";
 enum class ProductPathCheck{Safe,Unsafe};
 struct ProgressSink{std::wstring label;int base=0,span=0;std::atomic_bool* cancelRequested=nullptr;
 std::function<void(const std::wstring&)> onError;std::function<void(const std::wstring&,int)> onProgress;};
 bool IsProductInstalled(const wchar_t* code){return s.installed[code];}
 ProductPathCheck CheckProductContexts(const wchar_t*,std::wstring&,const std::atomic_bool*){return s.contexts?ProductPathCheck::Safe:ProductPathCheck::Unsafe;}
 bool UninstallProduct(const wchar_t* code,ProgressSink* sink,UINT* result){
  s.calls.emplace_back(code);*result=s.result;if(s.cancel){sink->cancelRequested->store(true);*result=ERROR_INSTALL_USEREXIT;return false;}
  if(s.msi&&!s.stillPresent)s.installed[code]=false;return s.msi;
 }
}
namespace GlassInstallRollback{struct Snapshot{std::wstring productCode;};struct SessionState{std::wstring workspaceDir=L"backup";std::vector<Snapshot> oldProducts;};
 void NoteModified(SessionState&,const wchar_t* code){s.modified.emplace_back(code);}}
namespace GlassInstallRemoval{struct Session{};
 bool Prepare(Session&,bool x86,bool og,bool shortcut,const std::wstring&,const std::wstring&,const std::wstring&,const std::wstring&,const std::atomic_bool*,bool,std::wstring&,bool x64){
  s.x86=x86;s.x64=x64;s.og=og;s.shortcut=shortcut;return s.prepare;
 }}
namespace GlassInstallUtil{
 constexpr char kInstallerShellExport[]="shell";
 std::wstring NativeDllForArch(GP::PlatformArch,const std::wstring&,const std::wstring&){return s.helper?L"native.dll":L"";}
 bool PeMatches(const std::wstring&,unsigned,const char*){return s.helper;}
 bool InvokeShellExtRegistration(bool registration,const std::wstring&,const std::wstring&,bool* changed,bool* conflict,std::wstring*){
 assert(!registration);s.calls.push_back(L"shell");*changed=s.shell;*conflict=s.conflict;return s.shell;
 }}
namespace InstallLog{std::wstring NextMsiVerboseLogPath(const wchar_t*) noexcept{return L"log";}
 template<class... T>void LineFormat(const wchar_t*,T...){} }
void MsiEnableLogW(unsigned,const wchar_t*,unsigned){}
struct InstallEngine{
 GlassProducts::Selection selection_;GlassProductPlan::Plan executionPlan_;bool executionPlanReady_=true,scanNativeX64_=true,shellRemovedForMaintenance_=false,family=true;
 std::atomic_bool cancelRequested_{false};GlassInstallRemoval::Session removalSession_;GlassInstallRollback::SessionState rollbackSession_;
 std::wstring x64Dir_=L"64",x86Dir_=L"32",openGlassDir_=L"OG";int failures=0,warnings=0;
 bool IsCancelRequested(){return cancelRequested_.load();}
 void SetFailure(const std::wstring&,unsigned){++failures;}
 void AddFinishWarning(const std::wstring&,const std::wstring&){++warnings;}
 void SetStatus(const std::wstring&,int){}void RememberMsiMessage(const wchar_t*){}
 bool RecheckRelatedProducts(const wchar_t*){return family;}
 bool RecheckSnapshottedProduct(const wchar_t* code){return code!=s.baselineBad;}
 bool PrepareManagedRemoval() noexcept;bool RemoveManagedProduct(GlassProductPlan::Product) noexcept;
};
'''
main=r'''
using namespace GlassProductPlan;
void reset(InstallEngine& e,Product p){s={};e.selection_.platform64=false;e.selection_.platform32=false;e.selection_.glassSettings=false;e.selection_.openGlass=false;
 e.selection_.independentSettings=true;e.selection_.removeMask=1u<<Index(p);e.executionPlan_.rows[Index(p)].action=Action::Remove;}
bool Run(InstallEngine& e,Product p){e.rollbackSession_.oldProducts.clear();for(auto& [code,present]:s.installed)if(present)e.rollbackSession_.oldProducts.push_back({code});return e.RemoveManagedProduct(p);}
int main(){
 for(auto [p,code]:{std::pair{Product::Platform64,L"p64"},{Product::Platform32,L"p32"},{Product::Settings64,L"s64"},{Product::Settings32,L"s32"},{Product::OpenGlass,L"og"}}){
  InstallEngine e;reset(e,p);s.installed[code]=true;assert(Run(e,p));assert(s.modified==std::vector<std::wstring>{code});assert(!s.installed[code]);
 }
 {InstallEngine e;reset(e,Product::Platform32);s.installed[L"old32"]=true;assert(Run(e,Product::Platform32));assert(s.calls==std::vector<std::wstring>{L"old32"});}
 {InstallEngine e;reset(e,Product::Platform32);s.installed[L"p32"]=s.installed[L"old32"]=true;assert(Run(e,Product::Platform32));assert(s.modified.size()==2);}
 {InstallEngine e;reset(e,Product::Platform64);s.installed[L"p64"]=true;s.shell=false;assert(!Run(e,Product::Platform64));assert(s.modified.empty());assert(e.failures==1);}
 {InstallEngine e;reset(e,Product::Platform32);s.installed[L"p32"]=true;s.baselineBad=L"p32";assert(!Run(e,Product::Platform32));assert(s.modified.empty());}
 {InstallEngine e;reset(e,Product::Platform32);s.installed[L"p32"]=true;s.msi=false;s.result=1603;assert(!Run(e,Product::Platform32));assert(s.modified.size()==1&&e.failures==1);}
 {InstallEngine e;reset(e,Product::Platform32);s.installed[L"p32"]=true;s.cancel=true;assert(!Run(e,Product::Platform32));assert(s.modified.size()==1&&e.IsCancelRequested()&&e.failures==0);}
 {InstallEngine e;reset(e,Product::Platform32);assert(!e.RemoveManagedProduct(Product::Platform32));assert(s.calls.empty());}
 {InstallEngine e;reset(e,Product::Platform32);s.installed[L"p32"]=true;s.contexts=false;assert(!Run(e,Product::Platform32));assert(s.modified.empty());}
 {InstallEngine e;reset(e,Product::Platform32);e.family=false;assert(!Run(e,Product::Platform32));assert(s.modified.empty());}
 {InstallEngine e;reset(e,Product::Platform32);e.executionPlan_.rows[1].action=Action::Keep;assert(!Run(e,Product::Platform32));assert(s.modified.empty());}
 {InstallEngine e;reset(e,Product::Platform32);s.installed[L"p32"]=true;s.stillPresent=true;assert(!Run(e,Product::Platform32));assert(e.failures==1);}
 {InstallEngine e;reset(e,Product::OpenGlass);s.installed[L"og"]=true;s.result=3010;assert(Run(e,Product::OpenGlass));assert(e.warnings==1);}
 {InstallEngine e;reset(e,Product::Platform64);assert(e.PrepareManagedRemoval());assert(s.x64&&!s.x86&&!s.og);}
 {InstallEngine e;reset(e,Product::Platform64);s.helper=false;assert(!e.PrepareManagedRemoval());assert(e.failures==1&&s.calls.empty());}
 {InstallEngine e;reset(e,Product::Settings64);s.installed[L"s64"]=s.installed[L"s32"]=true;assert(e.PrepareManagedRemoval());assert(!s.shortcut);}
 {InstallEngine e;reset(e,Product::Settings64);s.installed[L"s64"]=true;assert(e.PrepareManagedRemoval());assert(s.shortcut);}
 {InstallEngine e;reset(e,Product::Settings64);s.prepare=false;assert(!e.PrepareManagedRemoval());assert(e.failures==1);}
 std::cout<<"PASS: actual managed Remove adapters: five products, legacy, snapshot before MSI, failure/cancel, unchanged presence, reboot, shell guard, auxiliary/shortcut selection\n";
}
'''
with tempfile.TemporaryDirectory(prefix='ManagedRemoval-') as d:
    path=Path(d)/'test.cpp';exe=Path(d)/'test';path.write_text(prefix+code+main)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined','-no-pie','-I'+str(root/'GlassSetup/src'),str(path),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
