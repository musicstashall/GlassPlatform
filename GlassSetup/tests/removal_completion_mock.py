"""Real maintenance-finalization prefix with fake cache/ARP APIs; no Windows I/O.
Also checks the shared launch policy, including retained hosts after helper removal.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'GlassSetup/src/InstallEngine.cpp').read_text()
a = source.index('void InstallEngine::LaunchSelectedProducts() noexcept')
b = source.index('    // Policy: a selected platform', a)
prefix = source[a:b] + '}\n'
# Cancellation still closes under the old commit gate, not the UI-only predicate.
header = (root / 'GlassSetup/src/InstallEngine.h').read_text()
assert '&& !launchingPhase_.load(std::memory_order_acquire)' in header
assert 'for (;self->HasHostLaunchWork();)' in source
assert source.index('self->hostLaunchExpected_.store(GlassProducts::NeedsHostLaunch') < source.index('self->launchingPhase_.store(true')

harness = r'''
#include "GlassProducts.h"
#include <cassert>
#include <iterator>
#include <string>
#include <vector>
constexpr unsigned INVALID_FILE_ATTRIBUTES=~0u;
constexpr unsigned LANG_RUSSIAN=25,IDR_MAINTENANCE_UNINS=1;
#define PRIMARYLANGID(x) (x)
unsigned language=0;
unsigned GetUserDefaultUILanguage(){return language;}
int probes=0,publishes=0,arps=0;bool publishOk=true,arpOk=true;
bool coverageGap=false;
std::vector<std::wstring> published,warningTitles;
unsigned GetFileAttributesW(const wchar_t*){++probes;return INVALID_FILE_ATTRIBUTES;}
namespace InstallLog {void Line(const wchar_t*){}}
namespace GlassMaintenanceCache {
struct Blob {unsigned id;const wchar_t* name;const wchar_t* path;bool required;};
std::wstring CurrentFile(const wchar_t* name){return std::wstring(L"cache/")+name;}
bool Publish(int,const Blob* blobs,std::size_t count,unsigned,std::wstring*,std::vector<std::wstring>* missing){
 if(coverageGap)missing->push_back(L"cache/Glass-x64.msi");
 ++publishes;for(std::size_t i=0;i<count;++i)published.emplace_back(blobs[i].name);return publishOk;
}
}
namespace GlassInstallMsi {
bool RefreshManagedArp(const std::wstring&,const std::wstring&,const std::wstring&){++arps;return arpOk;}
}
struct InstallEngine {
 GlassProducts::Selection selection_;
 bool mixedOpenGlass_=false;struct {std::wstring workspaceDir;} officialResourcesRollback_;bool scanNativeX64_=true;int instance_=0,warnings=0;
 std::wstring tempDir_=L"temp",x64Dir_,x86Dir_,openGlassDir_;
 void AddFinishWarning(const std::wstring& title,const std::wstring&){++warnings;warningTitles.push_back(title);}
 void LaunchSelectedProducts() noexcept;
};
// REAL_PREFIX
GlassProducts::Selection Removal(unsigned mask){
 GlassProducts::Selection s;s.platform64=s.platform32=s.glassSettings=s.openGlass=false;
 s.independentSettings=true;s.removeMask=mask;return s;
}
void Reset(){language=0;warningTitles.clear();coverageGap=false;probes=publishes=arps=0;publishOk=arpOk=true;published.clear();}
int main(){
 using namespace GlassProducts;
 const OpenGlassStartupState existing{true,true,false},fresh{true,false,false};
 // Full remove must neither rotate/delete the executing cached controller nor
 // show a launch page, even if every removed host had been running.
 for(unsigned mask:{1u,4u,32u,8u,Selection::kManagedRemovalMask}){
  Reset();InstallEngine e;e.selection_=Removal(mask);e.LaunchSelectedProducts();
  assert(probes==0&&publishes==0&&arps==1&&e.warnings==0);
 }
 const auto all=Removal(Selection::kManagedRemovalMask);
 assert(!HasPackageApply(all));assert(!NeedsHostLaunch(all,existing));
 auto helper=Removal(0);helper.settings64=true;
 assert(!NeedsHostLaunch(helper,existing)); // Settings Apply has no launch phase.
 helper.settings32=true;assert(!NeedsHostLaunch(helper,existing));
 assert(!NeedsHostLaunch(Removal(4),existing));
 assert(!NeedsHostLaunch(Removal(1),existing));
 for(int row=0;row<6;++row){
  Reset();InstallEngine e;e.selection_=Removal(0);
  switch(row){case 0:e.selection_.platform64=true;break;case 1:e.selection_.platform32=true;break;
   case 2:e.selection_.settings64=true;break;case 3:e.selection_.settings32=true;break;
   case 4:e.selection_.openGlass=true;break;case 5:e.selection_.secureUxTheme=true;break;}
  assert(HasPackageApply(e.selection_));e.LaunchSelectedProducts();assert(publishes==1&&arps==1&&e.warnings==0);
 }
 for(unsigned lang:{0u,25u}){Reset();language=lang;coverageGap=true;InstallEngine e;e.selection_=Removal(0);e.selection_.settings64=true;
  e.LaunchSelectedProducts();assert(publishes==1&&arps==1&&e.warnings==1);
  assert(warningTitles[0]==(lang==25?L"Не для всех продуктов доступен источник восстановления":L"Repair sources are unavailable for some products"));}
 // Genuine publication/ARP failures remain visible; do not mask AccessDenied.
 {Reset();publishOk=false;InstallEngine e;e.selection_=Removal(0);e.selection_.settings64=true;
  e.LaunchSelectedProducts();assert(publishes==1&&arps==1&&e.warnings==1);}
 {Reset();arpOk=false;InstallEngine e;e.selection_=all;e.LaunchSelectedProducts();
  assert(publishes==0&&arps==1&&e.warnings==1);}
 // Mixed apply/remove keeps the existing source filtering and cache publication.
 {Reset();InstallEngine e;e.selection_=Removal(1);e.selection_.settings32=true;e.LaunchSelectedProducts();
  assert(publishes==1&&published.size()==5);for(const auto& name:published)assert(name!=L"Glass-x64.msi");}
 auto platform=Removal(0);platform.platform64=true;assert(NeedsHostLaunch(platform,existing));
 auto og=Removal(0);og.openGlass=true;assert(!NeedsHostLaunch(og,existing));
 assert(NeedsHostLaunch(og,fresh));
}
'''.replace('// REAL_PREFIX', prefix)
with tempfile.TemporaryDirectory(prefix='GlassRemovalCompletion-') as directory:
    cpp = Path(directory) / 'test.cpp'
    exe = Path(directory) / 'test'
    cpp.write_text(harness)
    subprocess.run([shutil.which('clang++') or shutil.which('g++'), '-std=c++17',
                    '-Wall', '-Wextra', '-Werror', '-pedantic',
                    '-I' + str(root / 'GlassSetup/src'), str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('PASS: actual finalization prefix: removal skips cache publication, retains ARP; apply errors preserved; launch page belongs only to selected startup products')
