"""Independent registry-cleanup consent and production Platform policy."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
main=(root/'GlassSetup/src/main.cpp').read_text()
engine=(root/'GlassSetup/src/InstallEngine.cpp').read_text()
profile=(root/'GlassSetup/src/InstallSupport/GlassProfileMaintenance.h').read_text()
assert main.index('if(selection_.Removes(3)&&!ogCleanupReviewed_)') < main.index('if(!GlassProducts::PlatformCleanupSelected')
assert 'reviewingOgCleanup_?selection_.cleanupOpenGlassConfig:selection_.cleanupUserData' in main
assert 'RunUninstaller(current,code,error,selection_.cleanupOpenGlassConfig)' in engine
assert 'fullRemovalProfiles_.CleanupRegistry(detail)' in engine
registry=profile[profile.index('    bool PreflightRegistryCleanup'):profile.index('    bool PreflightCleanup')]
assert 'savedThemes' not in registry and 'GlassUserDataCleanup' not in registry
assert 'CleanPlatformSettings' in registry and 'VerifyInventory' in registry
src=r'''
#include "GlassSetup/src/GlassProducts.h"
#include <cassert>
int main(){using namespace GlassProducts;
 for(unsigned installed=0;installed<128;++installed)for(unsigned removed=0;removed<128;++removed){
 Selection s{};s.platform64=s.platform32=s.openGlass=s.glassSettings=false;s.removeMask=removed;
 bool want=(installed&3)&&((removed&(installed&3))==(installed&3));
 assert(PlatformCleanupSelected(s,installed)==want);
 s.cleanupOpenGlassConfig=true;assert(PlatformCleanupSelected(s,installed)==want);
 s.platform64=true;assert(!PlatformCleanupSelected(s,installed));
 }
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';p.write_text(src);exe=Path(d)/'test'
 subprocess.run(['g++','-std=c++17','-I'+str(root),str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: separate OG/Platform consent, 16384 Platform inventory/removal combinations, registry-only scope')
