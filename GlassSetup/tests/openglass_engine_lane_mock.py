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
for forbidden in ['StopRunningHosts(', 'CreateRollbackSnapshot(', 'RunMsiSequence(', 'RegisterSelectedProducts(', 'WriteOpenGlassInstallPath(', 'PerformRollback(']:
    assert forbidden not in lane, forbidden
assert lane.index('StandaloneOpenGlassApply(')<lane.index('EnsureTempWorkspace(')<lane.index('PrepareInstallerForCurrentSystem(')<lane.index('RunInstaller(')
assert lane.index('Same(officialOpenGlassBefore_,current)')<lane.index('RunInstaller(')
assert lane.index('launchingPhase_.store(true')<lane.index('RunInstaller(')
assert 'target=before.directory' in lane and 'target=native+L"\\\\OpenGlass"' in lane
assert 'rollbackSucceeded_.store(false' in lane and 'ERROR_SUCCESS_REBOOT_REQUIRED' in lane
assert 'GlassOpenGlassInstall::SameVersion(after.version,openGlassPackage_.version)' in lane
assert 'RunUninstaller(current,code,error)' in lane and 'removing?!after.installed' in lane
assert 'if(!removing) {' in lane
unsupported=lane[lane.index('if(!removing&&!GlassProducts::StandaloneOpenGlassApply'):lane.index('return PrepareOfficialOpenGlass')]
assert 'ERROR_NOT_SUPPORTED' in unsupported and 'ERROR_INSTALL_PACKAGE_REJECTED' not in unsupported
assert 'OpenGlass dispatcher revision=2: route=%s' in worker
assert 'Running Setup FileVersion: %s' in (root/'GlassSetup/src/main.cpp').read_text()
inspect=(root/'GlassSetup/src/InstallSupport/GlassProductInspect.cpp').read_text()
assert 'PayloadFileVersion(official.directory+' in inspect
assert 'officialOpenGlassRoot.empty()?' in inspect
assert 'report.og.integrityKnown=false; // Registration/FileVersion is not a payload hash check.' in inspect

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
 &Selection::cleanupUserData,&Selection::trustSigner,&Selection::openGlassResources}){
 s.*member=true;assert(!StandaloneOpenGlassApply(s,false,true));s.*member=false;
 }
 for(unsigned i=0;i<7;++i){s.removeMask=1u<<i;assert(!StandaloneOpenGlassApply(s,false,true));}
 s.removeMask=0;
 s.platform64=true;assert(MixedOpenGlassApply(s,false,true));
 s.openGlassResources=true;assert(MixedOpenGlassApply(s,false,true));s.openGlassResources=false;
 assert(!MixedOpenGlassApply(s,true,true));assert(!MixedOpenGlassApply(s,false,false));
 s.secureUxTheme=true;assert(!MixedOpenGlassApply(s,false,true));s.secureUxTheme=false;
 s.removeMask=8;assert(!MixedOpenGlassApply(s,false,true));s.removeMask=0;
 s.platform64=false;s.independentSettings=true;s.settings64=true;assert(MixedOpenGlassApply(s,false,true));s.settings64=false;
 assert(!MixedOpenGlassApply(s,false,true));
 s.openGlass=false;assert(!StandaloneOpenGlassApply(s,false,true));
 s.removeMask=8;assert(StandaloneOpenGlassRemoval(s,false,true));
 assert(!StandaloneOpenGlassRemoval(s,true,true));assert(!StandaloneOpenGlassRemoval(s,false,false));
 s.openGlass=true;assert(!StandaloneOpenGlassRemoval(s,false,true));s.openGlass=false;
 s.cleanupUserData=true;assert(!StandaloneOpenGlassRemoval(s,false,true));s.cleanupUserData=false;
 auto mixed=Selection{};mixed.platform64=mixed.platform32=mixed.openGlass=false;
 mixed.independentSettings=true;mixed.settings32=true;mixed.glassSettings=true;mixed.removeMask=8;
 assert(MixedOpenGlassRemoval(mixed,false,true)); // Exact user plan: rows=0x28.
 mixed.openGlassResources=true;assert(!MixedOpenGlassRemoval(mixed,false,true));mixed.openGlassResources=false;
 assert(!MixedOpenGlassRemoval(mixed,true,true));assert(!MixedOpenGlassRemoval(mixed,false,false));
 mixed.secureUxTheme=true;assert(!MixedOpenGlassRemoval(mixed,false,true));mixed.secureUxTheme=false;
 mixed.cleanupUserData=true;assert(MixedOpenGlassRemoval(mixed,false,true));mixed.cleanupUserData=false;
 mixed.removeMask=9;assert(MixedOpenGlassRemoval(mixed,false,true));
 s.removeMask=12;assert(!StandaloneOpenGlassRemoval(s,false,true));

}
'''
with tempfile.TemporaryDirectory() as tmp:
    cpp=Path(tmp)/'policy.cpp';exe=Path(tmp)/'policy';cpp.write_text(h)
    subprocess.run(['g++','-std=c++17','-I',str(root),str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('PASS: standalone OpenGlass policy and engine boundary source guards')
