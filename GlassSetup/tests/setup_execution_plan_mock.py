"""Compile the ACTUAL Setup discovery/Build adapter with fake read-only APIs.
No MSI transactions, processes, registry, network, or Windows security exercised.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'GlassSetup/src/InstallEngine.cpp').read_text()
adapter=source[source.index('bool InstallEngine::BuildExecutionPlan() noexcept {'):source.index('bool InstallEngine::RunMsiSequence() noexcept {')]
harness=r'''
#include "GlassProductPlan.h"
#include "GlassProducts.h"
#include "InstallSupport/GlassPackageRules.h"
#include <atomic>
#include <cassert>
#include <cwchar>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#define _wcsicmp wcscasecmp
#define PRIMARYLANGID(x) (x)
constexpr unsigned LANG_RUSSIAN=25,ERROR_INSTALL_PACKAGE_REJECTED=1625;
constexpr int INSTALLSTATE_DEFAULT=5,INSTALLSTATE_UNKNOWN=-1,INSTALLSTATE_ADVERTISED=1;
unsigned GetUserDefaultUILanguage(){return 0;}
namespace InstallLog {template<class... Args>void LineFormat(const wchar_t*,Args...) {}}
namespace GlassProductInspect {
struct ProductReport {bool installed=false,integrityKnown=false,damaged=false;};
struct Report {bool ran=true;ProductReport x64,x86,settings64,settings32,og;};
struct Inputs {bool os64=false,inspectSources=true;const std::atomic_bool* cancelled=nullptr;};
Report observation;
Report Run(const Inputs& input){assert(!input.inspectSources);return observation;}
}
namespace GlassInstallMsi {
constexpr const wchar_t* kProductPlatformX64=L"p64",*kLegacyPlatformX64=L"old64",
 *kProductPlatformX86=L"p32",*kLegacyPlatformX86=L"old32",*kProductSettingsX64=L"s64",
 *kProductSettingsX86=L"s32",*kProductOpenGlass=L"og",*kLegacyOpenGlass=L"oldog";
enum class ProductPathCheck {Safe,Unsafe};
std::map<std::wstring,int> states;
std::wstring unsafe;
ProductPathCheck CheckProductContexts(const wchar_t* code,std::wstring&,const std::atomic_bool*){
 return code==unsafe?ProductPathCheck::Unsafe:ProductPathCheck::Safe;
}
}
int stateReads=0;bool concurrent=false;
int MsiQueryProductStateW(const wchar_t* code){
 if(concurrent && ++stateReads==3)GlassInstallMsi::states[code]=INSTALLSTATE_DEFAULT;
 auto it=GlassInstallMsi::states.find(code);return it==GlassInstallMsi::states.end()?INSTALLSTATE_UNKNOWN:it->second;
}
namespace GlassMaintenanceCache {
std::map<std::wstring,std::pair<std::wstring,std::wstring>> media;
bool Identity(const std::wstring& path,std::wstring& code,std::wstring& version){
 auto it=media.find(path);if(it==media.end())return false;
 code=it->second.first;version=it->second.second;return true;
}
}
struct Old {std::wstring productCode,cachedMsiPath;};
struct InstallEngine {
 GlassProductPlan::Plan executionPlan_{};bool executionPlanReady_=true,scanNativeX64_=true,cancelled=false;
 std::atomic_bool cancelRequested_{false};GlassProducts::Selection selection_{};
 std::wstring tempDir_=L"work",badBaseline,badFamily;
 struct {std::vector<Old> oldProducts;}rollbackSession_;
 int failures=0,baselineChecks=0,familyChecks=0;
 bool IsCancelRequested()const{return cancelled;}
 void SetFailure(const wchar_t*,unsigned){++failures;}
 bool RecheckSnapshottedProduct(const wchar_t* code){++baselineChecks;return code!=badBaseline;}
 bool RecheckRelatedProducts(const wchar_t* code){++familyChecks;return code!=badFamily;}
 bool BuildExecutionPlan() noexcept;
};
// ADAPTER
using namespace GlassProductPlan;
void Reset(InstallEngine& e){
 e.selection_.platform64=false;e.selection_.platform32=false;e.selection_.glassSettings=false;
 e.selection_.openGlass=false;e.selection_.independentSettings=true;
 GlassProductInspect::observation={};GlassInstallMsi::states.clear();GlassInstallMsi::unsafe.clear();
 GlassMaintenanceCache::media={{L"work\\Glass-x64.msi",{L"p64",L"2.0.0"}},
  {L"work\\Glass-x86.msi",{L"p32",L"2.0.0"}},
  {L"work\\GlassSettings-x64.msi",{L"s64",L"2.0.0"}},
  {L"work\\GlassSettings-x86.msi",{L"s32",L"2.0.0"}},
  {L"work\\OpenGlass-x64.msi",{L"og",L"2.0.0"}}};
 stateReads=0;concurrent=false;
}
void Installed(InstallEngine& e,const wchar_t* code,const wchar_t* version){
 GlassInstallMsi::states[code]=INSTALLSTATE_DEFAULT;
 const auto path=std::wstring(L"baseline-")+code;
 e.rollbackSession_.oldProducts.push_back({code,path});
 GlassMaintenanceCache::media[path]={code,version};
 auto& r=GlassProductInspect::observation.x64;r.installed=true;r.integrityKnown=true;
}
int main(){
 {InstallEngine e;Reset(e);e.selection_.platform64=true;assert(e.BuildExecutionPlan());
  assert(e.executionPlanReady_&&e.executionPlan_.rows[Index(Product::Platform64)].action==Action::Install);}
 for(const auto version:{L"1.0.0",L"2.0.0",L"3.0.0"}){
  InstallEngine e;Reset(e);e.selection_.platform64=true;Installed(e,L"p64",version);
  bool ok=e.BuildExecutionPlan();assert(ok==(version[0]!=L'3'));
  if(ok)assert(e.executionPlan_.rows[Index(Product::Platform64)].action==(version[0]==L'1'?Action::Update:Action::Repair));
  else assert(!e.executionPlanReady_);
 }
 {InstallEngine e;Reset(e);e.selection_.platform64=true;Installed(e,L"old64",L"1.0.0");assert(e.BuildExecutionPlan());assert(e.baselineChecks==1);}
 for(bool legacyNewer:{false,true}){
  InstallEngine e;Reset(e);e.selection_.platform64=true;
  Installed(e,L"p64",legacyNewer?L"1.0.0":L"3.0.0");
  Installed(e,L"old64",legacyNewer?L"3.0.0":L"1.0.0");
  assert(!e.BuildExecutionPlan());assert(!e.executionPlanReady_);
 }
 {InstallEngine e;Reset(e);e.selection_.platform64=true;assert(e.BuildExecutionPlan());
  GlassMaintenanceCache::media.clear();assert(!e.BuildExecutionPlan());assert(!e.executionPlanReady_);}
 {InstallEngine e;Reset(e);e.selection_.settings32=true;e.scanNativeX64_=false;assert(e.BuildExecutionPlan());
  assert(e.executionPlan_.rows[Index(Product::Settings32)].action==Action::Install);
  assert(e.executionPlan_.rows[Index(Product::Settings64)].action==Action::None);}
 {InstallEngine e;Reset(e);e.selection_.platform64=true;e.scanNativeX64_=false;assert(!e.BuildExecutionPlan());}
 {InstallEngine e;Reset(e);e.selection_.platform64=true;Installed(e,L"p64",L"2.0.0");
  GlassProductInspect::observation.x64.integrityKnown=false;assert(!e.BuildExecutionPlan());assert(e.failures==1);}
 {InstallEngine e;Reset(e);e.selection_.platform64=true;Installed(e,L"p64",L"2.0.0");
  GlassProductInspect::observation.x64.damaged=true;assert(e.BuildExecutionPlan());}
 {InstallEngine e;Reset(e);e.selection_.platform64=true;Installed(e,L"p64",L"2.0.0");e.rollbackSession_.oldProducts.clear();assert(!e.BuildExecutionPlan());}
 {InstallEngine e;Reset(e);e.selection_.platform64=true;GlassMaintenanceCache::media[L"work\\Glass-x64.msi"].first=L"s64";assert(!e.BuildExecutionPlan());}
 {InstallEngine e;Reset(e);e.selection_.platform64=true;GlassMaintenanceCache::media[L"work\\Glass-x64.msi"].second=L"2.invalid";assert(!e.BuildExecutionPlan());}
 {InstallEngine e;Reset(e);e.selection_.platform64=true;GlassMaintenanceCache::media.clear();assert(!e.BuildExecutionPlan());}
 {InstallEngine e;Reset(e);e.selection_.platform64=true;GlassInstallMsi::unsafe=L"p64";assert(!e.BuildExecutionPlan());}
 {InstallEngine e;Reset(e);e.selection_.platform64=true;e.badFamily=L"p64";assert(!e.BuildExecutionPlan());}
 {InstallEngine e;Reset(e);e.selection_.platform64=true;GlassInstallMsi::states[L"p64"]=INSTALLSTATE_ADVERTISED;assert(!e.BuildExecutionPlan());}
 {InstallEngine e;Reset(e);e.selection_.platform64=true;GlassInstallMsi::states[L"og"]=INSTALLSTATE_ADVERTISED;assert(e.BuildExecutionPlan());}
 {InstallEngine e;Reset(e);e.selection_.platform64=true;GlassInstallMsi::states[L"p64"]=INSTALLSTATE_DEFAULT;assert(!e.BuildExecutionPlan());}
 {InstallEngine e;Reset(e);e.cancelled=true;assert(!e.BuildExecutionPlan());assert(!e.executionPlanReady_&&e.failures==0);}
 {InstallEngine e;Reset(e);GlassProductInspect::observation.ran=false;assert(!e.BuildExecutionPlan());}
 {InstallEngine e;Reset(e);e.selection_.platform64=true;concurrent=true;assert(!e.BuildExecutionPlan());}
 {InstallEngine e;Reset(e);e.selection_.platform64=true;Installed(e,L"p64",L"2.0.0");e.badBaseline=L"p64";assert(!e.BuildExecutionPlan());}
 std::cout<<"PASS: actual Setup discovery/Build: install/update/repair, legacy, independent helpers, downgrade, unknown integrity, missing/wrong media, context/family/baseline/presence changes, cancellation\n";
}
'''.replace('// ADAPTER',adapter)
compiler=shutil.which('clang++') or shutil.which('g++')
if not compiler:raise SystemExit('clang++ or g++ required')
with tempfile.TemporaryDirectory(prefix='GlassSetupExecutionPlan-') as directory:
    path=Path(directory)/'plan.cpp';exe=Path(directory)/'plan'
    path.write_text(harness)
    subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror','-pedantic','-I'+str(root/'GlassSetup/src'),str(path),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
