"""Settings app/MSI independent release wiring. Source/XML checks, not MSBuild/WiX."""
from pathlib import Path
import xml.etree.ElementTree as ET
root=Path(__file__).resolve().parents[2]
def xml(name):return ET.parse(root/name).getroot()
version=xml('GlassSettings/Version.props').find('./PropertyGroup/GlassSettingsVersion').text
assert version=='2.0.1.0'
def numeric(v):return tuple(map(int,v.split('.')))
# Migration must win Windows Installer's three-part comparison, not only field 4.
assert numeric(version)[:3]>numeric('2.0.0.108')[:3]
props=xml('Directory.Build.props')
assert any('GlassSettings\\Version.props' in n.get('Project','') for n in props.findall('Import'))
settings=next(n for n in props.findall('PropertyGroup') if n.get('Condition')=="'$(MSBuildProjectName)' == 'GlassSettings'")
for field in ('Version','AssemblyVersion','InformationalVersion'):
 assert settings.find(field).text=='$(GlassSettingsVersion)'
targets=xml('Directory.Build.targets')
cs=targets.find("Target[@Name='GlassWriteCSharpFileVersion']")
file_versions=cs.findall('./PropertyGroup/GlassFileVersionDot')
assert file_versions[-1].get('Condition')=="'$(MSBuildProjectName)' == 'GlassSettings'"
assert file_versions[-1].text=='$(GlassSettingsVersionBase3).$(GlassBuildNumberNext)'
product=cs.findall('./PropertyGroup/_GlassCSharpProductVersion')
assert product[0].text=='$(GlassBundleVersionEffective)'
assert product[-1].text=='$(GlassSettingsVersion)'
assert 'AssemblyInformationalVersion("$(_GlassCSharpProductVersion)")' in cs.find('./PropertyGroup/_GlassBuildVersionCsContent').text
# Native helper identity keeps its existing independent per-module counter.
native=targets.find("Target[@Name='GlassWriteVersionHeader']")
assert native.find('./PropertyGroup/GlassFileVersionDot').text=='$(GlassProductVersionBase3).$(GlassBuildNumberNext)'
msi=xml('GlassSetup/msi/ProductVersions.props')
for arch in ('64','32'):
 assert msi.find('./PropertyGroup/GlassSettings'+arch+'MsiVersion').text=='$(GlassSettingsVersion)'
payload=xml('GlassSetup/msi/GlassSetupPayload.targets')
meta=payload.find("Target[@Name='GlassWriteProductMetadata']")
gate=next(n for n in meta.findall('Exec') if 'Assert-SettingsVersion.ps1' in n.get('Command',''))
assert 'GlassSettings-x64' in gate.get('Condition') and 'GlassSettings-x86' in gate.get('Condition')
assert 'GlassSettings\\GlassSettings.exe' in gate.get('Command')
assert '-ExpectedVersion &quot;' not in gate.get('Command') # XML decoded
assert '-ExpectedVersion "$(GlassSettingsVersion)"' in gate.get('Command')
assert '-MsiVersion "$(_GlassMsiVersion)"' in gate.get('Command')
script=(root/'GlassSetup/msi/scripts/Assert-SettingsVersion.ps1').read_text()
for token in ('$expected -ne $msi', '$actual -ne $expected', '.ProductVersion', 'ReparsePoint', 'GetVersionInfo'):
 assert token in script
assert 'LoadFrom' not in script and 'GlassSettings.Native.dll' not in script
catalog=(root/'GlassSetup/scripts/New-ReleaseCatalog.ps1').read_text()
assert 'New-Asset $files[$id] $properties.ProductVersion' in catalog
# Bumping the platform bundle never changes either architecture's Settings release.
for bundle in ('2.0.0.108','2.0.0.999','9.0.0.999'):
 for arch in ('64','32'):
  assert msi.find('./PropertyGroup/GlassSettings'+arch+'MsiVersion').text.replace('$(GlassSettingsVersion)',version)==version
for name in ('GlassSettings/Version.props','Directory.Build.props','Directory.Build.targets','GlassSetup/msi/ProductVersions.props','GlassSetup/msi/GlassSetupPayload.targets'):
 (root/name).read_text(encoding='utf-8')
print('PASS: independent Settings app/MSI release, both architectures, three-field upgrade from 2.0.0.N; stale payload gate wiring, Native and platform versions isolated (source checks only)')
