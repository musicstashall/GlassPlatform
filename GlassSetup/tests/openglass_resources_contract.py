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
assert 'UPSTREAMOPENGLASSROOT AND OPENGLASSDLL AND NOT UPSTREAMOPENGLASSALTROOT' in package.find('w:Launch',ns).get('Condition')
assert 'UPSTREAMOPENGLASSALTROOT AND OPENGLASSALTDLL AND NOT UPSTREAMOPENGLASSROOT' in package.find('w:Launch',ns).get('Condition')
assert '{D3D1BC7D-5E24-4B33-9383-7934271A3B05}}_is1' in (msi/'OpenGlassResourcesPackage.wxs').read_text()
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
# Exercise the actual staging allowlist against atlas sidecars and forbidden
# payloads (source contract only, not a PowerShell execution test).
import re
allowed = set(re.findall(r"'([^']+)'", re.search(r'\$allowed = @\(([^\n]+)\)', stage).group(1)))
for name in ('AtlasTheme/Aero Glow Square 10.png.Layout', 'AtlasTheme/Aero.png.layout',
             'AtlasTheme/Aero.PNG', 'GlassReflection/Reflection.png'):
 assert Path(name).suffix.lower() in allowed, name
for name in ('OpenGlass.dll', 'OpenGlass.exe', 'setup.ps1', 'run.cmd', 'run.bat',
             'run.vbs', 'payload.msi', 'Aero.png.Layout.exe'):
 assert Path(name).suffix.lower() not in allowed, name
solution=(root/'GlassProject.sln').read_text()
assert 'OpenGlassResources-x64.wixproj' in solution
# Build the x64 graphics MSI in the x64 release phase, not again in x86.
# ActiveCfg only maps the project for IDE use; Build.0 selects it for build.
resources_guid = '{66B912C7-3BF1-4EC3-8EA2-406DB4E8BB19}'
for mapping in ('ActiveCfg', 'Build.0'):
 line = f'{resources_guid}.Release|x64.{mapping} = Release|x64'
 assert solution.count(line) == 1, f'Missing/duplicate Resources solution mapping: {line}'
assert f'{resources_guid}.Release|x86.ActiveCfg = Release|x64' in solution
assert not any(resources_guid in line and '.Release|x86.Build.0' in line
               for line in solution.splitlines())
assert not any(resources_guid in line and '.Debug|' in line and '.Build.0' in line
               for line in solution.splitlines())
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

# Independently publishable/acquirable identity, never the retired OG MSI.
catalog=(root/'GlassSetup/src/InstallSupport/GlassPackageCatalog.h').read_text()
assert '"openglassResources"' in catalog and '"OpenGlassResources-x64.msi"' in catalog
acquisition=(root/'GlassSetup/src/InstallSupport/GlassPackageAcquisition.cpp').read_text()
assert 'kProductOpenGlassResourcesX64,L"{A5571574-00E7-43AD-81DF-B65D1AE07699}",L"x64"' in acquisition
assert acquisition.count('!NativeProductSupported(product)')==2
assert 'PROCESSOR_ARCHITECTURE_AMD64' in acquisition
assert 'Prepared(GlassPackageCatalog::PreparedIndex(product))' in acquisition
codes=(root/'GlassSetup/src/InstallSupport/GlassInstallMsi.h').read_text()
assert '{'+package.get('ProductCode')+'}' in codes
publish=(root/'GlassSetup/scripts/New-ReleaseCatalog.ps1').read_text()
assert "openglassResources='OpenGlassResources-x64.msi'" in publish
assert package.get('UpgradeCode') in publish and package.get('ProductCode') in publish
print('PASS: Resources catalog/publisher/acquisition fixed identity, AMD64 guard, non-colliding prepared slot; UI/engine and combined fresh OG protocol connected')

inspection=(root/'GlassSetup/src/InstallSupport/GlassProductInspect.cpp').read_text()
assert '{ GlassInstallMsi::kProductOpenGlassResourcesX64, nullptr, &report.openGlassResources }' in inspection
assert '{GlassPackageCatalog::Product::OpenGlassResources,&report.openGlassResources,GlassInstallMsi::kProductOpenGlassResourcesX64,nullptr}' in inspection
assert 'if(!in.os64 && product.report==&report.openGlassResources)continue;' in inspection
assert 'if(!in.os64 && job.product==GlassPackageCatalog::Product::OpenGlassResources)continue;' in inspection
# Graphics uses the installed MSI baseline, never the external Resources directory,
# and never PE payload version/service/task checks belonging to OG itself.
assert 'InstalledBaselineMatches(reference,installedCode)' in inspection
assert 'CheckProductFiles(reference,installedCode,signerSha1,*job.rep,job.product==GlassPackageCatalog::Product::OpenGlass)' in inspection
print('PASS: separate x64 graphics inventory/offer/installed-baseline lane; exact two upstream prerequisite identities, no ambiguous roots')

util=(root/'GlassSetup/src/InstallSupport/GlassInstallUtil.cpp').read_text()
fixed=util[util.index('bool ResolveOpenGlassResourcesRoot('):util.index('bool ResolveFixedPlatformRoots(')]
assert 'ResolveDefaultPlatformRoot(true,os64,platform)' in fixed
assert 'ReadOpenGlassInstallPath' not in fixed and 'IsSafeInstallPath(target)' in fixed
rollback=(root/'GlassSetup/src/InstallSupport/GlassInstallRollback.cpp').read_text()
assert '{kProductOpenGlassResourcesX64,64}' in rollback
assert 'TrySnapshotProduct(state, GlassInstallMsi::kProductOpenGlassResourcesX64, state.resourcesDir, cancelled)' in rollback
assert 'CopyTree(state.backupResources,state.resourcesDir' in rollback
assert package.find("w:Property[@Id='GLASS_RESTORE_NO_CAB']",ns) is not None
assert package.find('w:InstallExecuteSequence/w:InstallFiles',ns).get('Condition')=='GLASS_RESTORE_NO_CAB <> 1'
engine=(root/'GlassSetup/src/InstallEngine.cpp').read_text()
assert '{Product::OpenGlassResources,os64&&selection_.openGlassResources}' in engine
assert 'steps.push_back({P::Product::OpenGlassResources,L"OpenGlassResources-x64.msi",root,L"OpenGlass Resources"})' in engine
assert 'case P::Product::OpenGlassResources:current=GlassInstallMsi::kProductOpenGlassResourcesX64' in engine
assert 'constexpr unsigned sourceRows[]={0,1,2,5,4,6}' in engine
print('PASS: Resources engine acquire/apply/remove, independent fixed-root snapshot/restore and cache wiring; separate post-OG graphics transaction wired')
