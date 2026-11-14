"""Source/schema contract only; does not execute C# or Windows registry/file APIs."""
from pathlib import Path
import re, xml.etree.ElementTree as ET
root=Path(__file__).resolve().parents[2]
s=(root/'GlassSettings/Services/SettingsRegistryBackupService.cs').read_text(encoding='utf-8')
cpp=(root/'GlassShared/src/RegistrySanity.cpp').read_text(encoding='utf-8').split('constexpr Rule kDwmOpenGlassRules[] = {',1)[1].split('};',1)[0]
cs=s.split('private static readonly string[] DwmNames =',1)[1].split('];',1)[0]
assert set(re.findall(r'L"([^"]+)"',cpp))==set(re.findall(r'"([^"]+)"',cs))
assert len(re.findall(r'"([^"]+)"',cs))==49
for token in ['.glassbackup','formatVersion = 2','FileMode.CreateNew','ZipArchiveMode.Create','ZipArchiveMode.Read',
              'FixedTimeEquals','RegQueryValueExW','RegOpenKeyExW','RegSetValueExW','RegCreateKeyExW','RegDeleteValueW',
              'parent.View','type == 6','includesUserFiles = false','restoreImplemented = true',
              'RegistryHive.CurrentUser','Current user HKCU only','appearance','processes','openglass',
              'AttachList','BlockList','GlassSettings','InstallPathX64','OpenGlassInstallPath',
              'CustomThemeReflection','SetFileInformationByHandle','MaxValues','MaxBytes']:
    assert token in s, token
assert 'RegistryHive.Users' not in s and 'ProfileList' not in s and 'NTUSER.DAT' not in s
assert 'RegLoad' not in s and 'RegUnload' not in s
assert 'FileMode.Create' not in s.replace('FileMode.CreateNew','')
assert '0xC0010000' not in s and 'FILE_FLAG_BACKUP_SEMANTICS' not in s
assert s.index('Verify(zip, "registry.json"') < s.index('return path;')
assert 'UnmanagedType.U1' in s
ui=(root/'GlassSettings/Pages/SystemPage.xaml').read_text(encoding='utf-8')
assert 'CreateBackup_OnClick' in ui and 'RestoreApply_OnClick' in ui and 'RestoreSelect_OnClick' in ui
assert 'AllowBodyExpand="False"' in ui and 'x:Name="BackupExpander"' in ui
assert 'BackupPivot' in ui
host=(root/'GlassSettings/Pages/SystemPage.xaml.cs').read_text(encoding='utf-8')
backup=(root/'GlassSettings/Pages/SystemPage.Backup.cs').read_text(encoding='utf-8')
assert 'await SettingsRegistryBackupService.SaveAsync' in backup
assert 'ApplyAsync' in backup and 'OpenRestore' in backup
assert 'SettingsNoticeCoordinator.PresentAsync' in host
joined = host + backup
assert 'SettingsNoticeSeverity.Error' in joined and 'SettingsNoticeSeverity.Success' in joined
assert '_backupNoticeActive' in host
assert 'ReloadRenderFlags.AttachList' in backup
assert 'NotifyDwmColorizationChanged' in backup and 'NotifyThemeChanged' in backup
assoc=(root/'GlassSettings/Services/SettingsBackupAssociation.cs').read_text(encoding='utf-8')
assert 'EnsureIfMissing' in assoc and 'GlassPlatform.SettingsBackup' in assoc
engine=(root/'GlassSetup/src/InstallEngine.cpp').read_text(encoding='utf-8')
assert 'RegisterSettingsBackupAssociation' in engine
unins=(root/'GlassSetup/GlassUnins/main.cpp').read_text(encoding='utf-8')
assert 'RemoveSettingsBackupAssociationIfOwned' in unins
for lang in ['ru-RU','en-US']:
    doc=ET.parse(root/f'GlassSettings/Strings/{lang}/Resources.resw')
    names=[e.attrib['name'] for e in doc.findall('data')]
    for name in ['System_BackupTitle','System_BackupDescription','System_BackupSave','System_BackupBusy','System_BackupSaved','System_BackupFailed',
                 'System_BackupCreateTab','System_BackupRestoreTab','System_BackupCreateHeading','System_BackupCreateCall',
                 'System_BackupRestoreHeading','System_BackupRestoreCall','System_BackupPartAppearance','System_BackupPartProcesses',
                 'System_BackupPartOpenGlass','System_BackupPartApp','System_BackupSelect','System_BackupApply',
                 'System_BackupApplied','System_BackupApplyFailed','System_BackupInvalidFile','System_BackupFilePickerTitle',
                 'System_BackupFileFilter','System_BackupFileType']:
        assert names.count(name)==1, (lang, name)
print('PASS: backup source contract, exact 49-value DWM allowlist, current-user HKCU sections, .glassbackup restore; NOT a C#/Windows execution test')
assert '0x0009 | (int)parent.View' in s
for context in ['Read registry key {parent.Name}', 'Write registry value HKCU']:
    assert context in s, context
assert 'RegistryBackupRow' not in ui
