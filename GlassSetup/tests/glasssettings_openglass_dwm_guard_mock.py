"""Source gates for the GlassSettings side of the OpenGlass DWM crash guard.

Not a Windows build/runtime test — verifies the wiring between the C# UI
(Home/System notice strips, autostart consent path), the C-API bridge
(GlassSettings.Native.dll) and the shared implementation (GlassShared).
"""
from pathlib import Path
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[2]

def read(path):
    return (root / path).read_text(encoding='utf-8-sig')

native = read('GlassSettings/Native/OpenGlassDwmGuardNative.cpp')
header = read('GlassSettings/Native/NativeHost.h')
interop = read('GlassSettings/Services/NativeHostInterop.DwmGuard.cs')
service = read('GlassSettings/Services/OpenGlassDwmGuardService.cs')
vcx = read('GlassSettings/Native/GlassSettings.Native.vcxproj')
filters = read('GlassSettings/Native/GlassSettings.Native.vcxproj.filters')
actions = read('GlassSettings/Core/Platform/SystemPlatformRunActions.cs')
home = read('GlassSettings/Pages/HomePage.xaml.cs')
system = read('GlassSettings/Pages/SystemPage.xaml.cs')
row = read('GlassSettings/Controls/PlatformSchedulerRow.cs')
shared_header = read('GlassShared/include/OpenGlassDwmGuard.h')
shared_src = read('GlassShared/src/OpenGlassDwmGuard.cpp')

# --- native bridge: every export declared, defined and consumed
for name in (
    'GlassSettings_OpenGlassDwmGuard_Arm',
    'GlassSettings_OpenGlassDwmGuard_Disarm',
    'GlassSettings_OpenGlassDwmGuard_QueryState',
    'GlassSettings_OpenGlassDwmGuard_IsWatching',
    'GlassSettings_OpenGlassDwmGuard_IsQuarantined',
    'GlassSettings_OpenGlassDwmGuard_ShouldSuppressOpenGlassStart',
    'GlassSettings_OpenGlassDwmGuard_ClearQuarantine',
    'GlassSettings_OpenGlassDwmGuard_GetQuarantineInfo',
):
    assert name in header, f'NativeHost.h missing {name}'
    assert f'{name}(' in native, f'OpenGlassDwmGuardNative.cpp missing {name}'
    assert f'EntryPoint = "{name}"' in interop, f'interop missing {name}'

# --- shared implementation sanity (SubKey маркер, состояния, карантин)
BS = chr(92)
marker = f'Software{BS}{BS}StanislavKa{BS}{BS}GlassPlatform{BS}{BS}OpenGlassGuard'
assert marker in shared_header
assert 'kStateQuarantined' in shared_src and 'TryShutdown' in shared_src
assert 'DisableOgServiceAutostartKeepingDisabled' in shared_src  # DEMAND_START; DISABLED сохраняется

# --- project wiring
assert 'OpenGlassDwmGuardNative.cpp' in vcx and 'OpenGlassDwmGuardNative.cpp' in filters

# --- C# service: arm/poll/reinstate semantics
assert 'NativeHostInterop.OpenGlassDwmGuardArm' in service
assert 'OpenGlassDwmGuardIsWatching' in service  # this process is the watcher?
assert 'CreateTimer' in service and 'NotifyLifecycleChanged' in service
assert 'ReinstateAndLaunchAsync' in service
assert 'SetOrchestrationAutostartEnabled(true)' in service
assert 'OpenGlassDwmGuardClearQuarantine' in service
assert 'ShouldShowNotice' in service

# --- launch path arms BEFORE starting OG; disarms when start not confirmed
launch = actions.index('public static async Task<bool> StartOpenGlassAsync()')
launch_end = actions.index('    /// <summary>Home/System run controls', launch)
block = actions[launch:launch_end]
assert block.index('OpenGlassDwmGuardService.ArmForSettings()') < block.index('OpenGlassHostService.TryStartHostNoWait')
assert block.count('OpenGlassDwmGuardService.Disarm()') == 2  # start refused / not confirmed
assert 'ArmForSettings' in block

# --- notice surfaces: both pages reference the guard service + string keys
en = ET.parse(root / 'GlassSettings/Strings/en-US/Resources.resw')
ru = ET.parse(root / 'GlassSettings/Strings/ru-RU/Resources.resw')
need = {'OpenGlass_GuardNotice_Title', 'OpenGlass_GuardNotice_Body',
        'OpenGlass_GuardNotice_Action', 'OpenGlass_GuardNotice_Reinstated'}
for lang, tree in (('en-US', en), ('ru-RU', ru)):
    names = {d.get('name') for d in tree.getroot().iter('data')}
    missing = need - names
    assert not missing, f'{lang}: missing {missing}'

assert 'OpenGlassDwmGuardService.ShouldShowNotice()' in home
assert 'OpenGlass_GuardNotice_Action' in home
assert 'OpenGlassDwmGuardService.ReinstateAndLaunchAsync()' in home
assert '_guardUpdateNoticeActive' in home
assert 'OpenGlassDwmGuardService.ShouldShowNotice()' in system
assert 'OpenGlass_GuardNotice_Action' in system
assert 'OpenGlassDwmGuardService.ReinstateAndLaunchAsync()' in system
assert '_guardNoticeActive' in system

# --- manual consent: enabling OG autostart from the scheduler row clears quarantine
assert 'NotifyUserEnabledAutostart' in row
assert 'NotifyUserEnabledAutostart' in service

# --- launch via row toggle routes through StartOpenGlassAsync (with Arm)
assert 'SystemPlatformRunActions.StartOpenGlassAsync()' in row

print('PASS: GlassSettings DWM-guard wiring: exports <-> interop <-> service; arm in launch; notices in Home/System; consent path')
