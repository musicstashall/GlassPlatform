"""Static solution/target contract; does not run MSVC/WiX or prove MSI output."""
from pathlib import Path
import re
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[2]
solution = (root / "GlassProject.sln").read_text(encoding="utf-8")
projects = {}
for name, path, guid, body in re.findall(
    r'Project\("[^"\n]+"\) = "([^"]+)", "([^"]+)", "([^"]+)"\n(.*?)EndProject',
    solution, re.S,
):
    deps = set(re.findall(r'^\s*(\{[\dA-Fa-f-]+\}) = \{[\dA-Fa-f-]+\}\s*$', body, re.M))
    projects[name] = (path, guid.upper(), {dep.upper() for dep in deps})

shared = projects["GlassShared"][1]
ca_path, ca, deps = projects["SecureUxThemeCa"]
assert shared in deps
assert (root / ca_path.replace("\\", "/")).is_file()
for arch, native in (("x64", "x64"), ("x86", "Win32")):
    path, guid, deps = projects[f"SecureUxTheme-{arch}"]
    assert ca in deps, (arch, "MSI must wait for native CA")
    assert f"{guid}.Release|{arch}.Build.0 = Release|{arch}" in solution
    assert f"{ca}.Release|{arch}.Build.0 = Release|{native}" in solution
    other = "x86" if arch == "x64" else "x64"
    assert f"{guid}.Release|{other}.Build.0" not in solution

# No explicit solution dependency cycle may hide a project or block scheduling.
graph = {guid: deps for _, guid, deps in projects.values()}
visited, visiting = set(), set()
def visit(guid):
    assert guid not in visiting, ("Dependency cycle", guid)
    if guid in visited:
        return
    visiting.add(guid)
    for dep in graph.get(guid, set()):
        visit(dep)
    visiting.remove(guid)
    visited.add(guid)
for guid in graph:
    visit(guid)

targets = ET.parse(root / "GlassSetup/msi/GlassSetupPayload.targets").getroot()
ca_target = next(t for t in targets.findall("Target") if t.get("Name") == "GlassBuildSecureUxThemeCa")
call = ca_target.find("MSBuild")
assert call is not None
assert "'$(BuildingSolutionFile)' != 'true'" in call.get("Condition", "")
assert "'$(BuildingInsideVisualStudio)' != 'true'" in call.get("Condition", "")
assert ca_target.find("Error").get("Code") == "GSUT001"

# Both authoring templates must consume the CA produced in their own pass.
for name in ("SecureUxThemePackage.template.wxs", "SecureUxThemePackage-x86.template.wxs"):
    tree = ET.parse(root / "GlassSetup/msi" / name)
    ns = {"w": "http://wixtoolset.org/schemas/v4/wxs"}
    binary = tree.find("w:Package/w:Binary", ns)
    assert binary is not None and binary.get("SourceFile") == "$(var.SecureUxThemeCaDll)"
print("PASS static SUT build graph: Shared -> CA -> MSI, x64 then x86, no recursive solution build/cycles")
