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
                    "IDR_SLIDE_EXPLORER", "IDR_UNLOCKER_X64"):
    assert resource_id in ids and resource_id in rc and resource_id in unins_rc, resource_id
assert "IDR_UNLOCKER_X86" in ids and "IDR_UNLOCKER_X86" in rc
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
assert "WizardUnlockState.h" in project and "WizardUnlockState.h" in read("GlassSetup.vcxproj.filters")
assert "progress_tests.cpp" in project and "removal_order_tests.cpp" in project and "unlock_state_tests.cpp" in project
assert "GlassInstallRemoval.cpp" in project and "GlassRemovalOrder.h" in project
assert project.count("taskschd.lib") >= 2
assert 'ResourceCompile Include="GlassUnins.rc"' in unins_project
assert "GLASS_HAVE_UNLOCKER_X64" in project and "GLASS_HAVE_UNLOCKER_X86" in project
assert "GLASS_SETUP_NATIVE_MONITOR" in project and "IDR_UNLOCKER_X86" in project
assert "GlassSettings.Native.dll" in project and "GlassSettings.Native.dll" in unins_rc
assert "<LinkLibraryDependencies>true</LinkLibraryDependencies>" in project
assert "<AdditionalDependencies>GlassShared.lib;" not in project
solution = (repo / "GlassProject.sln").read_text(encoding="utf-8")
setup_guid = "{7E4C1A90-6B2D-4F58-9C31-0A6E5D4B2F18}"
assert f"{setup_guid}.Debug|x64.Build.0" not in solution
assert f"{setup_guid}.Debug|Any CPU.Build.0" not in solution
assert f"{setup_guid}.Debug|x86.Build.0 = Debug|Win32" in solution
assert f"{setup_guid}.Release|x86.Build.0 = Release|Win32" in solution

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
# Installation is a desired-state operation, not a skip-only selection.
policy = read("src/GlassProducts.h")
assert "RemovalForInstall" in policy and "not an uninstall request" not in policy
sequence = section(engine, "bool InstallEngine::RunMsiSequence()", "bool InstallEngine::LaunchAndVerifySelectedProducts()")
for token in ("kProductSettingsX64", "kProductSettingsX86", "kProductOpenGlass", "kLegacyOpenGlass",
              "kProductPlatformX86", "kLegacyPlatformX86", "ClearPlatformInstallPath(false)",
              "ClearOpenGlassInstallPath()", "ProductWasSnapshotted"):
    assert token in sequence, token
assert sequence.index("NoteModified(rollbackSession_, step.removeCode)") < sequence.index("UninstallProduct(step.removeCode, &sink")
assert "cancelRequested=&cancelRequested_" in sequence
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
assert worker.index("CreateRollbackSnapshot()") < worker.index("PrepareProductRemoval()") < worker.index("StopRunningHosts()")
assert worker.index("LaunchAndVerifySelectedProducts()") < worker.index("GlassInstallRemoval::Commit")
assert "GlassInstallRemoval::Restore" in worker and "restoreRemovalArtifacts()" in worker
success_restore = section(engine, "bool InstallEngine::RestorePriorHosts(", "DWORD WINAPI InstallEngine::WorkerThread(")
assert "selection_.platform32 && prior_.x86WasRunning" in success_restore and "if (selectedOnly) return ok" in success_restore
purge = engine[engine.index("bool InstallEngine::PurgeRemnants()"):]
assert "DeleteTree(" not in purge and "RemoveDirectoryTree(" not in purge
warnings = section(setup, "    std::wstring ComponentWarnings()", "    std::wstring ComponentsJson()")
assert "IDS_COMP_X86_WARNING" in warnings and "IDS_COMP_GS_WARNING" in warnings and 'L"\\n"' in warnings
assert "ComponentStateId" in setup and "installedSelection_.platform32" in setup and "IDS_COMP_STATE_REMOVE" in rc
assert "ActionForInstall" in policy and "ComponentBadgeId" in setup and "ComponentBadgeTone" in setup
for action in ("INSTALL", "UPDATE", "REMOVE"):
    assert f"IDS_COMP_BADGE_{action}" in setup and rc.count(f"IDS_COMP_BADGE_{action}") == 2
    assert f".comp-badge.action-{action.lower()}" in ui
assert "item.badgeTone" in ui and "comp-lock" in ui and "item.lockTitle" in ui
assert "action.append(componentLock(" in ui and "badge.textContent=String(item.badge);action.append(badge)" in ui
assert ".comp-action{display:flex" in ui and "top:198px" in ui and "top:440px" in ui
assert "#a7dfff" in ui and "#efb94f" in ui  # independent blue-and-gold lock, not badge-tinted
assert "DrawComponentActionBadge(dc,selected,installed" in setup and "DrawMandatoryLock(dc," in setup
assert "item.state" in ui and "white-space:pre-line" in ui
assert "DrawLabel(dc,Text(IDS_COMP_X86_WARNING)" in setup and "DrawLabel(dc,Text(IDS_COMP_GS_WARNING)" in setup

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
assert undo.index("GlassInstallMsi::UninstallProduct") < undo.index("++productsProcessed") < undo.index("productsProcessed*8000/productCount")
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
assert "ShouldStartOpenGlass(openGlassStartup_)" in engine and "Snapshot OpenGlass path unavailable" in rollback
assert "protectedHandles" in util and "FILE_FLAG_OPEN_REPARSE_POINT" in util and "FileDispositionInfo" in util
assert "PruneEmptyDirectories(openGlassDir_, keptRoots)" in engine and "PruneEmptyDirectories(openGlassDir_,keptRoots)" in unins
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
