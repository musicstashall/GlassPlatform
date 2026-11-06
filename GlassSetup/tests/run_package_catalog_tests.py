"""Production portable headers, plus build publication graph guards. No platform operations."""
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
publish = targets.find("Target[@Name='GlassPublishLocalMsi']")
assert publish is not None and publish.get('AfterTargets') == 'Build'
assert "$(BuildProject)' != 'false'" in publish.get('Condition')
copy = publish.find('Copy')
assert copy.get('SourceFiles') == '$(TargetPath)'
assert "'Build', 'GlassSetup', 'Release'" in publish.find('PropertyGroup/GlassLocalPackageDir').text
for name in ('Glass-x64', 'Glass-x86', 'GlassSettings-x64', 'GlassSettings-x86', 'OpenGlass-x64'):
    project = ET.parse(root / f'GlassSetup/msi/{name}.wixproj').getroot()
    assert any(item.get('Project') == 'GlassSetupPayload.targets' for item in project.findall('Import'))
    assert project.find('PropertyGroup/OutputName').text == name
# Transitional build must still feed the current runtime until AcquirePackages is wired.
assert 'GLASS_EMBED_MSI' in (root / 'GlassSetup/GlassSetup.vcxproj').read_text(encoding='utf-8')
print('PASS: five MSI build outputs published beside EXE; existing embedded inputs preserved')
