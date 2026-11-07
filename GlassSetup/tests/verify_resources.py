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
rc = read("res/GlassSetup.rc") + "\n" + read("res/ProductStrings.rcinc")
unins_rc = read("GlassUnins/GlassUnins.rc")
setup = read("src/main.cpp")
# Project items do not import C++ declarations. Each UI translation unit must
# include the policy headers itself, before using their namespaces.
for source in (setup, read("GlassUnins/main.cpp")):
    for policy_header, namespace in (("GlassProductPlan.h", "GlassProductPlan::"),
                                     ("GlassProductUiPolicy.h", "GlassProductUiPolicy::")):
        directive = '#include "' + policy_header + '"'
        assert directive in source, f"Missing direct include: {policy_header}"
        assert source.index(directive) < source.index(namespace), policy_header

engine = read("src/InstallEngine.cpp")
engine_h = read("src/InstallEngine.h")
rollback = read("src/InstallSupport/GlassInstallRollback.cpp")
msi = read("src/InstallSupport/GlassInstallMsi.cpp")
util = read("src/InstallSupport/GlassInstallUtil.cpp")
host = read("src/InstallSupport/GlassInstallHost.cpp")
removal = read("src/InstallSupport/GlassInstallRemoval.cpp")
removal_order = read("src/InstallSupport/GlassRemovalOrder.h")
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
                    "IDR_SLIDE_EXPLORER"):
    assert resource_id in ids and resource_id in rc and resource_id in unins_rc, resource_id
assert "IDR_UNLOCKER_X86" not in rc and "IDR_UNLOCKER_X64" not in rc
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
                root / "msi/GlassSettingsPackage.template.wxs",
                root / "msi/GlassSetupPayload.targets"):
    ET.parse(project)

project = read("GlassSetup.vcxproj")
unins_project = read("GlassUnins/GlassUnins.vcxproj")
for text in (project, unins_project):
    assert "WizardTheme.cpp" in text and "GdiPlusCompat.h" in text and "WebView2LoaderStatic.lib" in text
    assert "Detours.4.0.1" in text
assert "GdiPlusCompat.h" in read("GlassSetup.vcxproj.filters")
assert "WizardProgress.h" in project and "WizardProgress.h" in read("GlassSetup.vcxproj.filters")
assert "WizardUnlockState.h" in project and "WizardUnlockState.h" in read("GlassSetup.vcxproj.filters")
assert "progress_tests.cpp" in project and "removal_order_tests.cpp" in project and "unlock_state_tests.cpp" in project
assert "GlassInstallRemoval.cpp" in project and "GlassRemovalOrder.h" in project
assert project.count("taskschd.lib") >= 2
assert 'ResourceCompile Include="GlassUnins.rc"' in unins_project
assert "GLASS_HAVE_UNLOCKER" not in project and "GLASS_EMBED_MSI" not in project
assert "GLASS_SETUP_NATIVE_MONITOR" in project
assert "IDR_MSI_" not in rc and "IDR_UNLOCKER_" not in unins_rc
assert "GlassSettings.Native.vcxproj" not in project
assert "GLASS_EMBED_MAINTENANCE" in project and "IDR_MAINTENANCE_UNINS RCDATA" in rc
assert "VerifyEmbeddedNativePayload" not in engine
assert "InstalledNativeDllX86" in util and "SignedPublisher" in util
assert "WinVerifyTrust" in util and "expected==actual" in util
assert "kNativeInstallerProtocolExport" in util
assert "bool PeBufferMatches(" in util and "bool ResourcePeMatches(" in util

native = (repo / "GlassSettings/Native/InjectProcessMapNative.cpp").read_text(encoding="utf-8")
protocol = (repo / "GlassShared/include/InjectModuleScan.h").read_text(encoding="utf-8")
assert "GlassSettings_InstallerScanProtocolV2" in native and "GlassSettings_InstallerScanProtocolV2" in protocol
build_guard = read("scripts/Verify-InstallerNative.ps1")
assert "GlassSettings_InstallerScanProtocolV2" in build_guard
assert "-File &quot;$(ProjectDir)scripts\\Verify-InstallerNative.ps1" not in project
assert "<AdditionalDependencies>GlassShared.lib;" not in project
solution = (repo / "GlassProject.sln").read_text(encoding="utf-8")
setup_guid = "{7E4C1A90-6B2D-4F58-9C31-0A6E5D4B2F18}"
assert f"{setup_guid}.Debug|x64.Build.0" not in solution
assert f"{setup_guid}.Debug|Any CPU.Build.0" not in solution
assert f"{setup_guid}.Debug|x86.Build.0 = Debug|Win32" in solution
assert f"{setup_guid}.Release|x86.Build.0 = Release|Win32" in solution

# MSI products, individual product-code routing, and no legacy forced settings removal.
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
assert "GlassProductUiPolicy::WarningText" in setup and "HKCU\\\\Software\\\\StanislavKa\\\\GlassPlatform" in rc
assert "StartMenuShortcutExists" in util
assert "QueryPlatformInstallPath" in util and "QueryOpenGlassInstallPath" in util
# Setup never removes products: checked products are installed or updated,
# unchecked installed products are kept; removal belongs to GlassUnins.
policy = read("src/GlassProducts.h")
assert "RemovalForInstall" not in policy and "InstallAction::Keep" in policy
assert "bool secureUxThemeRemove = false" in policy and "OnlySecureUxThemeRemoval" in policy
sequence = section(engine, "bool InstallEngine::RunMsiSequence()", "void InstallEngine::SeedUserDefaults()")
for token in ("UninstallProduct(", "removeCode", "kProductSettingsX86", "kProductPlatformX86",
              "ClearPlatformInstallPath(", "ClearOpenGlassInstallPath(", "PruneEmptyDirectories("):
    assert token not in sequence, token
assert "ProductWasSnapshotted" in sequence  # legacy codes replaced by MajorUpgrade are still snapshot-checked
assert "PrepareProductRemoval" not in engine and "removalSession_" not in engine and "remove_." not in engine
# Per-user defaults: seeded before the first platform start, never over existing lists, undone by rollback.
defaults = read("src/InstallSupport/GlassUserDefaults.cpp")
default_lists = (repo / "GlassShared/include/DefaultProcessLists.h").read_text(encoding="utf-8")
assert "GlassUserDefaults.cpp" in project and "GlassUserDefaults.h" in project
assert "GlassUserDefaults.cpp" in read("GlassSetup.vcxproj.filters")
assert "hasEntries" in defaults and "RegDeleteTreeW" in defaults and "kBlockListDescription" in defaults
assert "#include <windows.h>" not in default_lists and "Software\\\\StanislavKa\\\\GlassPlatform\\\\AttachList" in default_lists
assert default_lists.count("kModeForbidden }") == 2 and default_lists.count("kModePopupOnly }") == 28 and default_lists.count("kModeFull }") == 34
assert '{ L"explorer.exe", kModeFull }' in default_lists and '{ L"cmd.exe", kModeForbidden }' in default_lists
assert default_lists.count('        L"') - default_lists.count('        L"BlackWindowManager') == 21
register = section(engine, "bool InstallEngine::RegisterSelectedProducts()", "void InstallEngine::LaunchSelectedProducts()")
assert register.index("SeedUserDefaults();") < register.index("EnsureGlassAutorunTask(")
seed = section(engine, "void InstallEngine::SeedUserDefaults()", "bool InstallEngine::RegisterSelectedProducts()")
assert "AddFinishWarning(" in seed and "SetFailure(" not in seed and "IDS_WARN_USER_DEFAULTS" in rc
# Every selected platform is started after the installation, whatever its prior state was.
assert "ShouldStartPlatformArch" not in engine
launch = section(engine, "void InstallEngine::LaunchSelectedProducts()", "bool InstallEngine::StopHostsForRollback()")
assert "GP::StartPlatform(arch)" in launch and "skip start" not in launch and "taskEnabledBefore)" not in launch
assert "TASK_LOGON_PASSWORD" in removal and "TASK_LOGON_S4U" in removal
assert all(t in removal for t in ("GetSecurityDescriptor(kTaskSecurity", "SACL_SECURITY_INFORMATION", "ExpectedAction",
                                  "TASK_DONT_ADD_PRINCIPAL_ACE", "TASK_IGNORE_REGISTRATION_TRIGGERS",
                                  "RegisterTask(taskName.value", "DeleteServiceLast", "FILE_FLAG_OPEN_REPARSE_POINT",
                                  "RegSaveKeyExW", "SE_BACKUP_NAME", "LocalSystem", "session.serviceConfiguration",
                                  "GlassSettings.security.bin", "RestoreFileSecurity", "security == session.shortcutSecurity",
                                  "The GlassSettings shortcut points to a different program"))
assert "saved.xml != actualXml.value" in removal and "saved.security != actualSecurity.value" in removal
assert "GlassRemovalOrder::Commit" in removal and removal_order.index("if (!removeShortcut())") < removal_order.index("if (!removeTask(i))") < removal_order.index("return deleteService()")
worker = section(engine, "DWORD WINAPI InstallEngine::WorkerThread(", "bool InstallEngine::Start(")
assert worker.index("CreateRollbackSnapshot()") < worker.index("StopRunningHosts()")
# Commit point: registration (rollback-able) precedes the first start; every
# later step can only add finish-page notes.
assert worker.index("RegisterSelectedProducts()") < worker.index("LaunchSelectedProducts()")
assert "SetFailure(" not in worker[worker.index("LaunchSelectedProducts()"):worker.index("MsiEnableLogW(")]
assert "FinishWarningsJson()" in setup and "hostState.finishWarnings" in ui and "IDS_DONE_WARN_TAG" in rc
assert "GlassInstallRemoval::Commit" not in worker and "GlassInstallRemoval::Restore" not in worker
assert worker.count("undoUserDefaults();") == 4 and "UndoSeededProcessLists" in worker
success_restore = section(engine, "bool InstallEngine::RestorePriorHosts(", "DWORD WINAPI InstallEngine::WorkerThread(")
# On success only kept (unselected, installed) products Setup stopped are restored; selected ones are launched.
assert "if (!keptOnly || !selection_.platform32)" in success_restore and "if (keptOnly && selection_.openGlass) return ok" in success_restore
purge = engine[engine.index("bool InstallEngine::PurgeRemnants()"):]
assert "DeleteTree(" not in purge and "RemoveDirectoryTree(" not in purge
warnings = section(setup, "    std::wstring ComponentWarnings()", "    std::wstring ComponentsJson()")
assert "SelectionPreview()" in warnings and "GlassProductUiPolicy::WarningText" in warnings
assert "ComponentStateId" in setup and "installedSelection_.platform32" in setup and "IDS_COMP_STATE_KEEP" in rc
assert "IDS_COMP_STATE_REMOVE" not in rc and "IDS_COMP_BADGE_REMOVE" not in rc and "IDS_STATUS_REMOVE " not in rc
assert "ActionForInstall" in policy and "ComponentBadgeId" in setup and "ComponentBadgeTone" in setup
# Warnings about a missing product apply only when it is neither selected nor installed.
assert "X86WarningApplies()" in setup and "SettingsWarningApplies()" in setup and "!installedSelection_.glassSettings" in setup
for action in ("INSTALL", "UPDATE", "KEEP"):
    assert f"IDS_COMP_BADGE_{action}" in setup and rc.count(f"IDS_COMP_BADGE_{action}") == 2
    assert f".comp-badge.action-{action.lower()}" in ui
assert "item.warning" in ui and "comp-warning-button" in ui and "function paintComponentCard(" in ui and "c-src" in ui
assert "row.append(input,top,meta,foot)" in ui and "name.onclick=()=>showProductWarning" in ui
assert 'components-installed-grid' in ui and 'class="c-lane installed"' in ui
assert "productActionLabel(item)" in ui and "input.dispatchEvent(new Event('change'" in ui
assert ".comp-action{display:flex" in ui and "components-stack" in ui  # карточки и информационная полоса в прокручиваемой раскладке
assert "#a7dfff" in ui and "#efb94f" in ui  # independent blue-and-gold lock, not badge-tinted
assert "DrawComponentActionBadge(dc,selected,installed" in setup and "DrawMandatoryLock(dc," in setup
assert "item.state" in ui and "white-space:pre-line" in ui  # строка «что будет выполнено» вернулась
assert "const auto warnings=ComponentWarnings()" in setup and "DrawLabel(dc,warnings" in setup
assert "GlassProductUiPolicy::WarningText" in setup

# The animated unlock screen must stop when the worker exits the unlock phase,
# not when a localised status string happens to change.
unlock = read("src/WizardUnlockState.h")
signal = section(engine, "void InstallEngine::SignalHold(", "bool InstallEngine::StopGlassPlatform(")
release = section(engine, "bool InstallEngine::ReleaseLockedModules(", "bool InstallEngine::EnsureTempWorkspace(")
assert "Phase::Modules" in signal and "Phase::Processes" in signal
assert "WizardUnlock::EndOnExit finishUnlock(unlockActivity_)" in release
assert "unlockActivity_.End()" in release  # the next prompt clears the previous operation
assert 'return L"installing"' in unlock and "activity_.End()" in unlock
assert "installEngine_.ActiveUnlock()" in setup and "WizardUnlock::InstallingPage(unlock)" in setup
assert "visualMode_" not in setup and "unlockVisualUntil_" not in setup

# Merely looking up a ProductCode must never start an MSI UI session; opening
# the read-only database avoids the initial "Preparing to install" window.
metadata = section(msi, "std::wstring ProductCodeFromMsiPath(", "std::wstring KnownProductCodeFromFileName(")
assert "MsiOpenDatabaseW" in metadata and "MSIDBOPEN_READONLY" in metadata
assert "MsiDatabaseOpenViewW" in metadata and "MsiViewFetch" in metadata and "MsiRecordGetStringW" in metadata
assert "MsiCloseHandle(record)" in metadata and "MsiCloseHandle(view)" in metadata and "MsiCloseHandle(db)" in metadata
assert "MsiOpenPackage" not in msi and "MsiOpenProduct" not in msi
msi_remove = section(msi, "bool UninstallProduct(", "std::wstring ProductCodeFromMsiPath(")
msi_install = section(msi, "UINT InstallProduct(", "std::wstring QueryDisplayVersion(")
assert msi_remove.index("MsiSetInternalUI(INSTALLUILEVEL_NONE") < msi_remove.index("MsiConfigureProductExW")
assert msi_install.index("MsiSetInternalUI(INSTALLUILEVEL_NONE") < msi_install.index("MsiInstallProductW")
assert "MsiSetExternalUIRecord(OnMsiUiRecord" in msi_install and "cancelRequested" in msi_install
restore_security = section(removal, "bool RestoreFileSecurity(", "bool PrepareShortcut(")
assert "READ_CONTROL | WRITE_DAC | WRITE_OWNER | ACCESS_SYSTEM_SECURITY" in restore_security
assert "ReadFileSecurity(file, actual)" in restore_security

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
assert "EnsureAndDraw" in theme and "AddState(destination, width, height, normalizedState, focusedBlue)" in theme
# Only the needed comctl32 state is rasterized on demand; do not prewarm all
# seven sizes on the setup UI thread before the first window appears.
assert "PrewarmButtonCache" not in setup

# One-document UI, static initialization and user-driven cancel controls.
for mode in ("welcome", "checking", "conflicts", "downgrade", "version-invalid", "license",
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

# The first, centered native spinner is painted before deferred startup. MSI,
# version, logger, EULA decoding, and the runtime probe run on the worker; the
# actual WebView environment/controller are still created on the UI STA.
create = section(setup, "    bool Create(int show)", "    void JoinStartup()")
assert create.index("ShowWindow(window, show)") < create.index("UpdateWindow(window)") < create.index("PostMessageW(window,kBeginStartup")
loading = section(setup, "    void InitializeLoading()", "    void InitializeControls()")
assert "kHeroTimer" in loading and "IsProductInstalled" not in loading and "IsWebView2RuntimeInstalled" not in loading
spinner = section(setup, "    void DrawLoadingScreen(", "    void EnsureBanner()")
assert all(token in spinner for token in ("Scale(473)", "Scale(243)", "Scale(54)", "loadingAnimationsEnabled_"))
assert "DrawLoadingScreen(dc)" in setup and "case WM_SETTINGCHANGE:" in setup
startup_worker = section(setup, "    void StartupWorker(", "    void BeginStartup()")
for token in ("COINIT_MULTITHREADED", "InstallLog::Initialize()", "SelfProductVersion()",
              "IsProductInstalled", "IsWebView2RuntimeInstalled()", "Utf8(Resource(IDR_EULA"):
    assert token in startup_worker, token
assert startup_worker.index("std::lock_guard<std::mutex> lock(startupMutex_)") < startup_worker.index("startupStop_.load(std::memory_order_acquire)) return;") < startup_worker.index("PostMessageW(target,kStartupComplete")
controls_init = section(setup, "    void InitializeControls()", "    void StartupWorker(")
assert "InstallLog::" not in controls_init and "IsProductInstalled" not in controls_init
begin_startup = section(setup, "    void BeginStartup()", "    void CompleteStartupIfReady()")
assert "std::thread" in begin_startup and "kStartupPollTimer" in begin_startup
assert begin_startup.index("WizardTheme::InstallDetours()") < begin_startup.index("std::thread")
assert "--debug-slow-startup" in begin_startup and "IDS_STARTUP_FAILED" in begin_startup
complete_startup = section(setup, "    void CompleteStartupIfReady()", "    void Layout()")
assert "startupThread_.join()" in complete_startup and "Page::RuntimeRequired" in complete_startup
assert "Page::AlreadyRunning" in complete_startup and "PostMessageW(window,kInitWebView" in complete_startup
assert "case kInitWebView: if (!closing_) InitializeWebView()" in setup
assert "COINIT_APARTMENTTHREADED" in setup
cleanup = section(setup, "    void Cleanup()", "    LRESULT Handle(")
assert "startupMutex_" in cleanup and "startupStop_.store(true" in cleanup and "startupThread_.join()" not in cleanup
main_loop = section(setup, "int WINAPI wWinMain(", "    if (gWindowClassRegistered)")
assert main_loop.index("gInstaller.Create(show)") < main_loop.index("gInstaller.JoinStartup()")
assert "InstallLog::Initialize()" not in main_loop
assert "case WM_CREATE: InitializeLoading()" in setup
assert "if (LOWORD(w)==IDCANCEL || LOWORD(w)==IDC_CANCEL) RequestClose()" in setup
assert "if (controlsReady_) { CreateFonts(); Layout(); WizardTheme::ResetButtonCache(); }" in setup
# The *last painted* left-anchored strip is captured when cancel is requested;
# confirmation, MSI acknowledgement, and unknown rollback stages cannot replace
# it with the engine target, a right-anchored fill, or a moving segment.
progress_draw = section(setup, "case WM_DRAWITEM:", "case WM_CTLCOLORBTN:")
assert "WizardProgress::DeterminateSpan" in progress_draw and "visibleProgress_.RecordPaint(progressValue_)" in progress_draw
assert "progressMarquee_ && page_==Page::RuntimeInstalling" in progress_draw
assert "MarqueePhase" not in progress_draw and "progressValue_,progressReverse_" not in progress_draw
cancel_request = section(setup, "void RequestClose() {", "void WebFailure(")
assert "visibleProgress_.PaintedOr(progressValue_)" in cancel_request
assert "visibleProgress_.Capture(visibleAtRequest)" in cancel_request
begin_rollback = section(setup, "void BeginRollbackVisual(", "void StartInstallation()")
assert "rollbackProgress_.Begin(visibleProgress_.RollbackStart(progressValue_))" in begin_rollback
assert "progressMarquee_=false" in begin_rollback
assert "visibleProgress_.Reset()" in section(setup, "void StartInstallation()", "bool RunPreflightChecks()")
assert 'L"<1%"' in section(setup, "void RenderFooterText(", "void Paint()")
simulation = section(setup, "void TickSimulation()", "    void Tick() {")
assert "page_==Page::ConfirmCancel) return" in simulation
assert "FadeToZero" in simulation and "rollbackProgress_.Start()" in simulation
progress_tick = section(setup, "void Tick() {", "void RequestClose() {")
assert "page_==Page::ConfirmCancel) { progressTickMs_=now; return; }" in progress_tick
assert "rollbackProgress_.ReportRemaining" in progress_tick
assert "engineReverse && !installEngine_.IsMarquee()" in progress_tick
assert progress_tick.index("RollbackSucceeded()") < progress_tick.index("rollbackProgress_.Finish()")
assert "WizardProgress::FadeToZero" in progress_tick and "WizardProgress::Percent(progressValue_)" in progress_tick
assert "WizardProgress::SmoothTowards(displayProgress_,progressTarget_,alpha)" in progress_tick
assert "progressTarget_=std::max(progressTarget_,reported)" in progress_tick
rollback_ui = section(engine, "bool InstallEngine::PerformRollback()", "bool InstallEngine::PurgeRemnants()")
assert "WizardProgress::RollbackRemaining(units)" in rollback_ui
for start, end in (("bool InstallEngine::PerformRollback()", "bool InstallEngine::UndoCleanInstall()"),
                   ("bool InstallEngine::UndoCleanInstall()", "bool InstallEngine::PurgeRemnants()")):
    phase = section(engine, start, end)
    assert phase.index('SetStatus(labels_.rollback.empty() ? L"…" : labels_.rollback, -1)') < phase.index("progressReverse_.store(true")
confirm = section(setup, "case IDC_NEXT:", "case IDCANCEL:")
assert "BeginRollbackVisual(GetTickCount64())" in confirm and "ShowCancelling()" in confirm
show_cancel = section(setup, "void ShowCancelling()", "void StartInstallation()")
assert "SetPage(Page::Cancelling)" in show_cancel and "UpdateWindow(progress_)" in show_cancel
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
assert undo.index("UninstallSessionProductSafely") < undo.index("++productsProcessed") < undo.index("productsProcessed*8000/productCount")
assert "DirBytes(state.backupX64)" in rollback and "IsNestedSubPath" in rollback
assert "receivedFrame" in watch and "FILE_FLAG_FIRST_PIPE_INSTANCE" in watch
assert "BeginLocalNativeMonitor" in watch and "GlassSettings_ScanGlassModules" in watch
assert "kNativeInstallerScanProtocol" in watch and "GetModuleHandleW(L\"GlassCompat.dll\")" in watch
assert "pendingRows.empty() && pipeBuffer.empty()" in watch and "helperExitCode!=STILL_ACTIVE" in watch
worker = section(engine, "DWORD WINAPI InstallEngine::WorkerThread(", "bool InstallEngine::Start(")
assert worker.index("CreateRollbackSnapshot()") < worker.index("StopRunningHosts()")
assert worker.index("StopRunningHosts()") < worker.index("InstallGlassSettingsEarly()")
assert worker.index("InstallGlassSettingsEarly()") < worker.index("ReleaseLockedModules()") < worker.index("RunMsiSequence()")
assert "nonSettingsChangesAttempted_ = true" in worker and "nonSettingsChangesAttempted_ ? GlassInstallRollback::RestoreScope::AllProducts" in rollback_ui
assert "RestoreScope::SettingsX64" in rollback and 'L"\\\\GlassSettings"' in rollback
assert "RestoreScope::SettingsX64" in rollback_ui and "RestoreScope::SettingsX86" in rollback_ui
assert "QueryPlatformMutex" in engine and "GetLastError() == ERROR_FILE_NOT_FOUND" in engine
native = (repo / "GlassSettings/Native/InjectProcessMapNative.cpp").read_text(encoding="utf-8")
assert "GlassSettings_ScanGlassModules" in native and "kNativeInstallerScanProtocol" in native
assert "IsWow64Process(GetCurrentProcess(), &selfWow)" in engine and "scanNativeX64_ = selfWow != FALSE" in engine
assert "if (priorStateOk) priorStateOk=self->DetermineCleanInstall()" in worker
assert "QueryOpenGlassTasks" in util and "GetInstances(0" in util and "QueryOpenGlassRunning" in setup
og_probe = section(util, "bool QueryOpenGlassRunning(", "bool QueryOpenGlassFiles(")
assert 'FindWindowW(L"Dwm"' not in og_probe and "serviceExists || tasks.found" in og_probe
og_stop = section(host, "bool StopOpenGlassHost(", "bool StopAllForUninstall(")
assert og_stop.index("QueryOpenGlassRunning") < og_stop.index("ControlService")
assert "if (!running)" in og_stop and "StopRunningOpenGlassTasks" in og_stop
assert "ChooseCancellationOrFailure(!ok)" in worker and "failureFinalizing_" in engine_h
assert "ShouldStartOpenGlass(openGlassStartup_)" in engine and "Snapshot OpenGlass path unavailable" in rollback
assert "protectedHandles" in util and "FILE_FLAG_OPEN_REPARSE_POINT" in util and "FileDispositionInfo" in util
assert "PruneEmptyDirectories(" not in engine and "PruneEmptyDirectories(openGlassDir_,keptRoots)" in unins  # pruning belongs to the uninstaller
assert "Page::Launching" in setup and "kMinLaunchMs = 5000" in engine
assert "--demo-tour" in setup and "debugDemoPending_ = false" in setup
assert "MsiEnableLogW(0, nullptr, 0)" in engine
assert "CERT_CHAIN_POLICY_MICROSOFT_ROOT" in setup
assert "GlassPackageAcquisition::FetchCatalog" in setup
assert "status!=200" in read("src/InstallSupport/GlassReleaseClient.cpp")
assert "IsAllowedReleaseUrl" in setup and "IShellDispatch2" in setup
assert "CreateSecureTempDirectory" in util
assert 'CreateSecureTempDirectory(L"GlassSetupLogs"' in read("src/InstallLog.cpp")
assert 'CreateSecureTempDirectory(L"GlassUninsLogs"' in read("src/InstallSupport/GlassInstallLog.cpp")
assert "SecureLogDirectory()" in unins and 'GetEnvironmentVariableW(L"ProgramW6432"' not in setup + unins + engine
native_helper = section(util, "std::wstring NativeDllForArch(", "bool QueryPlatformInstallPath(")
assert "InstalledSecureUxThemeNativeDll(arch,x64Dir,x86Dir" in native_helper and "kNativeInstallerProtocolExport" in native_helper
assert "EnsureExtractedUnlockerX64()" not in native_helper
assert "QueryPlatformInstallPath(" not in native_helper and "TryGetAutorunGlassCompatInstallRoot(" not in native_helper
shell_ops = (repo / "GlassSettings/Native/ShellExtensionOps.cpp").read_text(encoding="utf-8")
shell_sparse = (repo / "GlassSettings/Native/ShellSparsePackage.cpp").read_text(encoding="utf-8")
pack = (repo / "GlassShellExt/Package/BuildSparsePackage.ps1").read_text(encoding="utf-8")
shell_query = (repo / "GlassSettings/Native/ShellExtensionQuery.cpp").read_text(encoding="utf-8")
constants = (repo / "GlassShared/include/GlassShellMenuConstants.h").read_text(encoding="utf-8")
payload = (repo / "GlassSetup/msi/GlassSetupPayload.targets").read_text(encoding="utf-8")
assert "GlassSettings_ShellExtRegisterSetupV1W" in util and "/installroot" in util
assert "exactPlatformRoot" in shell_ops and "exactPlatformRoot" in shell_sparse
assert "0x47505245" in util and "0x47505245" in shell_ops
# H2/H3: the msix is signed by the author certificate only; the build never creates, exports or
# trusts certificates. On the user's machine the author certificate enters LocalMachine\TrustedPeople
# in exactly one place (ShellSparsePackage::TrustSparseSigner) and only after an explicit choice:
# the Setup certificate page or the GlassSettings notice link. The uninstaller never touches it.
assert "ShellDevCertificate" not in shell_sparse and "ShellDevCertificate" not in shell_ops
assert not (repo / "GlassSettings/Native/ShellDevCertificate.cpp").exists()
for forbidden in ("New-SelfSignedCertificate", "Export-PfxCertificate", "TrustedPeople", "signtool", "Import-Certificate"):
    assert forbidden not in pack, forbidden
assert "makeappx" in pack.lower() and "GlassShellSparse" in pack
assert "SignerNotTrusted" in shell_sparse and "IsThumbprintInTrustedPeople" in shell_sparse
assert "QuerySparseSignerTrust" in shell_sparse and "GlassSettings_GetShellSignerTrustV1" in shell_query
assert "GlassSettings_GetShellMenuModeV1" in shell_query
trust_impl = section(shell_sparse, "SparseSignerTrustResult TrustSparseSigner(", "InstallerSparseState QueryInstallerSparseState(")
assert shell_sparse.count("::CertAddCertificateContextToStore(") == 1 and "::CertAddCertificateContextToStore(" in trust_impl
assert "CERT_STORE_ADD_REPLACE_EXISTING" in trust_impl and "SubjectMismatch" in trust_impl and "CERT_SYSTEM_STORE_LOCAL_MACHINE" in trust_impl
assert "CertAddCertificateContextToStore" not in shell_ops + util + engine + unins + setup
assert "CertDeleteCertificateFromStore" not in shell_sparse + shell_ops + util + engine + unins + setup
assert "TrustedPeople" not in unins  # the uninstaller never removes the author certificate (user decision)
assert "GlassSettings_TrustShellSignerV1" in shell_ops and "GlassSettings_ShellSignerTrustSetupV1W" in shell_ops
assert "GlassSettings_ShellSignerTrustSetupV1W" in util and "0x47535400" in util and "0x47535400" in shell_ops
assert "selection_.trustSigner" in engine and "InvokeShellSignerTrust" in engine and "warnCertInstall" in engine
assert "Page::Certificate" in setup and "CertificatePageApplies()" in setup and "QuerySelfSignerInfo()" in setup
assert "postMessage('certificate:'+item.id)" in ui and "renderCertificatePanel" in ui and 'data-mode="certificate"' in ui
assert "certificate:1" in setup and "certificate:0" in setup  # host side of the web message
assert "IDS_CERT_TITLE" in setup and "IDS_WARN_CERT_INSTALL" in setup
# H3-UI: the certificate page is short (no what/why/note blocks), radios say accept/decline;
# text is never selectable and the old "GlassAuthorCodeSigning" naming is gone everywhere.
header_now = read("res/resource.h")
for removed in ("IDS_CERT_WHAT", "IDS_CERT_WHY", "IDS_CERT_NOTE"):
    assert removed not in header_now and removed not in setup and removed not in rc, removed
assert "Принять сертификат" in rc and "Не принимать" in rc and "Принять сертификат разработчика?" in rc
assert "certificate-what" not in ui and "certificate-note" not in ui and "user-select:none" in ui
for stale in (rc, util, unins, engine, payload, constants, read("res/Eula.ru.txt"), read("res/Eula.en.txt")):
    assert "GlassAuthorCodeSigning" not in stale
for eula in ("res/Eula.ru.txt", "res/Eula.en.txt"):
    text = read(eula)
    assert "certlm.msc" in text and "DeveloperCodeSigning.txt" in text, eula
assert "Сертификат разработчика" in read("res/Eula.ru.txt") and "Developer certificate" in read("res/Eula.en.txt")
# Classic Win32 menu: only regsvr32 /s (DllRegisterServer of the bundled GlassShellExt.dll) — on
# Windows 10 always, on Windows 11 as the fallback when the sparse package cannot be registered.
# Nobody writes shellex/ContextMenuHandlers keys by hand (GlassShellExt/README.md, AGENTS.md).
assert "RegisterClassicViaRegsvr32" in shell_ops and "UnregisterClassicViaRegsvr32" in shell_ops
assert "RemoveClassicAfterSparseSuccess" in shell_ops and "RegsvrClassicFallback" not in shell_ops
for forbidden in ("RegisterLegacyContextMenuHandler", "RegisterClassicInProcess", "ContextMenuHandlers\\GlassPlatformDesktopMenu"):
    assert forbidden not in shell_ops + util + engine + unins + setup, forbidden
assert "RegCreateKey" not in shell_ops and "RegSetValue" not in shell_ops
assert "kSetupClassicFallbackBase" in shell_ops and "0x47504C00" in shell_ops and "0x47504C00" in util
assert "DescribeShellClassicFallback" in util and "classicFallbackReason" in util
assert "IDS_WARN_SHELL_CLASSIC_CERT" in setup and "warnShellClassicCert" in engine and "DeveloperCodeSigning.cer" in engine
shell_actions = read("../GlassShellExt/DesktopShellActions.cpp")
assert "GlassProject" not in shell_actions and "GlassShellExt-launch.log" in shell_actions
assert 'L"%s\\\\GlassPlatform"' in shell_actions and 'L"%s\\\\Logs"' in shell_actions  # %LOCALAPPDATA%\GlassPlatform\Logs
assert "kShellExtRelativePath" in util and "kShellExtRelativePath" in unins and "kShellHostRelativePath" in setup
assert "kSparsePublisher[] = L\"CN=Stanislav K.\"" in constants
for manifest in ("GlassShellExt/Package/AppxManifest.xml", "GlassShellExt/Package/AppxManifest.UnregisterStub.xml"):
    assert 'Publisher="CN=Stanislav K."' in (repo / manifest).read_text(encoding="utf-8"), manifest
assert (repo / "Shared/certs/DeveloperCodeSigning.cer").exists() and (repo / "Shared/certs/DeveloperCodeSigning.txt").exists()
assert "DeveloperCodeSigning.cer" in payload and "DeveloperCodeSigning.txt" in payload and "GlassShellSparse" in payload
assert 'm.type===\'init\'||m.type===\'state\'' in ui

# SecureUxTheme uses the original author's MSI, not our payload or custom action.
native_host_h = (repo / "GlassSettings/Native/NativeHost.h").read_text(encoding="utf-8")
sut_native = (repo / "GlassSettings/Native/SecureUxThemeNative.cpp").read_text(encoding="utf-8")
sut_inspect = read("src/InstallSupport/GlassProductInspect.cpp")
sut_service = (repo / "GlassSettings/Services/SecureUxThemeService.cs").read_text(encoding="utf-8")
sut_package = read("src/InstallSupport/GlassSecureUxPackage.cpp")
sut_release = read("src/InstallSupport/GlassSecureUxRelease.h")
assert "releases/latest" in sut_package and "browser_download_url" in sut_release and "sha256:" in sut_release
assert "GetModuleFileNameW" in sut_package and "CreatePrivateFile" in sut_package
release_client = read("src/InstallSupport/GlassReleaseClient.cpp")
assert "WINHTTP_FLAG_SECURE" in release_client and "CryptHashData" in release_client
assert "GlassReleaseClient::VerifyHash" in sut_package and "using GlassReleaseClient::Get" in sut_package
assert "GlassSecureUxPackage::Prepare" in engine and "InstallOriginalSecureUxTheme" in worker
assert worker.index("RegisterSelectedProducts()") < worker.index("InstallOriginalSecureUxTheme()") < worker.index("LaunchSelectedProducts()")
original_msi=section(engine, "bool InstallEngine::InstallOriginalSecureUxTheme()", "bool InstallEngine::RunMsiSequence()")
assert 'L"REBOOT=ReallySuppress"' in original_msi
assert "PatchArpUninstallString(" not in original_msi and "NoteInstalled(" not in original_msi
assert "ERROR_SUCCESS_REBOOT_REQUIRED" in original_msi
run_sequence=section(engine, "bool InstallEngine::RunMsiSequence()", "void InstallEngine::SeedUserDefaults()")
assert "SecureUxTheme-" not in run_sequence
assert "IDR_MSI_SUT_" not in rc and "IDR_MSI_SUT_" not in header
assert "SecureUxThemeCa" not in payload and "SecureUxTheme-" not in payload
assert "SecureUxTheme-x64.msi" not in rc and "SecureUxTheme-x86.msi" not in project
solution=read("../GlassProject.sln")
assert '"SecureUxTheme-x64"' not in solution and '"SecureUxThemeCa"' not in solution
assert "<GlassSignThisProject>true</GlassSignThisProject>" in read("msi/OpenGlass-x64.wixproj")
# Actual discovered plan is built before any process shutdown or MSI mutation.
assert worker.index("CreateRollbackSnapshot()") < worker.index("BuildExecutionPlan()") < worker.index("CloseGlassSettingsProcesses(")
execution_plan=section(engine,"bool InstallEngine::BuildExecutionPlan()", "bool InstallEngine::RunMsiSequence()")
for token in ("GlassProductInspect::Run", "input.inspectSources=false", "CheckProductContexts", "RecheckSnapshottedProduct", "RecheckRelatedProducts", "GlassMaintenanceCache::Identity", "P::Build(facts,request)"):
    assert token in execution_plan, token
assert "const auto& plan=executionPlan_" in run_sequence
assert "steps[stepIndex++]" not in run_sequence
assert "if(action==P::Action::Remove)return ERROR_INVALID_PARAMETER" in run_sequence
helper_phase=section(engine,"bool InstallEngine::InstallGlassSettingsEarly()", "bool InstallEngine::InstallOriginalSecureUxTheme()")
assert "RunPhase(executionPlan_,GlassMaintenanceExecutor::Phase::Helpers" in helper_phase
assert "executionPlanReady_" in helper_phase
assert "PrepareSecureUxRemoval()" in worker
assert worker.index("PrepareSecureUxRemoval()") < worker.index("StopRunningHosts()")
assert 'message==L"component:secureUxTheme:remove"' in setup
assert "SecureUxThemeRemovalSupportedLocked" in setup and "secureUxRemovalCode_=inspect_.officialSecureUxThemeCode" in setup
assert "selection_.secureUxThemeRemove)return RemoveOriginalSecureUxTheme()" in engine
assert "PrepareRemoval" in sut_package and "RecheckRemoval" in sut_package
assert "MSIINSTALLCONTEXT_MACHINE" in sut_package and "Vendor cached PackageCode differs from registration" in sut_package
assert "before==removal.digest && now==removal.digest" in sut_package
assert "if(!vendorOnly)ok = self->RegisterSelectedProducts()" in worker
assert "if(!vendorOnly)self->LaunchSelectedProducts()" in worker
assert "IDS_APPLY" in setup and 'IDS_APPLY "Применить"' in read("res/ProductStrings.rcinc")

assert "if(in.inspectSources)GlassPackageAcquisition::FetchCatalog" in sut_inspect
assert worker.index("InstallGlassSettingsEarly()") < worker.index("RunMsiSequence()")
# Retain recognition and explicit removal of legacy Glass-owned products.
for suffix in ("5076", "5077"):
    assert suffix in read("src/InstallSupport/GlassInstallMsi.h")
assert "msiResSecureUxTheme" not in setup and "report.secureUxTheme" in sut_inspect
assert "kProductSecureUxThemeX64" in engine and "kProductSecureUxThemeX86" in rollback and "kProductSecureUxThemeX64" in unins
activation = section(sut_native, 'extern "C" HRESULT WINAPI GlassSettings_SecureUxThemeActivate(', 'extern "C" HRESULT WINAPI GlassSettings_SecureUxThemeDeactivate(')
assert "CopyFile" not in activation and "DeleteFile" not in activation
assert "InvokeSecureUxThemeDeactivate" in util and "InvokeSecureUxThemeDeactivate" in unins
# H3 item 6: the OpenGlass state query must see the installer's service (not only the legacy
# task) on every thread: the cheap SCM query runs before the scheduler-UI guard, and only the
# COM task-tree walk stays background-only. Otherwise Home/System rows show "No task" for a
# service-based installation.
og_state = (repo / "GlassShared/src/OpenGlassHostState.cpp").read_text(encoding="utf-8")
og_start = og_state.index("HostUiState QueryOrchestrationUiState() noexcept")
og_body = og_state[og_start:og_state.index("HostUiState QueryHostUiState(", og_start) if "HostUiState QueryHostUiState(" in og_state[og_start:] else og_start + 2500]
assert "QueryServiceUiState()" in og_body and "GlassShared::IsSchedulerUiThread()" in og_body
assert og_body.index("QueryServiceUiState()") < og_body.index("GlassShared::IsSchedulerUiThread()"), \
    "the OpenGlass service must be queried before the scheduler-UI guard"
assert "ServiceInstalled()" in og_state and "OrchestrationInstalled()" in og_state

# 2026-11-03 feedback: finish-page wording and bottom gap; stray Wow6432Node key cleanup;
# classic menu nested with icons; OpenGlass gate removable; inspection cards (Task F 1-2).
assert "Установка завершена с замечаниями" in rc and "Installation completed with notes" in rc
assert "--content-bottom-gap" in ui and "inset:0 0 var(--content-bottom-gap) 0" in ui
util_cpp = (repo / "GlassSetup/src/InstallSupport/GlassInstallUtil.cpp").read_text(encoding="utf-8")
assert "DeleteRegistryKeyIfEmpty" in util_cpp and "RemoveEmptyInstallPathKeys" in util_cpp
assert "Software\\\\Wow6432Node\\\\GlassPlatform" in util_cpp
menu_src = (repo / "GlassShellExt/DesktopShellMenu.cpp").read_text(encoding="utf-8")
assert "CreatePopupMenu()" in menu_src and "MIIM_SUBMENU" in menu_src
assert "IDS_GLASSSHELL_ROOT_TITLE" in menu_src and "MenuIcon(IDI_GLASSSHELLEXT" in menu_src
ogs = (repo / "GlassSettings/Core/Platform/OpenGlassInstallService.cs").read_text(encoding="utf-8")
assert "OpenGlassHostPath is not null" not in ogs, "uninstall must not gate service removal on the exe path"
row_src = (repo / "GlassSettings/Controls/PlatformSchedulerRow.cs").read_text(encoding="utf-8")
assert "Task.Run(() => QueryDirectState().TaskExists)" in row_src
inspect_cpp = (repo / "GlassSetup/src/InstallSupport/GlassProductInspect.cpp").read_text(encoding="utf-8")
assert "MsiGetFileHashW" in inspect_cpp and "WinVerifyTrust" in inspect_cpp and "MsiGetComponentPathW" in inspect_cpp
gp = read("src/GlassProducts.h")
assert "ProductCondition" in gp and "DefaultSelectedFor" in gp and "ActionForCondition" in gp
assert "IDS_COMP_BADGE_REPAIR" in setup and "IDS_INSPECT_FILE_HASH" in setup and "IDS_INSPECT_SIGNATURE" in setup
assert "action-repair" in ui and "comp-reasons" in ui and "reasons" in setup
assert "GlassProductInspect.cpp" in (repo / "GlassSetup/GlassSetup.vcxproj").read_text(encoding="utf-8")

# Registry sanity (2026-11-03 decisions): invalid values are DELETED (defaults are never
# written to the registry); only known names are touched; base Windows DWM values are not
# in the schema; optional GlassSettings startup check behind a toggle in app settings.
reg_sanity = (repo / "GlassShared/src/RegistrySanity.cpp").read_text(encoding="utf-8")
assert "RegDeleteValueW" in reg_sanity and "ColorizationColorOverride" in reg_sanity
assert '{ L"ColorizationColor"' not in reg_sanity and '{ L"EnableTransparency"' not in reg_sanity
assert "ValidateSettingsOnStartup" in reg_sanity
assert "GlassSettings_ValidateRegistryState" in native_host_h
assert "RegistrySanity" in inspect_cpp
settings_reg = (repo / "GlassSettings/Core/Registry/GlassSettingsRegistry.cs").read_text(encoding="utf-8")
assert "ValidateSettingsOnStartupValue" in settings_reg
app_panel = (repo / "GlassSettings/Controls/AppSettingsPanel.xaml").read_text(encoding="utf-8")
assert "ValidateRegistrySwitch" in app_panel
app_cs = (repo / "GlassSettings/Controls/AppSettingsPanel.xaml.cs").read_text(encoding="utf-8")
assert "WriteValidateSettingsOnStartup" in app_cs

# 2026-11-03 follow-up: update is not reinstall (reasons never mix), integrity is
# measured against the cached MSI the product was installed from (LocalPackage),
# and the components page is compact (no state line, single-line reasons).
assert "MsiGetProductInfoW" in inspect_cpp and "LocalPackage" in inspect_cpp
assert "AddReason(rep, Reason::VersionOutdated)" not in inspect_cpp
assert "comp-reason-line" in ui and "comp-state" in ui  # обе строки на каждой карточке
assert "if (rep->updateAvailable) return GlassProducts::ProductCondition::UpdateAvailable;" in setup

# 2026-11-03 follow-up 2: schema types match OpenGlass SettingsCatalog v3 and the C#
# writers (no false "damaged"); signature accepted when ours OR trusted by Windows;
# reasons are a bulleted list; the mandatory lock shows only when action is required.
assert '{ L"CustomThemeReflection", ValueKind::Text }' in reg_sanity
assert 'L"LastAppliedThemeDwmFingerprint", ValueKind::Text' in reg_sanity
assert "MINMAXBUTTONGLOWid" in reg_sanity and "ColorizationGlassReflectionPolicy" in reg_sanity
assert "trust != ERROR_SUCCESS" in inspect_cpp and "ours" in inspect_cpp
assert "condition!=GlassProducts::ProductCondition::Healthy)?Text(IDS_COMP_MANDATORY)" in setup
assert "...(item.reasons||[])" in ui and "item.disabled||item.locked" in ui

# 2026-11-03 follow-up 3: card reasons behind a mini-spoiler (accordion, equal heights);
# the registry reason names the offending values; OpenGlass user resources and
# OpenGlassHost.exe are exempt from integrity; the OS-native platform is no longer
# force-selected (mandatory = card lock when action is required).
assert "AppendName" in reg_sanity and "glassValueNames" in reg_sanity
assert "IsOpenGlassUserResource" in inspect_cpp and "OpenGlassHost.exe" in inspect_cpp
assert "registryValueNames" in setup and "comp-reason-line" in ui
assert "grid-auto-rows:1fr" in ui  # равные высоты карточек
assert "os64 && GlassProducts::DefaultSelectedFor(" in setup
assert "ОС-нативная платформа обязательна, но не принудительно отмечена" in gp

# 2026-11-03 follow-up 4: OpenGlass version comes from OpenGlass.dll (independent of the
# platform version); checking a card never force-checks the main product; the reasons line
# sits on every card; the uninstaller offers settings cleanup (GlassPlatform key + OpenGlass
# DWM values of the current user).
assert "QueryFileVersion" in inspect_cpp and "version.lib" in inspect_cpp
assert "OpenGlass.dll" in inspect_cpp and "FileVersionOf" in inspect_cpp
assert "components:select-all" in setup and "component:x64:1" in setup
assert "CleanOwnedSettings" in reg_sanity and "cleanupProfiles.Cleanup" in unins
assert 'L",\\\"locked\\\":false,\\\"hidden\\\":false}"' in unins
assert "GlassProductUiPolicy::WarningText" in unins and "componentsWarning" in unins
assert "Clean all user profiles, HKLM and saved Glass themes" in ui and "Unchecked products and settings are retained" in ui
assert "IDC_UNINS_SETTINGS_CLEANUP" in header_now and "cleanupSettings" in unins
assert "Удалить настройки" in unins and "RegDeleteTreeW" in reg_sanity

# 2026-11-03 follow-up 5: «Установить» disabled with no product selected; the reasons
# subline stays on every card with neutral grey «не требуется» when there are no reasons.
assert "HasSelectedComponent" in setup
assert "...(item.reasons||[])" in ui and "showProductWarning" in ui  # details live in the popup, not in C cards
# Approved 2026-11-04 layout supersedes the lower warning stripe and mandatory locks.
assert "comp-slim" in ui and "components-extra" in ui and "components-stack" in ui
assert "left:48px;width:302px" in ui and "right:48px;width:268px" in ui and "height:330px" in ui
assert "mask-image:linear-gradient(180deg,transparent,#000 18px" in ui
assert "has-warning" in ui and ".c-card.has-warning" in ui
assert "components:select-all" in ui and "components:select-all" in setup and "components:select-all" in unins
assert "comp-warning-button" in ui and "components-warning-shade" in ui
assert "e.stopPropagation();closeProductWarning()" in ui
assert "plan-warning" in ui and "li.textContent='⚠ '+item.warning" in ui
assert "selected.length} of ${inputs.length}" in ui
assert "More products below" in ui and "Products above" in ui
assert "const bool sutNeedsSettings=false" in setup and "const bool sutRequiresSettings=false" in setup
assert "(page==Page::UnlockModules || page==Page::Cleanup || (select&&!webReady_))?SW_SHOWNA:SW_HIDE" in unins and "ShowWindow(back_,SW_HIDE)" in unins
assert "(page_==Page::Select || page_==Page::Cleanup)?SW_SHOWNA:SW_HIDE" in unins
assert "WizardTheme::PaintFooterBar(dc" in setup and "void PaintFooterBar(" in theme and "bufferedErase_" not in setup
assert "const bool terminalPage=page_==Page::Complete || page_==Page::Cancelled || page_==Page::Error" in setup
assert "const int footerTop=terminalPage?kHtmlHeight:kContentHeight" in setup and "Box(0,kHtmlHeight,kWidth,1)" not in setup
assert "const bool progressPage=page_==Page::Stopping || page_==Page::Removing" in unins
assert "const int footerTop=progressPage?kContentHeight:kHtmlHeight" in unins
assert "text.find(part)!=std::wstring::npos" in setup  # error detail is not appended twice
assert "GlassSettings" in unins and "secureUxTheme" in unins and "IDC_UNINS_SECUREUXTHEME" in unins
assert "SecureUxTheme retained" in unins and "SecureUxThemeState::External" in unins
assert "slim" in unins and "cleanup" in unins and "Будет удалено" in unins and "PreflightCleanup" in unins

# 2026-11-03 follow-up 6: integrity analysis runs in the background during the first
# pages; the products page shows the startup-style loading donut until it completes;
# the reasons line starts with a bold dot vertically centered.
assert "kInspectReady" in setup and "inspectThread_" in setup and "IDS_COMP_ANALYZING" in setup
assert "componentsLoading" in setup and "comp-loading" in ui and "loading-donut" in ui
assert "comp-reason-line:before" in ui and "componentsLoading" in ui

# 2026-11-03 follow-up 7: final pages hide the progress strip and align one
# footer divider with the HTML content edge.
assert "const int footerTop=terminalPage?kHtmlHeight:kContentHeight" in setup and "Box(0,kHtmlHeight,kWidth,1)" not in setup
assert "page_==Page::RuntimeInstalling);" in setup and "page_==Page::RuntimeInstalling || page_==Page::Complete" not in setup

# 2026-11-03 follow-up 8: on a clean install the reason line states requiredness —
# «требуется» on platform products, «рекомендуется» on OpenGlass (neutral «не требуется»
# remains for installed/healthy cards).
assert "IDS_COMP_RECOMMENDED" in setup
assert "wcscmp(id,L\"openglass\")==0?IDS_COMP_RECOMMENDED:IDS_COMP_REQUIRED" not in setup  # defaults are recommendations, never mandatory locks
assert "IDS_COMP_REQUIRED" in header_now and "требуется" in rc and "рекомендуется" in rc
assert "GlassProducts::ProductCondition::NotInstalled)" in setup

# Строки действия и причин прижаты к низу карточки — один уровень Y на всех карточках.
assert "comp-foot" in ui and "margin-top:auto" in ui and "align-self:stretch}" in ui  # тело карточки тянется на всю высоту — строки внизу на одном уровне

# Production maintenance safety barriers (source checks, not registry/MSI integration).
profiles=read("src/InstallSupport/GlassProfileMaintenance.h")
cache=read("src/InstallSupport/GlassMaintenanceCache.h")
style=(repo / "GlassShared/include/SystemVisualStyle.h").read_text(encoding="utf-8")
assert "RegLoadAppKeyW" in profiles and "KEY_ALL_ACCESS,REG_PROCESS_APPKEY" in profiles
assert "ProfileList" in profiles and "NTUSER.DAT unavailable" in profiles
assert unins.index("cleanupProfiles.PreflightCleanup") < unins.index("StopAllForUninstall(x64Dir_")
assert "CleanOwnedSettings(profile.hive,false,false" in profiles and "CleanOwnedSettings(profile.hive,false,true" in profiles
assert "InstallMetadata(name)" in reg_sanity and "VerifyTreeAccess" in reg_sanity
assert "kDwmOpenGlassRules" in reg_sanity and "partial cleanup" in profiles
assert "GetCurrentThemeName" in style and "WinVerifyTrust" in style and "Microsoft Corporation" in style
assert "FILE_ATTRIBUTE_REPARSE_POINT" in style and "State::Unknown" in style
assert "IsStockStyleName" not in sut_native and "CustomVisualStyleActive" not in sut_native
assert "Shared::SystemVisualStyle::CurrentForMaintenance()" in sut_native
assert "GLASSSETTINGSINSTALLED=1" not in engine
assert "GlassMaintenanceCache::Publish" in engine and "IDR_MAINTENANCE_UNINS" in rc
assert "PROTECTED_DACL_SECURITY_INFORMATION" in cache and "WaitForSingleObject" in cache
assert "Registrations(code,all)" in cache and "RegistrationState" in cache and "PackageCode" in cache
assert "MsiSourceListAddSourceExW" in cache and "INSTALLPROPERTY_PACKAGENAME" in cache
assert "ModifyPath" in msi and "quietCommand.c_str()" in msi and "CurrentFile" in msi

# 2026-11-04 UI review: native-OS shell suffix is shared between both masters.
assert 'IDS_COMP_SHELL_SUFFIX " и оболочки Windows"' in read("res/ProductStrings.rcinc")
assert 'IDS_COMP_SHELL_SUFFIX " and the Windows shell"' in read("res/ProductStrings.rcinc")
assert 'ProductStrings.rcinc' in read("res/GlassSetup.rc") and 'ProductStrings.rcinc' in unins_rc
assert "platform64==os64" in read("src/GlassProductUiPolicy.h")
assert "PlatformDescription" in setup and "PlatformDescription" in unins
assert "ManualOpenGlass" in read("src/GlassProductPlan.h")
assert "Средство для использования неподписанных визуальных стилей. Установка в System32." in rc

# Fixed clean build order: Release x64 -> Release x86. First-pass Unins
# must not consume a Native DLL that is produced only by the second pass.
assert "IDR_UNLOCKER_" not in unins_rc
assert "ConfigureEmbeddedUnlocker" not in unins
assert "IDR_MAINTENANCE_UNINS,&cacheFailure)" in engine
assert "nativeX86Resource" not in cache and "GlassSettings.Native.x86.dll" not in cache
assert "EmbeddedNativeDll" not in util
assert "Первичная подготовка этой ветки" not in read("README.md")

# Maintenance fixes: machine installation facts, installed-only helpers,
# explicit MSI absent-state handling and failure propagation to HRESULT UI.
platform_registry=read("../GlassSettings/Core/Platform/PlatformInstallRegistry.cs")
assert "RegistryHive.CurrentUser" not in platform_registry
for start,end in (("RegistryPathRead ReadPlatformInstallPath", "DWORD ValidateRegisteredInstallRoot"),
                  ("bool WritePlatformInstallPath", "bool ClearPlatformInstallPath"),
                  ("bool WriteOpenGlassInstallPath", "bool ClearOpenGlassInstallPath")):
    assert "HKEY_CURRENT_USER" not in section(util,start,end)
assert "state==INSTALLSTATE_ABSENT) continue" in msi
assert "i,installedCode,nullptr,nullptr,nullptr" in msi
assert "chosen.secureUxTheme && installed_.settings" in unins
assert "chosen.secureUxTheme && installed_.settings && SecureUxThemeManaged()" in unins
assert "SecureUxThemeHook::Uninstall" not in sut_native
assert "DeleteFileW(patcherPath" not in sut_native
assert "NativeHostInterop.SecureUxThemeActivate(string.Empty, reactivate)" in sut_service
assert "NativeHostInterop.SecureUxThemeDeactivate()" in sut_service
assert "InstallFromCache" not in sut_service and "UninstallProduct" not in sut_service
msi_service=read("../GlassSettings/Services/SecureUxThemeMsiService.cs")
assert "MsiEnumRelatedProductsW" in msi_service
assert "RunMsiexec" not in msi_service and "UninstallProduct" not in msi_service
assert "REG_OPTION_BACKUP_RESTORE" in sut_native and "RegSetKeySecurity(" not in sut_native
assert "SameRegistrationValue" in sut_native and "registration rollback incomplete" in sut_native
assert "CopyFileW(" not in sut_native and "DeleteFileW(" not in sut_native
assert "schtasks" not in sut_native and "Cleanup.cmd" not in sut_native
assert "q.ComProxyHijacked || q.SystemDll4Present || q.ThemeUiProxyPresent || q.OfficialMsiPresent" in sut_native
assert "glassRepackedSut &&" in sut_inspect
assert "Shared::SecureUxThemeInventory::IsInstalled()" in setup
assert "LegacySecureUxThemePayloadPresent" not in setup

# Generic notes are not proof of a startup failure. Cache errors are attributed.
assert "не всё запустилось" not in rc+ui and "not everything started" not in rc+ui
assert "завершающим действиям установщика" in rc and "завершающим действиям установщика" in ui
cache_diag=read("src/InstallSupport/GlassCacheDiagnostics.h")
assert "Diag::Publication publication(failureDetail)" in cache
assert 'L"validate-packages-parent"' in cache and 'L"create-owned-root"' in cache
assert 'L"ValidateOwnerAndDacl"' in cache and 'L"Policy"' in cache
assert "SID=" in cache and "mask=" in cache and "owner=" in cache
assert "stage=" in cache_diag and "operation=" in cache_diag and "CodeText" in cache_diag
assert "context.first" in cache_diag and "first failure" in cache_diag
assert "ERROR_CANCELLED" in util and "DeleteFileW(destPath.c_str());SetLastError(failure)" in util

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
