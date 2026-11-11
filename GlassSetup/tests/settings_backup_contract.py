"""Source/schema contract only; does not execute C# or Windows registry/file APIs."""
from pathlib import Path
import re, xml.etree.ElementTree as ET
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSettings/Services/SettingsRegistryBackupService.cs').read_text()
cpp=(root/'GlassShared/src/RegistrySanity.cpp').read_text().split('constexpr Rule kDwmOpenGlassRules[] = {',1)[1].split('};',1)[0]
cs=s.split('private static readonly string[] DwmNames = [',1)[1].split('];',1)[0]
assert set(re.findall(r'L"([^"]+)"',cpp))==set(re.findall(r'"([^"]+)"',cs))
assert len(re.findall(r'"([^"]+)"',cs))==49
for token in ['SpecialFolder.MyDocuments','ZipArchiveMode.Create','ZipArchiveMode.Read','file.Flush(true)',
              'FixedTimeEquals','RegQueryValueExW','RegOpenKeyExW','parent.View','type == 6',
              'includesUserFiles = false','restoreImplemented = false','RegistryHive.Users',
              'Profile inventory changed','MaxValues','MaxBytes','SetFileInformationByHandle',
              'GetFileInformationByHandle','0xC0010000, 0,','current, 0x80, 3,',
              'InstallPathX64','OpenGlassInstallPath','No complete backup was created']:
    assert token in s,token
assert 'RegLoad' not in s and 'RegUnload' not in s
assert 'FileMode.Create' not in s and 'File.Delete(' not in s
assert s.index('Verify(zip, "registry.json"') < s.index('return path;')
assert 'UnmanagedType.U1' in s # FILE_DISPOSITION_INFO is BOOLEAN, not BOOL.
ui=(root/'GlassSettings/Pages/SystemPage.xaml').read_text(encoding='utf-8-sig')
assert 'RegistryBackup_OnClick' in ui and 'RegistryBackupStatus' in ui
host=(root/'GlassSettings/Pages/SystemPage.xaml.cs').read_text()
assert 'await SettingsRegistryBackupService.SaveAsync()' in host
assert 'finally { RegistryBackupButton.IsEnabled = true; }' in host
for lang in ['ru-RU','en-US']:
    doc=ET.parse(root/f'GlassSettings/Strings/{lang}/Resources.resw')
    names=[e.attrib['name'] for e in doc.findall('data')]
    for name in ['System_BackupTitle','System_BackupDescription','System_BackupSave','System_BackupBusy','System_BackupSaved','System_BackupFailed']:
        assert names.count(name)==1
print('PASS: backup source contract, exact 49-value DWM allowlist, bounded raw registry schema, guarded archive publication and RU/EN wiring; NOT a C#/Windows execution test')

assert '0x0009 | (int)parent.View' in s
for context in ['Read registry key {parent.Name}', 'Create backup {path}', 'Read Documents ancestor {current}', 'Read profile inventory {sid}']:
    assert context in s, context
assert 'RegistryBackupRow' not in ui
assert 'x:Name="BackupExpander"' in ui
assert 'SettingsNoticeCoordinator.PresentAsync' in host
assert 'SettingsNoticeSeverity.Error' in host and 'SettingsNoticeSeverity.Success' in host
assert '_backupNoticeActive' in host
