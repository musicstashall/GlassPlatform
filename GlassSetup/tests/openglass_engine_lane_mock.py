#!/usr/bin/env python3
"""Standalone vendor lane policy and source-order guards; not Windows acceptance."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSetup/src/InstallEngine.cpp').read_text()
worker=s[s.index('DWORD WINAPI InstallEngine::WorkerThread('):s.index('bool InstallEngine::Start(')]
assert worker.index('RunOfficialOpenGlassOnly()') < worker.index('CapturePriorHostState()')
lane=s[s.index('bool InstallEngine::RunOfficialOpenGlassOnly()'):s.index('DWORD WINAPI InstallEngine::WorkerThread(')]
for forbidden in ['StopRunningHosts(', 'CreateRollbackSnapshot(', 'RunMsiSequence(', 'RegisterSelectedProducts(', 'WriteOpenGlassInstallPath(', 'RefreshManagedArp(', 'PerformRollback(']:
    assert forbidden not in lane, forbidden
assert lane.index('StandaloneOpenGlassApply(')<lane.index('EnsureTempWorkspace(')<lane.index('PrepareInstallerForCurrentSystem(')<lane.index('RunInstaller(')
assert lane.index('Same(before,current)')<lane.index('RunInstaller(')
assert lane.index('launchingPhase_.store(true')<lane.index('RunInstaller(')
assert 'target=before.directory' in lane and 'target=native+L"\\\\OpenGlass"' in lane
assert 'rollbackSucceeded_.store(false' in lane and 'ERROR_SUCCESS_REBOOT_REQUIRED' in lane
assert 'after.version==openGlassPackage_.version' in lane
ui=(root/'GlassSetup/src/GlassProductDownloads.h').read_text()
assert 'GlassOpenGlassPackage::PrepareInstallerForCurrentSystem(' in ui
h=r'''
#include "GlassSetup/src/GlassProducts.h"
#include <cassert>
#include <initializer_list>
int main(){
 using namespace GlassProducts;Selection s;
 s.platform64=s.platform32=s.glassSettings=false;
 assert(StandaloneOpenGlassApply(s,false,true));
 assert(!StandaloneOpenGlassApply(s,true,true));
 assert(!StandaloneOpenGlassApply(s,false,false));
 for(bool Selection::* member:{&Selection::platform64,&Selection::platform32,&Selection::glassSettings,
 &Selection::settings64,&Selection::settings32,&Selection::secureUxTheme,&Selection::secureUxThemeRemove,
 &Selection::cleanupUserData,&Selection::trustSigner}){
 s.*member=true;assert(!StandaloneOpenGlassApply(s,false,true));s.*member=false;
 }
 for(unsigned i=0;i<6;++i){s.removeMask=1u<<i;assert(!StandaloneOpenGlassApply(s,false,true));}
 s.removeMask=0;s.openGlass=false;assert(!StandaloneOpenGlassApply(s,false,true));
}
'''
with tempfile.TemporaryDirectory() as tmp:
    cpp=Path(tmp)/'policy.cpp';exe=Path(tmp)/'policy';cpp.write_text(h)
    subprocess.run(['g++','-std=c++17','-I',str(root),str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('PASS: standalone OpenGlass policy and engine boundary source guards')
