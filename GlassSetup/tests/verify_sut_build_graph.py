"""Original vendor MSI is resolved at runtime, never built or embedded by Glass."""
from pathlib import Path
import re
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[2]
solution = (root / "GlassProject.sln").read_text(encoding="utf-8")
retired = ("{0CD1A2BE-2C39-4404-A026-34642B517428}",
           "{81FE634A-DB11-4759-B76D-D1119AC2CF13}",
           "{A1B2C3D4-E5F6-7890-ABCD-EF1234567890}")
assert "SecureUxTheme" not in solution
for guid in retired:
    assert guid not in solution
projects = {}
for name, path, guid, body in re.findall(
    r'Project\("[^"\n]+"\) = "([^"]+)", "([^"]+)", "([^"]+)"\n(.*?)EndProject', solution, re.S
):
    deps = set(re.findall(r'^\s*(\{[\dA-Fa-f-]+\}) = \{[\dA-Fa-f-]+\}\s*$', body, re.M))
    assert (root / path.replace("\\", "/")).exists(), path
    projects[guid] = deps
for guid, deps in projects.items():
    assert deps <= projects.keys(), (guid, deps - projects.keys())
def visit(guid, stack):
    assert guid not in stack, (guid, stack)
    for dependency in projects[guid]:
        visit(dependency, stack | {guid})
for guid in projects:
    visit(guid, set())
for path in ("GlassSetup/msi/GlassSetupPayload.targets", "GlassSetup/msi/Directory.Build.props",
             "GlassSetup/GlassSetup.vcxproj"):
    ET.parse(root / path)
    text = (root / path).read_text(encoding="utf-8")
    assert "SecureUxThemeCa" not in text and "SecureUxTheme-x64" not in text
project = (root / "GlassSetup/GlassSetup.vcxproj").read_text(encoding="utf-8")
assert 'ClCompile Include="src\\InstallSupport\\GlassSecureUxPackage.cpp"' in project
assert 'Release|Win32' in project
rc = (root / "GlassSetup/res/GlassSetup.rc").read_text(encoding="utf-8")
assert "IDR_MSI_SUT" not in rc
print("PASS: original MSI runtime provider; no SUT repack/CA build graph, dangling projects or dependency cycles")
