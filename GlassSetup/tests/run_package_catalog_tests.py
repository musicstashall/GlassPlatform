"""Production portable headers, plus direct MSI output graph guards. No platform operations."""
from pathlib import Path
import shutil
import sys
import subprocess
import tempfile
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[2]
compiler = next((shutil.which(c) for c in ('cl', 'clang++', 'g++') if shutil.which(c)), None)
if not compiler:
    raise SystemExit('A C++17 compiler is required')
with tempfile.TemporaryDirectory(prefix='GlassPackageTests-') as temp:
    exe = Path(temp) / 'test.exe'
    source = root / 'GlassSetup/tests/package_catalog_tests.cpp'
    if Path(compiler).stem.lower() == 'cl':
        command = [compiler, '/nologo', '/std:c++17', '/EHsc', '/utf-8', str(source), '/Fe:' + str(exe)]
    else:
        command = [compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic', str(source), '-o', str(exe)]
    if '--sanitize' in sys.argv:
        if Path(compiler).stem.lower() == 'cl':
            raise SystemExit('This sanitizer mode requires clang++/g++')
        command[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie']
    subprocess.run(command, cwd=temp, check=True)
    subprocess.run([str(exe)], cwd=temp, check=True)

targets = ET.parse(root / 'GlassSetup/msi/GlassSetupPayload.targets').getroot()
assert targets.find("Target[@Name='GlassPublishLocalMsi']") is None
assert not any(copy.get('SourceFiles') == '$(TargetPath)' for copy in targets.iter('Copy'))
# Check BOTH early props and late SDK overrides; checking only the copy destination
# hid the old double-output build. Keep Clean/obj scope private to each project.
for build_file in ('GlassSetup/msi/Directory.Build.props','Directory.Build.targets'):
    tree=ET.parse(root/build_file).getroot()
    groups=[g for g in tree.findall('PropertyGroup') if g.find('BaseOutputPath') is not None and 'GlassWixLockedPlatform' in g.get('Condition','')]
    assert len(groups)==1,build_file
    group=groups[0]
    assert "'Build', 'GlassSetup', '$(Configuration)'" in group.findtext('OutputPath')
    assert "'Cache', '$(MSBuildProjectName)', 'Output'" in group.findtext('BaseOutputPath')
    assert group.findtext('AppendPlatformToOutputPath')=='false'
    assert "'msi'" not in group.findtext('OutputPath')
late=ET.parse(root/'Directory.Build.targets').getroot()
assert any(g.findtext('TargetPath')=='$(OutputPath)$(OutputName).msi' for g in late.findall('PropertyGroup'))
assert any(g.findtext('CleanDependsOn')=='GlassWixSkipForeignPlatform' for g in late.findall('PropertyGroup'))
assert targets.find("Target[@Name='GlassVerifyBuiltMsi']") is not None
signing=ET.parse(root/'Shared/GlassSign.targets').getroot()
sign_after=signing.find("Target[@Name='GlassSignAfterBuild']")
assert sign_after is not None and sign_after.get('AfterTargets')=='Build'
assert '$(TargetPath)' in (root/'Shared/GlassSign.targets').read_text(encoding='utf-8')
output_names=set()
for name in ('Glass-x64', 'Glass-x86', 'GlassSettings-x64', 'GlassSettings-x86'):
    project = ET.parse(root / f'GlassSetup/msi/{name}.wixproj').getroot()
    assert any(item.get('Project') == 'GlassSetupPayload.targets' for item in project.findall('Import'))
    output_name=project.find('PropertyGroup/OutputName').text
    assert output_name == name and output_name not in output_names
    output_names.add(output_name)
# Production uses external acquisition; no stale transitional embedded runtime.
assert 'GLASS_EMBED_MSI' not in (root / 'GlassSetup/GlassSetup.vcxproj').read_text(encoding='utf-8')
engine=(root / 'GlassSetup/src/InstallEngine.cpp').read_text(encoding='utf-8')
assert 'GlassPackageAcquisition::Prepare(' in engine
cache=(root / 'GlassSetup/src/InstallSupport/GlassMaintenanceCache.h').read_text(encoding='utf-8')
assert 'PublishedDigestMatches(file)' in cache and 'InstalledCacheDigest(file,installed)' in cache
assert 'blobs[i].verifiedSource || PublishedDigestMatches(source)' in cache
print('PASS: four Glass MSI outputs beside EXE; external runtime acquisition; protected distribution and installed-cache digest binding')
