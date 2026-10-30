"""Offline source/resource smoke test. Run from the repository: python GlassSetup/tests/verify_resources.py

This checks static invariants and resource integrity. It does NOT build MSVC/WiX,
execute MSI transactions, or replace Windows VM tests of cancellation and rollback.
"""
from pathlib import Path
import hashlib
import json
import re
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[1]
repo = root.parent

def read(path):
    return (root / path).read_text(encoding="utf-8")

def section(text, start, end):
    return text[text.index(start):text.index(end, text.index(start))]

header = read("res/resource.h")
assert read("res/resource.h.txt") == header, "resource.h.txt is out of sync"
ids = dict(re.findall(r"#define\s+(\w+)\s+(\d+)", header))
assert len(ids.values()) == len(set(ids.values())), "duplicate resource/control IDs"
rc = read("res/GlassSetup.rc")
unins_rc = read("GlassUnins/GlassUnins.rc")
setup = read("src/main.cpp")
engine = read("src/InstallEngine.cpp")
engine_h = read("src/InstallEngine.h")
rollback = read("src/InstallSupport/GlassInstallRollback.cpp")
msi = read("src/InstallSupport/GlassInstallMsi.cpp")
util = read("src/InstallSupport/GlassInstallUtil.cpp")
host = read("src/InstallSupport/GlassInstallHost.cpp")
watch = read("src/InstallSupport/GlassInstallWatch.cpp")
ui = read("res/slideshow.html")
theme = read("src/WizardTheme.cpp")
assets = read("src/WizardAssets.h")
unins = read("GlassUnins/main.cpp")

for name in ids:
    if name.startswith("IDS_"):
        assert len(re.findall(r"\b" + re.escape(name) + r"\b", rc)) == 2, f"{name}: RU/EN string missing"
for path in [*root.joinpath("src").rglob("*.cpp"), root / "GlassUnins/main.cpp"]:
    text = path.read_text(encoding="utf-8")
    used = set(re.findall(r"\b(?:IDS|IDR|IDI|IDC)_[A-Z0-9_]+\b", text)) - {"IDC_ARROW"}
    assert not used - ids.keys(), f"undefined IDs in {path}: {used - ids.keys()}"
    assert not re.search(r"\b(?:MessageBox[AW]?|TaskDialog(?:Indirect)?)\s*\(", text), f"system dialog in {path}"
    includes = [m.group(1).lower() for m in re.finditer(
        r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', text, re.MULTILINE)]
    assert "gdiplus.h" not in includes, f"{path}: use GdiPlusCompat.h instead"

# With WIN32_LEAN_AND_MEAN/NOMINMAX, MSVC's GDI+ headers need explicit COM
# declarations and min/max in their namespace. Match directives, not comments.
compat = read("src/GdiPlusCompat.h")
compat_includes = [m.group(1).lower() for m in re.finditer(
    r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', compat, re.MULTILINE)]
assert "gdiplus.h" in compat_includes
for dependency in ("windows.h", "objbase.h", "objidl.h", "algorithm"):
    assert dependency in compat_includes and compat_includes.index(dependency) < compat_includes.index("gdiplus.h"), dependency
assert compat.index("namespace Gdiplus") < compat.index("#include <gdiplus.h>")
assert "using std::min;" in compat and "using std::max;" in compat
for path in ("src/main.cpp", "src/WizardTheme.cpp", "GlassUnins/main.cpp"):
    assert '#include "GdiPlusCompat.h"' in read(path), path

# Original user-provided legal/banner resources are untouched.
for filename, expected in json.loads(read("tests/user-resource-hashes.json")).items():
    assert hashlib.sha256((root / "res" / filename).read_bytes()).hexdigest() == expected, filename

# The original image/font bytes moved out of base64 into binary resources.
expected_assets = json.loads(read("tests/asset-hashes.json"))
for filename, expected in expected_assets.items():
    path = root / "res/assets" / filename
    data = path.read_bytes()
    assert hashlib.sha256(data).hexdigest() == expected, filename
    if filename.endswith(".jpg"):
        assert data.startswith(b"\xff\xd8") and data.endswith(b"\xff\xd9"), filename
    elif filename.endswith(".png"):
        assert data.startswith(b"\x89PNG\r\n\x1a\n"), filename
    elif filename.endswith(".ttf"):
        assert data.startswith(b"\x00\x01\x00\x00"), filename

for resource_id in ("IDR_HTML", "IDR_WELCOME", "IDR_FONT_HANDWRITTEN", "IDR_SLIDE_START",
                    "IDR_SLIDE_TASKBAR", "IDR_SLIDE_NOTIFICATION", "IDR_SLIDE_CONTEXT_MENU",
                    "IDR_SLIDE_EXPLORER", "IDR_UNLOCKER_X64"):
    assert resource_id in ids and resource_id in rc and resource_id in unins_rc, resource_id
for filename in expected_assets:
    assert filename in rc and filename in unins_rc and filename in assets, filename
for token in ("data:image/jpeg;base64,", "data:image/png;base64,", "data:font/ttf;base64,"):
    assert token not in ui, f"binary data should be streamed separately: {token}"
assert "data:image/svg+xml;base64," in ui, "the local SVG mask is intentional"
assert "https://glassplatform.invalid/assets/" in ui
assert "connect-src 'none'" in setup and "connect-src 'none'" in unins
assert "WizardAssets::Resolve(url)" in setup and "WizardAssets::Resolve(url)" in unins
assert "allowed?200:403" in setup and "allowed?200:403" in unins

# Validate the manifests/project files as XML, including elevation and DPI.
ns = {"asmv3": "urn:schemas-microsoft-com:asm.v3",
      "ws": "http://schemas.microsoft.com/SMI/2016/WindowsSettings"}
for manifest in (root / "res/app.manifest", root / "GlassUnins/app.manifest"):
    tree = ET.parse(manifest)
    level = tree.find(".//asmv3:requestedExecutionLevel", ns)
    assert level is not None and level.get("level") == "requireAdministrator", manifest
    dpi = tree.find(".//ws:dpiAwareness", ns)
    assert dpi is not None and dpi.text == "PerMonitorV2", manifest
for project in (root / "GlassSetup.vcxproj", root / "GlassUnins/GlassUnins.vcxproj",
                root / "msi/PlatformPackage.template.wxs", root / "msi/OpenGlassPackage.template.wxs",
                root / "msi/GlassSettingsPackage.template.wxs", root / "msi/GlassSetupPayload.targets"):
    ET.parse(project)

project = read("GlassSetup.vcxproj")
unins_project = read("GlassUnins/GlassUnins.vcxproj")
for text in (project, unins_project):
    assert "WizardTheme.cpp" in text and "GdiPlusCompat.h" in text and "WebView2LoaderStatic.lib" in text
    assert "Detours.4.0.1" in text
assert "GdiPlusCompat.h" in read("GlassSetup.vcxproj.filters")
assert "WizardProgress.h" in project and "WizardProgress.h" in read("GlassSetup.vcxproj.filters")
assert "progress_tests.cpp" in project
assert 'ResourceCompile Include="GlassUnins.rc"' in unins_project
assert "GLASS_HAVE_UNLOCKER_X64" in project
assert "GlassSettings.Native.dll" in project and "GlassSettings.Native.dll" in unins_rc
assert (repo / "GlassProject.sln").is_file()

# Four user-visible products, individual product code routing and no legacy forced settings removal.
settings_wxs = read("msi/GlassSettingsPackage.template.wxs")
platform_wxs = read("msi/PlatformPackage.template.wxs")
og_wxs = read("msi/OpenGlassPackage.template.wxs")
payload = read("msi/GlassSetupPayload.targets")
assert "ARPSYSTEMCOMPONENT" not in settings_wxs
assert "GlassProductIcon" in settings_wxs and "ARPPRODUCTICON" in settings_wxs
assert "GlassSettings\\unins000.exe" in payload and "GlassSettings\\unins000.dat" in payload
assert "SETTINGS_PRODUCT_CODE=" not in payload
assert "GlassInstallPathValueName" in platform_wxs and "InstallPathX64" in payload and "InstallPathX86" in payload
assert 'Root="HKLM"' in og_wxs and 'Key="Software\\StanislavKa\\GlassPlatform"' in og_wxs
assert "--product" in msi and "--silent" in msi
for suffix in ("5071", "5072", "5073", "5074", "5075"):
    assert suffix in read("src/InstallSupport/GlassInstallMsi.h"), suffix
assert "NormalizeInstall" in engine and "NormalizeRemoval" in unins
assert "selection_.platform64=os64" in setup
assert "IDS_COMP_GS_WARNING" in setup and "HKCU\\\\Software\\\\StanislavKa\\\\GlassPlatform" in rc
assert "StartMenuShortcutExists" in util
assert "QueryPlatformInstallPath" in util and "QueryOpenGlassInstallPath" in util

# Detours remain in the button drawing path. The button subclass only forwards to comctl32.
assert "DetourAttach(&g_originalDrawThemeBackground" in theme
assert "DetourAttach(&g_originalDrawThemeText" in theme
hook = section(theme, "HRESULT WINAPI HookDrawThemeText(", "HRESULT WINAPI HookDrawThemeBackground(")
assert "DrawThemeTextEx(" in hook and "DTT_TEXTCOLOR" in hook
assert "return original(" in hook, "non-wizard controls must preserve default behavior"
assert hook.index("return original(") < hook.index("DrawThemeTextEx(")
assert "HookDrawTextExW" not in theme + setup and "DetourEnumerateImportsEx" not in theme + setup
scope = section(theme, "LRESULT CALLBACK ButtonScopeSubclassProc(", "bool IsActiveFocusedButton(")
assert "DefSubclassProc" in scope and not re.search(r"case\s+WM_(?:PAINT|PRINTCLIENT)\b", scope)
assert "EnsureAndDraw" in theme and "Scale(132)" in setup and "Scale(184)" in setup

# One-document UI, static initialization and user-driven cancel controls.
for mode in ("welcome", "checking", "update", "conflicts", "downgrade", "version-invalid", "license",
             "components", "installing", "unlock-modules", "unlock-processes", "confirm-cancel",
             "cancelling", "cancelled", "launching", "complete", "error", "uninstall-select", "uninstalling"):
    assert mode in ui, mode
for node in ("components-panel", "confirm-panel", "cancelling-panel", "cancelled-panel", "launch-panel", "unlock-panel"):
    assert f'id="{node}"' in ui, node
assert "prefers-reduced-motion:reduce" in ui
assert "postMessage('cancel')" in ui and 'message==L"cancel"' in setup and 'message==L"cancel"' in unins
assert 'Page::Components' in setup and 'installEngine_.Start' in setup
assert "PauseForCancelDecision" in engine and "decisionGate_" in engine
assert "Page::ConfirmCancel" in setup and "Page::Cancelling" in setup and "Page::Cancelled" in setup
# Both the pending MSI cancellation marquee and determinate rollback use the
# same right-to-left progress policy; the standalone C++ test checks geometry.
progress_draw = section(setup, "case WM_DRAWITEM:", "case WM_CTLCOLORBTN:")
assert "WizardProgress::DeterminateSpan" in progress_draw and "WizardProgress::MarqueePhase" in progress_draw
progress_tick = section(setup, "void Tick() {", "void RequestClose() {")
assert "WizardProgress::ResolveMode" in progress_tick and "BeginRollbackVisual(now)" in progress_tick
assert "page_==Page::Cancelling" in progress_tick and "WizardProgress::Percent" in progress_tick
rollback_ui = section(engine, "bool InstallEngine::PerformRollback()", "bool InstallEngine::PurgeRemnants()")
assert "WizardProgress::RollbackRemaining(units)" in rollback_ui
for start, end in (("bool InstallEngine::PerformRollback()", "bool InstallEngine::UndoCleanInstall()"),
                   ("bool InstallEngine::UndoCleanInstall()", "bool InstallEngine::PurgeRemnants()")):
    phase = section(engine, start, end)
    assert phase.index('SetStatus(labels_.rollback.empty() ? L"…" : labels_.rollback, -1)') < phase.index("progressReverse_.store(true")
confirm = section(setup, "case IDC_NEXT:", "case IDCANCEL:")
assert "BeginRollbackVisual(GetTickCount64())" in confirm and "SetPage(Page::Cancelling)" in confirm
assert "simulateRollback_" in confirm and "IDS_SIMULATION_ROLLBACK" in setup and "IDS_SIMULATION_CANCELLED" in setup
assert "WM_CLOSE" in setup and "SC_CLOSE" in setup and "SetSystemCloseEnabled" in setup
assert "cancelRequested_.store(true" in engine and "return IDCANCEL" in msi and "cancellationSent" in msi
assert engine.index("GlassInstallRollback::NoteInstalled(rollbackSession_,productCode.c_str())") < engine.index("if (exitCode == ERROR_INSTALL_USEREXIT")
assert "CopyFileExW" in rollback and "ERROR_CANCELLED" in rollback
assert "Snapshot enumeration interrupted" in rollback and "Snapshot source unavailable" in rollback
assert "SnapshotCopyProgress" in msi and "CopyFileExW" in msi
assert "NoteModified" in rollback and "RestoreRegistryValues" in rollback
assert "kToggleExplorerClsidBraced" in rollback and "kOpenConfigExplorerClsidBraced" in rollback
assert "x64TaskCreatedBySetup_" in engine and "ogServiceCreatedBySetup_" in engine
assert "/SC ONLOGON /RL HIGHEST /F" not in host, "never overwrite a pre-existing task"
assert "refusing to replace existing OpenGlassHost service" in host
assert "compare_exchange_strong(expected,OperationState::Removing" in unins
assert "compare_exchange_strong(expected,OperationState::CancelRequested" in unins
assert "cached MSI unavailable" in rollback
assert "preserveSnapshot_=true" in engine and "RollbackSucceeded" in engine_h
undo = section(engine, "bool InstallEngine::UndoCleanInstall()", "bool InstallEngine::PurgeRemnants()")
assert "DeleteTree" not in undo, "never recursively delete unrelated files on clean-install cancellation"
assert "DirBytes(state.backupX64)" in rollback and "IsNestedSubPath" in rollback
assert "receivedFrame" in watch and "FILE_FLAG_FIRST_PIPE_INSTANCE" in watch
assert "Page::Launching" in setup and "kMinLaunchMs = 5000" in engine
assert "--demo-tour" in setup and "debugDemoPending_ = false" in setup
assert "MsiEnableLogW(0, nullptr, 0)" in engine
assert "CERT_CHAIN_POLICY_MICROSOFT_ROOT" in setup and "httpStatus==200" in setup
assert "IsAllowedReleaseUrl" in setup and "IShellDispatch2" in setup
assert "CreateSecureTempDirectory" in util
assert 'CreateSecureTempDirectory(L"GlassSetupLogs"' in read("src/InstallLog.cpp")
assert 'CreateSecureTempDirectory(L"GlassUninsLogs"' in read("src/InstallSupport/GlassInstallLog.cpp")
assert "SecureLogDirectory()" in unins and 'GetEnvironmentVariableW(L"ProgramW6432"' not in setup + unins + engine
native_helper = section(util, "std::wstring NativeDllForArch(", "bool QueryPlatformInstallPath(")
assert "if (x64)" in native_helper and "return EnsureExtractedUnlockerX64();" in native_helper
assert native_helper.index("return EnsureExtractedUnlockerX64();") < native_helper.index("QueryPlatformInstallPath(")
shell_ops = (repo / "GlassSettings/Native/ShellExtensionOps.cpp").read_text(encoding="utf-8")
shell_sparse = (repo / "GlassSettings/Native/ShellSparsePackage.cpp").read_text(encoding="utf-8")
pack = (repo / "GlassShellExt/Package/BuildSparsePackage.ps1").read_text(encoding="utf-8")
assert 'L"/register /setup"' in util and "0x47505245" in util and "0x47505245" in shell_ops
assert "provisionDevCertificate && !ShellDevCertificate::Ensure" in shell_sparse
assert "provisionDevCertificate && ResolveSparseSdkTools" in shell_sparse
assert "Export-PfxCertificate" not in pack and "$cert.RawData" in pack
assert 'm.type===\'init\'||m.type===\'state\'' in ui

# Check the embedded JavaScript syntax when Node is available.
node = shutil.which("node")
if node:
    script = re.search(r"<script>([\s\S]*?)</script>", ui)
    assert script, "embedded JavaScript missing"
    with tempfile.TemporaryDirectory() as temp:
        script_path = Path(temp) / "wizard.js"
        script_path.write_text(script.group(1), encoding="utf-8")
        subprocess.run([node, "--check", str(script_path)], check=True)

print("PASS: RU/EN resources, offline assets, manifests, product policy, theme detours, UI/IPC, cancellation and rollback source invariants.")
print("NOTE: This is a source smoke test, not an MSVC/WiX build or an actual Windows install/uninstall/rollback test.")
