"""Settings uses the shared bundle version. Source/XML checks, not MSBuild/WiX."""
from pathlib import Path
import xml.etree.ElementTree as ET
root=Path(__file__).resolve().parents[2]
def xml(name):return ET.parse(root/name).getroot()
assert not (root/'GlassSettings/Version.props').exists()
props=xml('Directory.Build.props')
assert not any('Version.props' in n.get('Project','') for n in props.findall('Import'))
assert props.find('./PropertyGroup/GlassProductVersion').text=='2.0.0.0'
settings=next(n for n in props.findall('PropertyGroup') if n.get('Condition')=="'$(MSBuildProjectName)' == 'GlassSettings'")
for field in ('Version','AssemblyVersion','InformationalVersion'):
 assert settings.find(field) is None
targets=xml('Directory.Build.targets')
cs=targets.find("Target[@Name='GlassWriteCSharpFileVersion']")
file_versions=cs.findall('./PropertyGroup/GlassFileVersionDot')
assert len(file_versions)==1
assert file_versions[0].get('Condition') is None
assert file_versions[0].text=='$(GlassProductVersionBase3).$(GlassBuildNumberNext)'
product=cs.findall('./PropertyGroup/_GlassCSharpProductVersion')
assert len(product)==1 and product[0].text=='$(GlassBundleVersionEffective)'
assert 'GlassSettingsVersion' not in ET.tostring(cs, encoding='unicode')
native=targets.find("Target[@Name='GlassWriteVersionHeader']")
assert native.find('./PropertyGroup/GlassFileVersionDot').text=='$(GlassProductVersionBase3).$(GlassBuildNumberNext)'
msi=xml('GlassSetup/msi/ProductVersions.props')
assert not any('Version.props' in n.get('Project','') for n in msi.findall('Import'))
for arch in ('64','32'):
 node=msi.find('./PropertyGroup/GlassSettings'+arch+'MsiVersion')
 assert node.get('Condition')=="'$(GlassSettings"+arch+"MsiVersion)' == ''"
 assert node.text in (None,'')
payload=xml('GlassSetup/msi/GlassSetupPayload.targets')
meta=payload.find("Target[@Name='GlassWriteProductMetadata']")
gate=next(n for n in meta.findall('Exec') if 'Assert-SettingsVersion.ps1' in n.get('Command',''))
assert 'GlassSettings-x64' in gate.get('Condition') and 'GlassSettings-x86' in gate.get('Condition')
assert 'GlassSettings\\GlassSettings.exe' in gate.get('Command')
assert '-ExpectedVersion "$(GlassBundleVersionEffective)"' in gate.get('Command')
assert '-MsiVersion "$(_GlassMsiVersion)"' in gate.get('Command')
assert 'GlassSettingsVersion' not in gate.get('Command')
script=(root/'GlassSetup/msi/scripts/Assert-SettingsVersion.ps1').read_text(encoding='utf-8')
for token in ('$expected -ne $msi', '$actual -ne $expected', '.ProductVersion', 'ReparsePoint', 'GetVersionInfo'):
 assert token in script
assert 'LoadFrom' not in script and 'GlassSettings.Native.dll' not in script
catalog=(root/'GlassSetup/scripts/New-ReleaseCatalog.ps1').read_text(encoding='utf-8')
assert 'New-Asset $files[$id] $properties.ProductVersion' in catalog
for name in ('Directory.Build.props','Directory.Build.targets','GlassSetup/msi/ProductVersions.props','GlassSetup/msi/GlassSetupPayload.targets'):
 text=(root/name).read_text(encoding='utf-8')
 assert 'GlassSettingsVersion' not in text
print('PASS: Settings app and both MSI use the bundle version; FileVersion keeps the shared 2.0.0 prefix and its own counter')
