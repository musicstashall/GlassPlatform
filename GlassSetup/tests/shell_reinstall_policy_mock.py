#!/usr/bin/env python3
"""Shell reinstall safety checks; the deployment APIs are not executed."""
from pathlib import Path
import xml.etree.ElementTree as ET
repo=Path(__file__).resolve().parents[2]
engine=(repo/'GlassSetup/src/InstallEngine.cpp').read_text(encoding='utf-8')
ops=(repo/'GlassSettings/Native/ShellExtensionOps.cpp').read_text(encoding='utf-8')
sparse=(repo/'GlassSettings/Native/ShellSparsePackage.cpp').read_text(encoding='utf-8')
util=(repo/'GlassSetup/src/InstallSupport/GlassInstallUtil.cpp').read_text(encoding='utf-8')
def function(text,token):
    start=text.index(token);brace=text.index('{',start);depth=0
    for i in range(brace,len(text)):
        if text[i]=='{':depth+=1
        if text[i]=='}':
            depth-=1
            if not depth:return text[start:i+1]
    raise ValueError(token)
launch=function(engine,'bool InstallEngine::LaunchAndVerifySelectedProducts(')
reg=launch[launch.index('bool shellConflict=false;'):]
assert 'InvokeShellExtRegistration(true,x64Dir_,x86Dir_' in reg
assert 'cleanInstall_' not in reg # also during reinstalls
entry=function(ops,'void CALLBACK GlassSettings_ShellExtRegisterSetupV1W(')
assert 'QueryInstallerSparseState' in entry and 'InstallerSparseHasVerifiedMenu' in entry
assert '0x47504346u' in entry and 'HasMatchingLegacyShellRegistration' in entry
query=function(sparse,'InstallerSparseState QueryInstallerSparseState(')
assert 'FindPackagesForUser' in query and 'kSparsePackageFamilyName' in query
assert 'EffectiveExternalPath()' in query and 'VerifyIsOK()' in query and 'pm.FindPackages()' in query
assert 'InstallerSparseState::Conflict' in query and 'InstallerSparseState::Unknown' in query
check=function(sparse,'bool InstallerSparseHasVerifiedMenu(')
assert 'pkg.IsDevelopmentMode()' in check and 'return verb && cls' in check
assert "@Id='GlassPlatform00Toggle'" in check
remove=function(sparse,'bool UnregisterInstallerSparsePackageQuiet(')
assert 'pm.RemovePackageAsync' in remove and 'FindPackagesForUser' in remove
assert 'EnsureSparsePackagesRemoved' not in remove and 'RegisterPackageByUriAsync' not in remove
register=function(sparse,'bool RegisterSparsePackageQuiet(')
assert 'if (installerMode)' in register and 'ForceUpdateFromAnyVersion(!installerMode)' in register
assert '!registered && !installerMode' in register
invoke=function(util,'bool InvokeShellExtRegistration(')
assert '!alreadyRegistered && !conflict' in invoke and '*registrationConflict=conflict' in invoke
assert 'INSTALLLOGATTRIBUTES_APPEND' in function(engine,'bool InstallEngine::RunOneMsi(')

ns={'d4':'http://schemas.microsoft.com/appx/manifest/desktop/windows10/4',
    'd5':'http://schemas.microsoft.com/appx/manifest/desktop/windows10/5',
    'com':'http://schemas.microsoft.com/appx/manifest/com/windows10'}
normal=ET.parse(repo/'GlassShellExt/Package/AppxManifest.xml').getroot()
stub=ET.parse(repo/'GlassShellExt/Package/AppxManifest.UnregisterStub.xml').getroot()
identity=lambda r:next(x for x in r if x.tag.endswith('}Identity')).attrib
assert identity(normal)==identity(stub) # family/root/status alone cannot distinguish them
verb=lambda r:r.findall('.//d4:Extension[@Category="windows.fileExplorerContextMenus"]/d4:FileExplorerContextMenus/d5:ItemType/d5:Verb',ns)
assert len(verb(normal))==1 and not verb(stub)
assert verb(normal)[0].attrib['Id']=='GlassPlatform00Toggle'
print('PASS: reinstall gate, identity/root checks, preserved conflicts, per-user removal and stub manifest source invariants')
