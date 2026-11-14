"""Stop/restore source ordering regression for cancellation during cooperative stop."""
from pathlib import Path
root=Path(__file__).resolve().parents[2]
host=(root/'GlassSetup/src/InstallSupport/GlassInstallHost.cpp').read_text()
stop=host[host.index('bool StopGlassPlatformImpl('):host.index('bool StopGlassPlatform(',host.index('bool StopGlassPlatformImpl('))]
signal=stop.index('Shared::GlassStop::Request(arch, options)')
assert stop.index('if(stopAttempted)*stopAttempted=true;')<signal
assert 'if (stopAttempted) *stopAttempted = false;' not in stop
assert 'if (stopAttempted) *stopAttempted=attempted;' not in stop
engine=(root/'GlassSetup/src/InstallEngine.cpp').read_text()
restore=engine[engine.index('bool InstallEngine::RestorePriorHosts('):engine.index('bool InstallEngine::PrepareOfficialOpenGlass(')]
assert restore.index('if (stopAttempted && !GP::WaitForPlatformStopped')<restore.index('if (hostRunning) return true;')<restore.index('GP::StartPlatform(arch)')
print('PASS: source ordering: cooperative request recorded before signal, retained through cancellation, restore waits before restart (not Windows runtime coverage)')
