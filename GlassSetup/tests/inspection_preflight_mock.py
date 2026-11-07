"""Exercise the actual inspector source/integrity loop with fake read-only APIs.
No real network, Windows Installer, signature, file integrity or registry access.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'GlassSetup/src/InstallSupport/GlassProductInspect.cpp').read_text()
loop=source[source.index('    std::atomic_bool neverCancel{false};'):source.index('    report.settings=report.settings64.installed?report.settings64:report.settings32;',source.index('    std::atomic_bool neverCancel{false};'))]
harness=r'''
#include <atomic>
#include <cassert>
#include <iostream>
#include <set>
#include <string>
#include <vector>
using MSIHANDLE=unsigned;
constexpr unsigned ERROR_SUCCESS=0,MSIDBOPEN_READONLY=0;
int fetches=0,workspaces=0,prepares=0,cleanups=0;
bool baseline=true,context=true;
std::set<std::wstring> installed;
std::vector<std::wstring> checked;
struct ProductReport {bool installed=false,integrityKnown=false,sourceVerified=false,updateAvailable=false;
 std::wstring targetVersion,installedVersion,installedPackageVersion;int unavailable=0;};
struct Report {ProductReport x64,x86,settings64,settings32,og;};
struct Inputs {bool inspectSources=true;const std::atomic_bool* cancelled=nullptr;};
enum class Reason {InspectionUnavailable};
void AddReason(ProductReport& r,Reason){++r.unavailable;}
namespace GlassPackageCatalog {
enum class Product {Platform64,Platform32,Settings64,Settings32,OpenGlass};
struct Catalog {};
const char* kFiles[]={"Glass-x64.msi","Glass-x86.msi","GlassSettings-x64.msi","GlassSettings-x86.msi","OpenGlass-x64.msi"};
}
namespace GlassPackageAcquisition {
bool FetchCatalog(const std::atomic_bool&,GlassPackageCatalog::Catalog&,std::wstring&){++fetches;return true;}
bool CreateWorkspace(std::wstring& out){++workspaces;out=L"workspace";return true;}
bool Prepare(GlassPackageCatalog::Product,const GlassPackageCatalog::Catalog&,const std::wstring&,const std::atomic_bool&,std::wstring&){++prepares;return false;}
}
namespace GlassInstallMsi {
constexpr const wchar_t* kProductPlatformX64=L"p64",*kLegacyPlatformX64=L"old64",*kProductPlatformX86=L"p32",
 *kLegacyPlatformX86=L"old32",*kProductSettingsX64=L"s64",*kProductSettingsX86=L"s32",*kProductOpenGlass=L"og",*kLegacyOpenGlass=L"oldog";
bool IsProductInstalled(const wchar_t* code){return installed.count(code)!=0;}
enum class ProductPathCheck {Safe,Unsafe};
ProductPathCheck CheckProductContexts(const wchar_t*,std::wstring&){return context?ProductPathCheck::Safe:ProductPathCheck::Unsafe;}
}
bool DeleteFileW(const wchar_t*){++cleanups;return true;}
bool RemoveDirectoryW(const wchar_t*){++cleanups;return true;}
unsigned MsiOpenDatabaseW(const wchar_t*,unsigned,MSIHANDLE*){return 5;}
void MsiCloseHandle(MSIHANDLE){}
std::wstring QueryMsiProperty(MSIHANDLE,const wchar_t*){return L"2.0.0";}
struct QuadVersion {static QuadVersion Parse(const std::wstring&){return {};}
 int Compare(const QuadVersion&)const{return 0;}};
std::wstring CachedMsiPath(const wchar_t* code){return installed.count(code)?std::wstring(code)+L".msi":L"";}
bool InstalledBaselineMatches(const std::wstring& path,const wchar_t* code){assert(path==std::wstring(code)+L".msi");return baseline;}
bool CheckProductFiles(const std::wstring& path,const wchar_t* code,const std::wstring&,ProductReport&,bool){
 assert(path==std::wstring(code)+L".msi");checked.emplace_back(code);return true;
}
void Inspect(const Inputs& in,Report& report){const std::wstring signerSha1=L"signer";
// LOOP
}
void Reset(){installed.clear();checked.clear();fetches=workspaces=prepares=cleanups=0;baseline=context=true;}
int main(){
 for(const auto* code:{L"p64",L"old64"}){
  Reset();installed.insert(code);Report r;r.x64.installed=true;Inputs in;in.inspectSources=false;Inspect(in,r);
  assert(r.x64.integrityKnown&&checked==std::vector<std::wstring>{code});
  assert(fetches==0&&workspaces==0&&prepares==0&&cleanups==0);
 }
 for(const auto* code:{L"old32",L"oldog"}){
  Reset();installed.insert(code);Report r;auto& product=std::wstring(code)==L"old32"?r.x86:r.og;
  product.installed=true;Inputs in;in.inspectSources=false;Inspect(in,r);
  assert(product.integrityKnown&&checked==std::vector<std::wstring>{code});
 }
 {Reset();installed={L"p64",L"old64"};Report r;r.x64.installed=true;Inputs in;in.inspectSources=false;Inspect(in,r);
  assert(checked==std::vector<std::wstring>{L"p64"});}
 {Reset();installed={L"s32"};Report r;r.settings32.installed=true;Inputs in;Inspect(in,r);
  assert(fetches==1&&workspaces==1&&prepares==1&&cleanups==11&&r.settings32.integrityKnown);}
 for(bool unsafe:{false,true}){
  Reset();installed={L"p64"};baseline=unsafe;context=!unsafe;Report r;r.x64.installed=true;Inputs in;in.inspectSources=false;Inspect(in,r);
  assert(!r.x64.integrityKnown&&r.x64.unavailable==1&&checked.empty());
 }
 {Reset();Report r;r.x64.installed=true;Inputs in;in.inspectSources=false;Inspect(in,r);
  assert(!r.x64.integrityKnown&&r.x64.unavailable==1);}
 {Reset();installed={L"p64"};Report r;r.x64.installed=true;Inputs in;in.inspectSources=false;
  std::atomic_bool cancel{true};in.cancelled=&cancel;Inspect(in,r);assert(checked.empty()&&fetches==0);}
 std::cout<<"PASS: actual inspector loop: offline preflight, modern/legacy installed baselines, default source discovery, missing/untrusted baseline, context and cancellation\n";
}
'''.replace('// LOOP',loop)
compiler=shutil.which('clang++') or shutil.which('g++')
if not compiler:raise SystemExit('clang++ or g++ required')
with tempfile.TemporaryDirectory(prefix='GlassInspectionPreflight-') as directory:
    path=Path(directory)/'inspect.cpp';exe=Path(directory)/'inspect'
    path.write_text(harness)
    subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror','-pedantic',str(path),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
