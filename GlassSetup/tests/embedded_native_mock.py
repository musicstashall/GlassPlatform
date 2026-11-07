#!/usr/bin/env python3
"""Regression: Setup/Unins no longer embed or extract MSI/Native. No Windows execution."""
from pathlib import Path
import xml.etree.ElementTree as ET
root=Path(__file__).resolve().parents[1]
for path in ['res/GlassSetup.rc','GlassUnins/GlassUnins.rc']:
 s=(root/path).read_text()
 assert 'IDR_UNLOCKER_' not in s and 'IDR_MSI_' not in s and 'GlassSettings.Native.dll' not in s
for path in ['src/InstallEngine.cpp','src/InstallSupport/GlassInstallUtil.cpp','GlassUnins/main.cpp']:
 s=(root/path).read_text()
 for obsolete in ['ConfigureEmbeddedUnlocker','CleanupEmbeddedUnlocker','EnsureExtractedUnlocker','EmbeddedNativeDll','VerifyEmbeddedNativePayload','IDR_UNLOCKER_','IDR_MSI_']:
  assert obsolete not in s,(path,obsolete)
s=(root/'src/InstallSupport/GlassProductInspect.cpp').read_text()
assert 'GlassPackageAcquisition::Prepare' in s and 'GlassPackageAcquisition::CreateWorkspace' in s
assert 'ExtractResourceToFile' not in s and 'GetTempPath' not in s
assert 'InstalledBaselineMatches(reference,installedCode)' in s and 'INSTALLPROPERTY_PACKAGECODE' in s
assert 'report.settings64' in s and 'report.settings32' in s
cache=(root/'src/InstallSupport/GlassMaintenanceCache.h').read_text()
assert 'nativeX86Resource' not in cache and 'GlassSettings.Native.x86.dll' not in cache
project=(root/'GlassSetup.vcxproj').read_text()
assert 'GLASS_EMBED_MSI' not in project and 'GlassSettings.Native.vcxproj' not in project
assert 'GLASS_EMBED_MAINTENANCE' in project
for path in ['GlassSetup.vcxproj','GlassUnins/GlassUnins.vcxproj']:ET.parse(root/path)
print('PASS: no MSI/Native resources, extractors, mandatory bundle verification, Native cache publication or reverse Native project dependency; verified external inspector and retained maintenance EXE')
