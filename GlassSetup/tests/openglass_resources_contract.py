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
# Prerequisite location must never become the graphics destination.
upstream=package.find("w:Property[@Id='UPSTREAMOPENGLASSROOT']",ns)
assert upstream is not None and upstream.find('w:RegistrySearch',ns) is not None
assert package.find("w:Property[@Id='OPENGLASSROOT']",ns) is None
assert package.find(".//w:DirectorySearch",ns).get('Path')=='[UPSTREAMOPENGLASSROOT]'
assert 'UPSTREAMOPENGLASSROOT AND OPENGLASSDLL' in package.find('w:Launch',ns).get('Condition')
native=package.find("w:StandardDirectory[@Id='ProgramFiles64Folder']",ns)
platform=native.find("w:Directory[@Name='GlassPlatform']",ns)
og=platform.find("w:Directory[@Name='OpenGlass']",ns)
resources=og.find("w:Directory[@Id='INSTALLDIR']",ns)
assert resources is not None and resources.get('Name')=='Resources'
assert not package.findall(".//w:Directory[@Id='UPSTREAMOPENGLASSROOT']",ns)
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
resource_policy=paths[paths.index('public static string? OpenGlassResourcesRoot'):paths.index('/// <summary>Резолв модуля платформы')]
for forbidden in ('AddRoot(OpenGlassDir)', 'ReadUpstreamOpenGlassInstallPath()', 'Path.Combine(PlatformRoot', 'Path.Combine(AppBase'):
 assert forbidden not in resource_policy
assert 'OpenGlassResourcesRoot is { } root ? new[] { root } : Array.Empty<string>()' in resource_policy
assert 'RegistryView.Registry64' in resource_policy and 'RegistryValueOptions.DoNotExpandEnvironmentNames' in resource_policy
assert 'RegistryValueKind.String' in resource_policy and 'Path.IsPathFullyQualified(programFiles)' in resource_policy
assert 'Path.Combine(Path.GetFullPath(programFiles), "GlassPlatform", "OpenGlass")' in resource_policy
assert 'Path.Combine(root, "Resources")' in resource_policy
catalog=(root/'GlassSettings/Services/ThemeImageCatalog.cs').read_text()
assert 'new(PlatformPaths.GetResourceSearchRoots())' in catalog
for kind in ('AtlasTheme','GlassReflection','ThemeMaterial'):
 assert f'Path.Combine("Resources", "{kind}")' in catalog
policy=(root/'GlassSettings/Core/Platform/OpenGlassUiPolicy.cs').read_text()
assert 'IsBundledWithApplication' not in policy and 'OrchestrationTargetsBundledPackage' not in policy
assert 'PlatformPaths.OpenGlassDllPath' in policy
print('PASS: graphics-only Resources MSI build/dependency contracts; fixed native GlassPlatform graphics destination independent of upstream OG; Settings catalog source wiring')
