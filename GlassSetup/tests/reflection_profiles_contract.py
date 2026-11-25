"""Portable profile protocol/wiring checks; not a Windows build/runtime test."""
from pathlib import Path
import struct
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[2]
def read(path):
    return (root / path).read_text(encoding='utf-8-sig')
native = read('GlassSettings/Native/ReflectionProfilesNative.cpp')
api = read('GlassShared/include/ReflectionProfileApi.h')
host = read('GlassCompat/src/ReflectionProfileHost.h')
entry = read('GlassCompat/src/EntryRundll.cpp')
service = read('GlassSettings/Services/ReflectionTextureService.cs')
state = read('GlassSettings/Services/ReflectionRenderSettings.cs')
dialog = read('GlassSettings/Services/ReflectionSettingsDialog.cs')
page = read('GlassSettings/Pages/Dwm/DwmThemeDetailPage.xaml.cs')
watcher = read('GlassSettings/Services/DesktopEnvironmentWatcher.cs')
wrapper = read('GlassSettings/Services/ReflectionDisplayProfile.cs')

assert 'Snapshot' in api and 'monitors[32]' in api and 'key[128]' in api
assert 'SizeConst = 32' in wrapper and 'SizeConst = 128' in wrapper
assert 'ReflectionDisplayProfile.DisplayMonitor[] Monitors)' in wrapper
assert 'internal struct DisplayMonitor' in wrapper and 'new DisplayMonitor[32]' in wrapper
assert 'Monitor[] Monitors)' not in wrapper.replace('DisplayMonitor[] Monitors)', '')
for export in ('CaptureReflectionProfile', 'SignalReflectionDisplayChange', 'TickReflectionProfiles'):
    assert f'GlassSettings_{export}V1' in native and f'GlassSettings_{export}V1' in host
assert 'IsWow64Process' in host and '|| wow)' in host
assert 'LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32' in host
assert 'FILE_ATTRIBUTE_REPARSE_POINT' in host and 'if (!module)' in host
assert 'optional helper load failed' in host and 'helper ready:' in host
assert 'host loop alive; optional helper=' in host
assert 'nextStatus = now + 30000' in host
# Debug prefers authoritative flat VC output over potentially stale Settings copy.
flat_start = host.index("// Native.vcxproj's authoritative Debug output")
assert host.rfind('#ifdef _DEBUG', 0, flat_start) > host.index('std::wstring target')
flat_end = host.index('#endif', flat_start)
flat = host[flat_start:flat_end]
assert 'buildAttributes != INVALID_FILE_ATTRIBUTES' in flat and 'target = buildTarget' in flat
assert 'error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND' in flat
assert flat_end < host.index('std::wstring probe = target') < host.index('LoadLibraryExW')
assert 'missing V1 exports path=%ls capture=%d signal=%d tick=%d' in host
assert 'RebindReflectionToDisplayAsync' in page and 'var recipe = matching ?? previous;' in page
assert '_pendingDisplayReflectionSource' in page and 'canApply: canApply' in page
assert 'ReflectionRenderState.IsManagedOutput(active)' in page
assert 'ReadForOutput(_currentReflectionPath)' in page
assert 'state.Output, output, StringComparison.OrdinalIgnoreCase' in state
assert 'skip unmanaged:' in native and 'selection=%ls active=%ls output=%ls' in native
assert 'WM_THEMECHANGED posted=%d error=%lu' in native
assert '_wcsicmp(path, target.c_str())' in host
assert 'ReflectionProfileHost::OnMessage(msg);' in entry  # sent as well as posted messages
assert 'ReflectionProfileHost::Start(hSelf)' in entry and 'ReflectionProfileHost::Poll()' in entry
assert 'ReflectionProfileHost::Stop()' in entry
assert 'signal(); // Initial reconcile' in host
assert 'GetTickCount64() + 750' in native
assert 'SetThreadDpiAwarenessContext' in native and 'GetDpiForMonitor' in native
assert 'm.x -= left; m.y -= top;' in native
assert 'm.dpiX, m.dpiY' in native and 'std::sort' in native
assert 'const bool adopt = managed != 0' in native
assert 'if (!adopt)' in native
assert 'RegDeleteValueW(dwm.value, L"CustomThemeReflection")' in native
assert 'ValidHash(candidate,' in native and 'integer(16) == DWORD(width)' in native
assert 'FindWindowW(L"Dwm", nullptr)' in native and 'PostMessageW(window, WM_THEMECHANGED' in native
assert 'HWND_BROADCAST' not in native and 'WM_DWMCOLORIZATIONCOLORCHANGED' not in native
assert 'CaptureSnapshot(&check)' in native and 'wcscmp(current.key, check.key)' in native
assert 'MutexName' in native and 'ReflectionDisplayProfile.PublicationMutex' in service
assert 'ReflectionDisplayProfile.PublicationMutex' in page
assert 'Profiles' in state and 'state.Profile != profile' in state
assert state.index('key.SetValue("Ready", 0') < state.index('key.SetValue("State"')
assert state.index('key.SetValue("OutputHash"') < state.index('key.SetValue("Ready", state.Ready')
assert 'profile.Key + ".png"' in service and 'File.Delete(file)' not in service
assert 'Directory.Enumerate' not in service and 'Inventory(' not in service
assert 'saved.Profile == profile.Key' in service and 'saved.SourceHash == sourceHash' in service
assert 'ReflectionRenderState.ReleaseManagement()' in page and 'ReflectionRenderState.MarkManaged' in page
assert 'expectedProfile != profile.Key' in service
assert 'args.DisplayChanged' in dialog and 'var replacement = new ReflectionDesktopPreview()' in dialog
assert 'CloseDismissed()' not in dialog
assert 'old.Dispose()' in dialog and 'preview.Dispose()' in dialog
assert 'body.Children.Move((uint)body.Children.IndexOf(replacement), (uint)index)' in dialog
# the displaced old preview must be removed, otherwise both layouts stay visible
assert 'body.Children.Remove(old);' in dialog
assert dialog.index('body.Children.Move(') < dialog.index('body.Children.Remove(old);') < dialog.index('initialProfile = replacement.ProfileKey')
assert '!ReferenceEquals(treeAtStart, previewTree)' in dialog
assert 'initialProfile = replacement.ProfileKey' in dialog
assert 'ReflectionDisplayProfile.Capture().Key == initialProfile' in dialog
assert '_pendingDisplay' in watcher and 'DisplayChanged = displayChanged' in watcher
assert 'new RegistryValueChangeWatcher' not in wrapper
project = ET.parse(root / 'GlassSettings/Native/GlassSettings.Native.vcxproj')
assert any(x.get('Include') == 'ReflectionProfilesNative.cpp' for x in project.iter())
print('PASS: optional host-only Native, versioned ABI, startup/live observation, safe ownership/default, verified PNG and serialized per-profile publication')

# Executable specification of canonical FNV byte order used by Native.
def key(monitors):
    monitors = sorted(monitors)
    left = min(m[0] for m in monitors)
    top = min(m[1] for m in monitors)
    width = max(m[0] + m[2] for m in monitors) - left
    height = max(m[1] + m[3] for m in monitors) - top
    fingerprint = 14695981039346656037
    for x, y, w, h, dx, dy in monitors:
        for byte in struct.pack('<6I', x-left, y-top, w, h, dx, dy):
            fingerprint = ((fingerprint ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return f'{width}x{height}_DPI{monitors[0][4]}_{fingerprint:016X}'
a = [(0, 0, 1920, 1080, 96, 96), (1920, 0, 2560, 1440, 144, 144)]
assert key(a) == key(list(reversed(a)))
assert key(a) == key([(x-1920, y+80, w, h, dx, dy) for x,y,w,h,dx,dy in a])
assert key(a) != key([a[0], (1920, 0, 2560, 1440, 120, 120)])
assert key(a) != key(a[:1])
assert key(a) != key([a[0], (1920, 100, 2560, 1440, 144, 144)])
print('PASS: canonical identity specification: enumeration/origin invariant, geometry/DPI/connectivity sensitive')
print('NOTE: Windows compilation, optional loader, HKCU/DWM, mixed-DPI and hot-plug require Windows testing')

assert 'SettingsSession.Current.AcceptAutomaticReflectionBaseline(baselineAtSignal, path)' in page
assert 'StillSelected, activeAtSignal);' in page
assert 'if (canApply is not null' in page
snapshot = read('GlassSettings/Core/Snapshot/SettingsSnapshot.cs')
session = read('GlassSettings/Services/SettingsSession.cs')
watch = read('GlassSettings/Services/RegistrySnapshotWatchService.cs')
assert 'ReconcileAutomaticReflectionFromWatch' not in watch and 'ReconcileAutomaticReflectionFromWatch' not in session
profile = read('GlassSettings/Services/ReflectionDisplayProfile.cs')
assert 'GetMonitorInfoW' in profile and 'MonitorDefaultToNearest' in profile
assert 'TakeWhile(character => char.IsDigit(character))' in profile
assert 'internal bool AcceptAutomaticReflectionBaseline(string? previousOutput, string expectedOutput)' in snapshot
assert 'internal bool AcceptAutomaticReflectionBaseline(string? previousOutput, string expectedOutput)' in session
assert 'ReflectionRenderState.ReadForOutput(expectedOutput) is null' in session
assert 'StampDirtyScopesFromSnapshot();' in session
assert 'string.Equals(previousOutput, expectedOutput, StringComparison.OrdinalIgnoreCase)' in snapshot
assert 'CaptureDwmKeySubset([name], DwmRegistryHive.Hkcu)' in snapshot
assert 'ReconcileScopeDirtyFlags();' in session
assert 'Save(' not in snapshot[snapshot.index('internal bool AcceptAutomaticReflectionBaseline'):snapshot.index('internal bool AcceptAutomaticReflectionBaseline') + 700]
print('PASS: live dialog re-layout preserves draft, single reflection canvas and targeted automatic path baseline reconciliation')

state = read('GlassSettings/Services/ReflectionRenderSettings.cs')
assert 'public static State? ReadReadyProfile()' in state
assert 'public static bool AdoptReadyProfile(string profile)' in state
assert 'PlatformPaths.ReflectionAppliedDirectory' in state
assert 'MarkManaged(state.Output, notify: false)' in state
assert 'RestoreReadyReflectionProfile();' in page
assert 'ReflectionRenderState.IsManagedOutput(active)' in page
assert 'ReflectionDisplayProfile.PublicationMutex' in page
print('PASS: startup restores a verified ready canvas for the current topology without replacing external selections')

native = read('GlassSettings/Native/ReflectionProfilesNative.cpp')
assert 'const bool adopt = managed != 0' in native
assert '(active.empty() || _wcsicmp(active.c_str(), owned.c_str()) != 0)' in native
assert '_wcsicmp(active.c_str(), output.c_str())' in native
print('PASS: platform startup adopts the ready canvas only when the last selection was managed')
