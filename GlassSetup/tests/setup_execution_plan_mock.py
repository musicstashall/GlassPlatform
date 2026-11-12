"""Compile the ACTUAL Setup discovery/Build adapter with fake read-only APIs.
No MSI transactions, processes, registry, network, or Windows security exercised.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'GlassSetup/src/InstallEngine.cpp').read_text()
adapter=source[source.index('bool InstallEngine::BuildExecutionPlan() noexcept {'):source.index('bool InstallEngine::PrepareFullRemovalCleanup() noexcept {')]
util=(root/'GlassSetup/src/InstallSupport/GlassInstallUtil.cpp').read_text()
a=util.index('bool GlassSettingsAbsentForSecureUxRemoval()');b=util.index('bool PrepareSecureUxRemovalVisualStyle(',a)
absence=util[a:b]
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
constexpr unsigned LANG_RUSSIAN=25,ERROR_INSTALL_PACKAGE_REJECTED=1625,INVALID_FILE_ATTRIBUTES=0xffffffff,FILE_ATTRIBUTE_DIRECTORY=16,FILE_ATTRIBUTE_REPARSE_POINT=1024;
constexpr int INSTALLSTATE_DEFAULT=5,INSTALLSTATE_UNKNOWN=-1,INSTALLSTATE_ADVERTISED=1;
unsigned GetUserDefaultUILanguage(){return 0;}
bool upstreamKnown=true,upstreamInstalled=true,pathSafe=true;unsigned dllAttributes=0;
unsigned GetFileAttributesW(const wchar_t*){return dllAttributes;}
namespace GlassOpenGlassInstall {
struct Record {bool installed=false;std::wstring directory=L"upstream";};
bool Query(Record& r,std::wstring&){r.installed=upstreamInstalled;return upstreamKnown;}
}
namespace GlassOpenGlassFiles {
bool HasReceipt(const std::wstring&){return false;}
struct State {bool present=false,integrity=false;std::wstring version;};
State Inspect(const std::wstring&){return {};}
}
namespace GlassOpenGlassRelease {bool PayloadUpdateAvailable(const std::wstring& a,const std::wstring& b){return a<b;}}

namespace InstallLog {template<class... Args>void LineFormat(const wchar_t*,Args...) {}}
namespace GlassProductInspect {
struct ProductReport {bool installed=false,integrityKnown=false,damaged=false,residual=false,residualSafe=false;};
struct Report {bool ran=true;ProductReport x64,x86,settings64,settings32,og,secureUxTheme,openGlassResources;std::wstring officialSecureUxThemeCode;};
struct Inputs {bool os64=false,inspectSources=true;const std::atomic_bool* cancelled=nullptr;};
Report observation;
Report Run(const Inputs& input){assert(!input.inspectSources);return observation;}
}
namespace GlassInstallMsi {
constexpr const wchar_t* kProductPlatformX64=L"p64",*kLegacyPlatformX64=L"old64",
 *kProductPlatformX86=L"p32",*kLegacyPlatformX86=L"old32",*kProductSettingsX64=L"s64",
 *kProductOpenGlassResourcesX64=L"res",*kProductSettingsX86=L"s32",*kProductOpenGlass=L"og",*kLegacyOpenGlass=L"oldog";
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
namespace GlassInstallUtil {
bool IsSafeInstallPath(const std::wstring&){return pathSafe;}
// ACTUAL ABSENCE

 enum class SecureUxThemeState {Conflict,Inactive,Active,ActivationPending,DeactivationPending};
 SecureUxThemeState state=SecureUxThemeState::Inactive;bool query=true;
 bool QuerySecureUxThemeState(const std::wstring&,const std::wstring&,SecureUxThemeState* out,std::wstring*){*out=state;return query;}
}
namespace GlassSecureUxPackage {
 struct Package {std::wstring path,productCode,version;bool alreadyInstalled=false;};
 struct Removal {bool ready=false;};
 bool presenceSafe=true,legacyAbsent=true,recheck=true;std::vector<std::wstring> installed;
 bool QueryInstallPresence(std::vector<std::wstring>& out,const std::atomic_bool&,std::wstring&){out=installed;return presenceSafe;}
 bool CheckLegacyRepackAbsent(std::wstring&){return legacyAbsent;}
 bool RecheckRemoval(const std::wstring&,const std::atomic_bool&,const Removal& r,std::wstring&){return recheck&&r.ready;}
}
struct Old {std::wstring productCode,cachedMsiPath;};
struct InstallEngine {
 GlassProductPlan::Plan executionPlan_{};bool executionPlanReady_=true,openGlassResidualRemoval_=false,scanNativeX64_=true,cancelled=false,forceClean_=false,preparingOfficialResources_=false;
 std::atomic_bool cancelRequested_{false};GlassProducts::Selection selection_{};
 GlassSecureUxPackage::Package secureUxPackage_;
 GlassSecureUxPackage::Removal secureUxRemoval_;
 bool secureUxLoadedRemoval_=false;struct {bool RecheckLoadedRemoval(std::wstring&)const{return true;}}secureUxRemovalProfiles_;
 std::wstring secureUxRemovalCode_=L"vendor",x64Dir_=L"x64",x86Dir_=L"x86";
 std::wstring tempDir_=L"work",badBaseline,badFamily,openGlassDir_;
 struct {std::wstring dll=L"dll",host=L"host",gui=L"gui",version=L"3.0.0";}openGlassPackage_;
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
 GlassSecureUxPackage::presenceSafe=GlassSecureUxPackage::legacyAbsent=GlassSecureUxPackage::recheck=true;
 GlassSecureUxPackage::installed.clear();GlassInstallUtil::query=true;
 GlassInstallUtil::state=GlassInstallUtil::SecureUxThemeState::Inactive;
 stateReads=0;concurrent=false;upstreamKnown=upstreamInstalled=pathSafe=true;dllAttributes=0;
}
void Installed(InstallEngine& e,const wchar_t* code,const wchar_t* version){
 GlassInstallMsi::states[code]=INSTALLSTATE_DEFAULT;
 const auto path=std::wstring(L"baseline-")+code;
 e.rollbackSession_.oldProducts.push_back({code,path});
 GlassMaintenanceCache::media[path]={code,version};
 auto& r=GlassProductInspect::observation.x64;r.installed=true;r.integrityKnown=true;
}
int main(){
 for(int left:{INSTALLSTATE_UNKNOWN,INSTALLSTATE_DEFAULT,INSTALLSTATE_ADVERTISED})for(int right:{INSTALLSTATE_UNKNOWN,INSTALLSTATE_DEFAULT,INSTALLSTATE_ADVERTISED}) {
  GlassInstallMsi::states[L"s64"]=left;GlassInstallMsi::states[L"s32"]=right;
  assert(GlassInstallUtil::GlassSettingsAbsentForSecureUxRemoval()==(left==INSTALLSTATE_UNKNOWN&&right==INSTALLSTATE_UNKNOWN));
 }

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
 // Actual adapter: selected vendor install is in the plan, never an implicit repair.
 for(int n=0;n<10;++n){
  InstallEngine e;Reset(e);e.selection_.secureUxTheme=true;
  e.secureUxPackage_={L"vendor.msi",L"vendor",L"4.0.0",false};
  GlassMaintenanceCache::media[L"vendor.msi"]={L"vendor",L"4.0.0"};
  if(n==1)GlassSecureUxPackage::presenceSafe=false;
  if(n==2)GlassSecureUxPackage::legacyAbsent=false;
  if(n==3)GlassSecureUxPackage::installed={L"vendor"};
  if(n==4)GlassProductInspect::observation.secureUxTheme.installed=true;
  if(n==5)e.secureUxPackage_.alreadyInstalled=true;
  if(n==6)GlassMaintenanceCache::media.erase(L"vendor.msi");
  if(n==7)GlassMaintenanceCache::media[L"vendor.msi"].first=L"foreign";
  if(n==8)GlassMaintenanceCache::media[L"vendor.msi"].second=L"4.1.0";
  if(n==9)e.selection_.secureUxThemeRemove=true;
  assert(e.BuildExecutionPlan()==(n==0));assert(e.executionPlanReady_==(n==0));
  if(n==0)assert(e.executionPlan_.rows[Index(Product::SecureUxTheme)].action==Action::Install&&e.executionPlan_.actionCount==1);
 }
 {InstallEngine e;Reset(e);e.selection_.secureUxThemeRemove=true;e.secureUxRemoval_.ready=true;
  auto& r=GlassProductInspect::observation;r.secureUxTheme.installed=true;r.officialSecureUxThemeCode=L"vendor";
  GlassInstallUtil::state=GlassInstallUtil::SecureUxThemeState::ActivationPending;
  assert(e.BuildExecutionPlan());assert(e.executionPlan_.rows[Index(Product::SecureUxTheme)].action==Action::Remove);
 }
 {InstallEngine e;Reset(e);e.selection_.secureUxThemeRemove=true;e.secureUxRemoval_.ready=true;e.secureUxLoadedRemoval_=true;
  auto& r=GlassProductInspect::observation;r.secureUxTheme.installed=true;r.officialSecureUxThemeCode=L"vendor";
  GlassInstallUtil::state=GlassInstallUtil::SecureUxThemeState::Active;
  assert(e.BuildExecutionPlan());assert(e.executionPlan_.rows[Index(Product::SecureUxTheme)].action==Action::Remove);
 }
 // Manual exception still requires exact original MSI and absent Settings.
 for(bool absent:{false,true}) {InstallEngine e;Reset(e);e.selection_.secureUxThemeRemove=true;e.selection_.secureUxRemovalWithoutSettings=true;e.secureUxRemoval_.ready=true;
  auto& r=GlassProductInspect::observation;r.secureUxTheme.installed=true;r.officialSecureUxThemeCode=L"vendor";
  GlassInstallUtil::query=false;if(!absent)GlassInstallMsi::states[L"s64"]=INSTALLSTATE_DEFAULT;
  assert(e.BuildExecutionPlan()==absent);
 }
 // Exact original MSI + baseline + unloaded hook are required.
 for(int n=0;n<9;++n){
  InstallEngine e;Reset(e);e.selection_.secureUxThemeRemove=true;e.secureUxRemoval_.ready=true;
  auto& r=GlassProductInspect::observation;r.secureUxTheme.installed=true;r.officialSecureUxThemeCode=L"vendor";
  if(n==1)r.secureUxTheme.installed=false;
  if(n==2)r.officialSecureUxThemeCode.clear();
  if(n==3)e.secureUxRemovalCode_=L"other";
  if(n==4)e.secureUxRemoval_.ready=false;
  if(n==5)GlassInstallUtil::query=false;
  if(n==6)GlassInstallUtil::state=GlassInstallUtil::SecureUxThemeState::Active;
  if(n==7)GlassSecureUxPackage::recheck=false;
  if(n==8)e.selection_.secureUxTheme=true;
  assert(e.BuildExecutionPlan()==(n==0));assert(e.executionPlanReady_==(n==0));
  if(n==0)assert(e.executionPlan_.rows[Index(Product::SecureUxTheme)].action==Action::Remove&&e.executionPlan_.actionCount==1);
 }
 {InstallEngine e;Reset(e);GlassProductInspect::observation.secureUxTheme.installed=true;
  assert(e.BuildExecutionPlan());assert(e.executionPlan_.rows[Index(Product::SecureUxTheme)].action==Action::Keep);}

 // Planned OG satisfies only the private PREPARATION phase, never post-commit revalidation.
 {InstallEngine e;Reset(e);e.selection_.openGlassResources=true;e.preparingOfficialResources_=true;upstreamKnown=false;
  GlassMaintenanceCache::media[L"work\\OpenGlassResources-x64.msi"]={L"res",L"2.0.0"};
  assert(e.BuildExecutionPlan());e.preparingOfficialResources_=false;assert(!e.BuildExecutionPlan());
  e.preparingOfficialResources_=true;e.openGlassPackage_.version.clear();assert(!e.BuildExecutionPlan());}
 // Resources uses its own prepared MSI and the existing official prerequisite.
 for(unsigned failure=0;failure<9;++failure) {
  InstallEngine e;Reset(e);e.selection_.openGlassResources=true;
  GlassMaintenanceCache::media[L"work\\OpenGlassResources-x64.msi"]={L"res",L"2.0.0"};
  if(failure==1)upstreamKnown=false;
  if(failure==2)upstreamInstalled=false;
  if(failure==3)pathSafe=false;
  if(failure==4)dllAttributes=INVALID_FILE_ATTRIBUTES;
  if(failure==5)dllAttributes=FILE_ATTRIBUTE_REPARSE_POINT;
  if(failure==6)e.scanNativeX64_=false;
  if(failure==7)GlassMaintenanceCache::media.clear();
  if(failure==8)GlassMaintenanceCache::media[L"work\\OpenGlassResources-x64.msi"].first=L"og";
  assert(e.BuildExecutionPlan()==(failure==0));
  if(failure==0)assert(e.executionPlan_.rows[6].action==Action::Install&&e.executionPlan_.rows[3].action==Action::Keep);
 }
 {InstallEngine e;Reset(e);e.selection_.removeMask=64;Installed(e,L"res",L"1.0.0");
  GlassProductInspect::observation.openGlassResources.installed=true;upstreamKnown=false;
  GlassMaintenanceCache::media.erase(L"work\\OpenGlassResources-x64.msi");
  assert(e.BuildExecutionPlan()&&e.executionPlan_.rows[6].action==Action::Remove);}
 for(const auto version:{L"1.0.0",L"2.0.0",L"3.0.0"}) {
  InstallEngine e;Reset(e);e.selection_.openGlassResources=true;Installed(e,L"res",version);
  auto& graphics=GlassProductInspect::observation.openGlassResources;graphics.installed=graphics.integrityKnown=true;
  GlassMaintenanceCache::media[L"work\\OpenGlassResources-x64.msi"]={L"res",L"2.0.0"};
  assert(e.BuildExecutionPlan()==(version[0]!=L'3'));
  if(e.executionPlanReady_)assert(e.executionPlan_.rows[6].action==(version[0]==L'1'?Action::Update:Action::Repair));
 }
 // Managed removal needs no adjacent/release media, but retains exact installed authority.
 for(auto p:{Product::Platform64,Product::Platform32,Product::Settings64,Product::Settings32,Product::OpenGlass}){
  InstallEngine e;Reset(e);const auto row=Index(p);e.selection_.removeMask=1u<<row;
  const wchar_t* code=row==0?L"p64":row==1?L"p32":row==2?L"s64":row==5?L"s32":L"og";
  Installed(e,code,L"1.0.0");
  auto& report=GlassProductInspect::observation;
  (row==0?report.x64:row==1?report.x86:row==2?report.settings64:row==5?report.settings32:report.og).installed=true;
  GlassMaintenanceCache::media.clear();assert(e.BuildExecutionPlan());assert(e.executionPlan_.rows[row].action==Action::Remove);
 }
 {InstallEngine e;Reset(e);e.selection_.removeMask=1;e.selection_.platform64=true;assert(!e.BuildExecutionPlan());}
 {InstallEngine e;Reset(e);e.selection_.removeMask=16;assert(!e.BuildExecutionPlan());}
 {InstallEngine e;Reset(e);e.selection_.removeMask=1;e.selection_.secureUxTheme=true;assert(!e.BuildExecutionPlan());}
 {InstallEngine e;Reset(e);e.selection_.removeMask=4;Installed(e,L"s64",L"1.0.0");Installed(e,L"p64",L"1.0.0");
  GlassProductInspect::observation.settings64.installed=true;GlassProductInspect::observation.x64.installed=true;
  assert(!e.BuildExecutionPlan());e.selection_.confirmedSettingsRemovalRisk=2;assert(!e.BuildExecutionPlan());
  e.selection_.confirmedSettingsRemovalRisk=1;assert(e.BuildExecutionPlan());}
 // A residual authorizes auxiliary-only Remove, never a fabricated MSI baseline.
 for(bool safe:{false,true}){InstallEngine e;Reset(e);e.selection_.removeMask=8;
  auto& og=GlassProductInspect::observation.og;og.residual=true;og.residualSafe=safe;
  GlassMaintenanceCache::media.clear();assert(e.BuildExecutionPlan()==safe);
  assert(e.openGlassResidualRemoval_==safe);if(safe)assert(e.executionPlan_.rows[Index(Product::OpenGlass)].action==Action::Remove);
  assert(e.rollbackSession_.oldProducts.empty());}
 {InstallEngine e;Reset(e);e.selection_.openGlass=true;
  auto& og=GlassProductInspect::observation.og;og.residual=true;og.residualSafe=true;og.damaged=true;
  assert(e.BuildExecutionPlan());assert(e.executionPlan_.rows[Index(Product::OpenGlass)].action==Action::Install);
  assert(!e.openGlassResidualRemoval_);}
 {InstallEngine e;Reset(e);e.selection_.removeMask=8;auto& og=GlassProductInspect::observation.og;
  og.residual=og.residualSafe=true;assert(e.BuildExecutionPlan());og.residualSafe=false;
  assert(!e.BuildExecutionPlan());assert(!e.openGlassResidualRemoval_);}
 {InstallEngine e;Reset(e);e.selection_.removeMask=8;auto& og=GlassProductInspect::observation.og;
  og.residual=og.residualSafe=true;GlassInstallMsi::unsafe=L"og";assert(!e.BuildExecutionPlan());}
 std::cout<<"PASS: actual Setup discovery/Build: install/update/repair, legacy, independent helpers, downgrade, unknown integrity, missing/wrong media, context/family/baseline/presence changes, cancellation; vendor install/remove/keep and 19 authority/revalidation cases\n";
}
'''.replace('// ADAPTER',adapter).replace('// ACTUAL ABSENCE',absence)
compiler=shutil.which('clang++') or shutil.which('g++')
if not compiler:raise SystemExit('clang++ or g++ required')
with tempfile.TemporaryDirectory(prefix='GlassSetupExecutionPlan-') as directory:
    path=Path(directory)/'plan.cpp';exe=Path(directory)/'plan'
    path.write_text(harness)
    subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror','-pedantic','-I'+str(root/'GlassSetup/src'),str(path),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
