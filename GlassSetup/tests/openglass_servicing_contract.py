#!/usr/bin/env python3
"""Source wiring checks, not Win32 registry/SCM integration tests."""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
u = (root/'GlassUnins/main.cpp').read_text(encoding='utf-8-sig')
m = (root/'src/InstallSupport/GlassInstallMsi.cpp').read_text(encoding='utf-8-sig')
r = (root/'src/InstallSupport/GlassInstallRollback.cpp').read_text(encoding='utf-8-sig')
assert 'fileRootsKnown&&GlassOpenGlassFiles::HasReceipt(fileRoots.openGlass)' in u
assert 'chosen.openGlass&&!fileOpenGlass' in u
assert 'if(fileOpenGlass)facts[P::Index(P::Product::OpenGlass)]' in u
branch = u[u.index('if (product==P::Product::OpenGlass) {'):]
assert branch.index('fileRemoval->Begin') < branch.index('fileRemoval->Remove')
assert branch.index('RemoveOpenGlassServiceAndTasks') < branch.index('fileRemoval->Commit')
restore = u[u.index('auto restoreRemaining='):u.index('auto restoreRemaining=')+1400]
assert restore.index('fileRemoval->Rollback') < restore.index('restoreArch(')
arp = m[m.index('static bool RefreshOpenGlassFileArp'):m.index('bool RefreshManagedArp')]
assert 'Uninstall\\\\GlassPlatform.OpenGlass' in arp
assert arp.index('if(!ours)') < arp.index('RegDeleteKeyExW')
assert arp.index('GlassOpenGlassFiles::Inspect') < arp.index('RegCreateKeyExW')
assert 'CurrentFile(L"GlassUnins.exe")' in arp and 'CurrentFile(L"GlassSetup.exe")' in arp
assert 'put(L"DisplayVersion",record.version)' in arp
assert '--modify --product' in arp and '--silent' in arp
assert 'ok=RefreshOpenGlassFileArp(openGlassDir)&&ok;' in m
assert '!state.restoreOpenGlassFiles && !state.backupOpenGlass.empty()' in r
print('PASS: source contracts for OpenGlass file discovery/removal/journal, separate owned ARP and no second directory rollback')
