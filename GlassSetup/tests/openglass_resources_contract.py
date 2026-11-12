"""Source/build graph checks only; Windows WiX/PowerShell execution is separate."""
from pathlib import Path
import xml.etree.ElementTree as ET
root=Path(__file__).resolve().parents[2]
msi=root/'GlassSetup/msi'
project=ET.parse(msi/'OpenGlassResources-x64.wixproj')
wxs=ET.parse(msi/'OpenGlassResourcesPackage.wxs')
ns={'w':'http://wixtoolset.org/schemas/v4/wxs'}
package=wxs.find('w:Package',ns)
assert package is not None and package.get('Name')=='OpenGlass Resources'
assert package.get('UpgradeCode')!='B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5030'
assert package.find("w:Launch",ns) is not None
assert '{D3D1BC7D-5E24-4B33-9383-7934271A3B05}_is1' in (msi/'OpenGlassResourcesPackage.wxs').read_text()
for forbidden in ('CustomAction','ServiceInstall','ServiceControl','RemoveFile','RemoveFolder'):
 assert not package.findall('.//w:'+forbidden,ns)
assert not package.findall('.//w:File',ns) # all graphics enter through the validated harvest
stage=(msi/'scripts/Stage-OpenGlassResources.ps1').read_text()
assert '$allowed -notcontains $file.Extension.ToLowerInvariant()' in stage
assert 'ReparsePoint' in stage and 'Refusing to produce an empty' in stage
assert "'.exe'" not in stage and "'.dll'" not in stage
solution=(root/'GlassProject.sln').read_text()
assert 'OpenGlassResources-x64.wixproj' in solution
paths=(root/'GlassSettings/Core/Platform/PlatformPaths.cs').read_text()
assert paths.index('var upstream = ReadUpstreamOpenGlassInstallPath()') < paths.index('var installDir = ReadBlackWindowManagerInstallPath()')
policy=(root/'GlassSettings/Core/Platform/OpenGlassUiPolicy.cs').read_text()
assert 'IsBundledWithApplication' not in policy and 'OrchestrationTargetsBundledPackage' not in policy
assert 'PlatformPaths.OpenGlassDllPath' in policy
print('PASS: graphics-only Resources MSI build/dependency contracts; location-independent Settings discovery/policy source wiring')
