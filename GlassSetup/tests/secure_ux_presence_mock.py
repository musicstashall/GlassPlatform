"""Actual native inventory + inspector presence projection, fake read-only MSI APIs.
No Windows registration, native helper, filesystem or MSI transaction is tested.
"""
from pathlib import Path
import re, runpy, shutil, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
base=runpy.run_path(str(root/'GlassSetup/tests/secure_ux_inventory_mock.py'))['harness']
base=base[:base.index('int main(){')]
source=(root/'GlassSetup/src/InstallSupport/GlassProductInspect.cpp').read_text()
projection=source[source.index('    struct ProductInputs'):source.index('    // Probe physical backends BEFORE')]
header=(root/'GlassSetup/src/InstallSupport/GlassInstallMsi.h').read_text()
codes='\n'.join(re.findall(r'inline constexpr wchar_t k(?:Product|Legacy)\w+\[\]\s*=\s*L"[^"\n]+";',header))
assert codes
# The UI and both mutation boundaries must consume the distinct projections.
main=(root/'GlassSetup/src/main.cpp').read_text()
assert 'JsStringLiteral(inspect_.secureUxThemePresenceDetail)' in main
provider=(root/'GlassSetup/src/InstallSupport/GlassSecureUxPackage.cpp').read_text()
prepare=provider[provider.index('bool Prepare(const std::wstring& privateDirectory'):provider.index('namespace {\nbool RemovalInventory')]
assert prepare.index('QueryInstallPresence(installed,cancel,error)') < prepare.index('package.alreadyInstalled=true')
engine=(root/'GlassSetup/src/InstallEngine.cpp').read_text()
install=engine[engine.index('bool InstallEngine::InstallOriginalSecureUxTheme()'):engine.index('bool InstallEngine::BuildExecutionPlan()')]
assert install.index('QueryInstallPresence(officialProducts,cancelRequested_,presenceError)') < install.index('if (!officialProducts.empty())') < install.index('GlassInstallMsi::InstallProduct(')
harness=base+r'''
namespace GlassInstallMsi {
// CODES
bool IsProductInstalled(const wchar_t* code){return MsiQueryProductStateW(code)==INSTALLSTATE_DEFAULT;}
std::map<std::wstring,std::wstring> versions;
std::wstring QueryDisplayVersion(const wchar_t* code){auto it=versions.find(code);return it==versions.end()?L"4.0.0":it->second;}
}
namespace GlassProductInspect {
struct ProductReport {bool installed=false;std::wstring installedVersion,installedPackageVersion;};
struct Report {ProductReport x64,x86,settings64,settings32,settings,og,secureUxTheme;
 std::wstring officialSecureUxThemeCode,secureUxThemePresenceDetail;};
}
namespace GlassInstallLog {std::wstring last;void LineFormat(const wchar_t*,const wchar_t* details){last=details;}}
GlassProductInspect::Report InspectPresence(){using namespace GlassProductInspect;Report report;
// PROJECTION
return report;}
int main(){
 const wchar_t* family=L"{965B5B74-4040-4E89-947F-AF8D99A76706}";
 assert(!Shared::SecureUxThemeInventory::IsInstalled());
 auto r=InspectPresence();assert(!r.secureUxTheme.installed&&r.officialSecureUxThemeCode.empty());
 // A stale ABSENT family candidate was precisely what the old projection
 // (any nonempty conservative Query) incorrectly promoted to Installed.
 families[family]={L"candidate"};states[L"candidate"]=INSTALLSTATE_ABSENT;
 std::vector<std::wstring> guards;assert(Shared::SecureUxThemeInventory::Query(guards)==0&&!guards.empty());
 assert(!Shared::SecureUxThemeInventory::IsInstalled());r=InspectPresence();
 assert(!r.secureUxTheme.installed&&r.officialSecureUxThemeCode.empty());
 assert(r.secureUxThemePresenceDetail.find(L"candidate state=2")!=std::wstring::npos);
 assert(GlassInstallLog::last==r.secureUxThemePresenceDetail);
 states[L"candidate"]=INSTALLSTATE_DEFAULT;r=InspectPresence();
 assert(r.secureUxTheme.installed&&r.officialSecureUxThemeCode==L"candidate");
 assert(Shared::SecureUxThemeInventory::IsInstalled());
 families.clear();states.clear();states[GlassInstallMsi::kProductSecureUxThemeX64]=INSTALLSTATE_DEFAULT;
 r=InspectPresence();assert(r.secureUxTheme.installed&&r.officialSecureUxThemeCode.empty());
 families[family]={L"candidate"};states[L"candidate"]=INSTALLSTATE_DEFAULT;
 r=InspectPresence();assert(r.secureUxTheme.installed&&r.officialSecureUxThemeCode.empty());
 families.clear();states.clear();enumerationError=ERROR_ACCESS_DENIED;
 r=InspectPresence();assert(!r.secureUxTheme.installed&&r.officialSecureUxThemeCode.empty());
 assert(r.secureUxThemePresenceDetail.find(L"rc=5")!=std::wstring::npos);
 // Distinct MSI registrations must remain distinct; no bundle/PE substitution.
 families.clear();states.clear();enumerationError=0;
 const wchar_t* codes[]={GlassInstallMsi::kProductPlatformX64,GlassInstallMsi::kProductPlatformX86,
  GlassInstallMsi::kProductSettingsX64,GlassInstallMsi::kProductSettingsX86,GlassInstallMsi::kProductOpenGlass};
 for(unsigned i=0;i<5;++i){states[codes[i]]=INSTALLSTATE_DEFAULT;GlassInstallMsi::versions[codes[i]]=std::to_wstring(i+1)+L".2.3";}
 r=InspectPresence();unsigned n=0;
 for(auto* product:{&r.x64,&r.x86,&r.settings64,&r.settings32,&r.og}){
  assert(product->installedVersion==std::to_wstring(++n)+L".2.3");
  assert(product->installedPackageVersion==product->installedVersion);
 }
 std::cout<<"PASS: actual native presence projection: none, ABSENT not installed, DEFAULT installed, repack, mixed, failed inventory; diagnostic evidence\n";
}
'''.replace('// CODES',codes).replace('// PROJECTION',projection)
with tempfile.TemporaryDirectory(prefix='GlassPresence-') as d:
 path=Path(d)/'test.cpp';path.write_text(harness);exe=Path(d)/'test'
 subprocess.run([shutil.which('clang++') or shutil.which('g++'),'-std=c++17','-Wall','-Wextra',str(path),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
