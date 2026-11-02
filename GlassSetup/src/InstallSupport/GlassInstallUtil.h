#pragma once

#include <windows.h>
#include <atomic>
#include <GlassPlatformTask.h>
#include <string>
#include <vector>

namespace GlassInstallUtil {

void ConfigureEmbeddedUnlocker(HINSTANCE instance, UINT x64ResourceId, UINT x86ResourceId = 0) noexcept;
void CleanupEmbeddedUnlocker() noexcept;
// Only the installer embeds an x86 Native helper; never fall back to a
// potentially user-writable installed path for an elevated in-process DLL.
std::wstring EmbeddedNativeDllX86() noexcept;
// Creates a new, administrator/SYSTEM-only directory under Windows Temp.
bool CreateSecureTempDirectory(const wchar_t* prefix, std::wstring& out) noexcept;

bool ExtractResourceToFile(
    HINSTANCE instance,
    UINT resourceId,
    const wchar_t* resourceType,
    const std::wstring& destPath,
    const std::atomic_bool* cancelled = nullptr) noexcept;

bool PeMatches(const std::wstring& path, WORD machine, const char* exportName) noexcept;
// Protocol marker and rundll32 entry point for exact installer-selected paths.
inline constexpr char kInstallerShellExport[] = "GlassSettings_ShellExtRegisterSetupV1W";
inline constexpr char kInstallerSignerTrustExport[] = "GlassSettings_ShellSignerTrustSetupV1W";
// Uninstaller entry point: SecureUxTheme deactivation «like GlassSettings» (stock style
// restore + unified GlassSettings_SecureUxThemeDeactivate).
inline constexpr char kInstallerSecureUxThemeExport[] = "GlassSettings_SecureUxThemeSetupV1W";
// Exit-code bits of kInstallerSecureUxThemeExport (see NativeHost.h).
inline constexpr DWORD kSecureUxThemeExitRebootRequired = 2u;
inline constexpr DWORD kSecureUxThemeExitThemeNotRestored = 4u;
inline constexpr DWORD kSecureUxThemeExitDeactivateFailed = 8u;
inline constexpr DWORD kSecureUxThemeExitExternalInstall = 16u;
std::wstring NativeRundll64();
std::wstring System64ExePath(const wchar_t* exeName);
std::wstring RundllPathForTaskArch(GlassShared::GlassPlatformTask::PlatformArch arch);

std::wstring NativeDllForArch(
    GlassShared::GlassPlatformTask::PlatformArch arch,
    const std::wstring& x64Dir,
    const std::wstring& x86Dir) noexcept;

bool RunHiddenCommand(const std::wstring& commandLine, DWORD* exitCode, DWORD timeoutMs,
    bool* started = nullptr, bool* completed = nullptr,
    const std::atomic_bool* cancelled = nullptr) noexcept;
bool RunHidden(const wchar_t* file, const std::wstring& args) noexcept;

void CloseGlassSettingsProcesses(const std::atomic_bool* cancelled = nullptr) noexcept;
bool QueryImageRunning(const wchar_t* imageName, bool& found) noexcept;
bool ImageRunning(const wchar_t* imageName) noexcept;
// A disabled legacy task can still have a running instance. Failure to inspect
// Scheduler is distinct from the absence of a task.
struct OpenGlassTaskState {
    bool found = false, anyDisabled = false, anyRunning = false;
    bool multipleTasks = false, multipleRunningInstances = false;
};
bool QueryOpenGlassTasks(OpenGlassTaskState& state) noexcept;
// Run an existing enabled legacy task without ever enabling a disabled task.
bool RunEnabledOpenGlassTask() noexcept;
// Stop only running instances; do not enable/delete tasks. `requested` is set
// once a stop may have been sent, so rollback can restore the previous host.
bool StopRunningOpenGlassTasks(bool& requested, const std::atomic_bool* cancelled = nullptr) noexcept;
bool QueryOpenGlassRunning(bool& running, const OpenGlassTaskState* knownTasks = nullptr) noexcept;
bool QueryOpenGlassFiles(const std::wstring& openGlassDir, const std::wstring& platformDir, bool& present) noexcept;

bool IsOs64Bit() noexcept;
// Default product roots for a first installation, derived from Windows rather
// than an inherited environment variable. x64 is empty on 32-bit Windows.
bool ResolveFixedPlatformRoots(std::wstring& x64, std::wstring& x86) noexcept;

struct InstallRoots {
    std::wstring x64, x86, openGlass;
    std::wstring sourceX64, sourceX86, sourceOpenGlass;
};
// Use saved product InstallPath values when present; otherwise use the
// system defaults. Task commands and uninstaller locations are never targets.
// Failure clears the result and sets GetLastError for diagnostics.
bool ResolveInstallRoots(InstallRoots& roots) noexcept;
std::wstring TrimTrailingSlashes(std::wstring path);
// Best-effort cleanup of empty directories only; never deletes files, follows
// reparse points, or enters a directory retained by another product.
void PruneEmptyDirectories(const std::wstring& root,
    const std::vector<std::wstring>& protectedRoots = {}) noexcept;
bool SameInstallPath(const std::wstring& left, const std::wstring& right) noexcept;
// Lexical, case-insensitive containment for an absolute registered MSI path.
// This does not establish file identity, signature or reparse-point safety.
bool IsWithinInstallRoot(const std::wstring& root, const std::wstring& path) noexcept;
// Reject existing reparse-point ancestors and multiply linked leaf files.
// This is a conservative preflight, not an atomic lock against other processes.
bool IsSafeInstallPath(const std::wstring& path) noexcept;

bool QueryPlatformInstallPath(bool x64, std::wstring& out) noexcept;
bool WritePlatformInstallPath(bool x64, const std::wstring& installDir) noexcept;
bool ClearPlatformInstallPath(bool x64) noexcept;

bool QueryOpenGlassInstallPath(std::wstring& out) noexcept;
bool WriteOpenGlassInstallPath(const std::wstring& x64RootDir, const std::wstring& openGlassDir) noexcept;
bool ClearOpenGlassInstallPath() noexcept;

bool StartMenuShortcutExists() noexcept;
bool CreateStartMenuShortcut(const std::wstring& targetExe, const std::wstring& workingDir) noexcept;
void DeleteStartMenuShortcut() noexcept;

// failureDetail (optional) receives a short technical, language-neutral
// description of why the helper failed (exit code, decoded HRESULT); it is
// meant for the log and the finish-page notes, not as a user-facing sentence.
// classicFallbackReason (optional, register only): non-zero when the helper
// succeeded on Windows 11 by registering the classic Win32 menu because the
// sparse package could not be deployed for this user; the value is the
// helper's reason id (8 = the msix signer certificate is not trusted).
bool InvokeShellExtRegistration(
    bool registerExtension,
    const std::wstring& x64Dir,
    const std::wstring& x86Dir,
    bool* registrationMayHaveChanged = nullptr,
    bool* registrationConflict = nullptr,
    std::wstring* failureDetail = nullptr,
    DWORD* classicFallbackReason = nullptr) noexcept;

// Technical, language-neutral text for a classic-fallback reason id.
std::wstring DescribeShellClassicFallback(DWORD reason);

// Windows 11 (build 22000+): the only Windows where the compact menu (sparse
// package) and therefore the author certificate are used.
bool IsWindows11OrGreater() noexcept;

// Runs GlassSettings_ShellSignerTrustSetupV1W of the embedded x64 helper: adds
// the certificate that signs <root>\GlassShellSparse\GlassShellSparse.msix to
// LocalMachine\TrustedPeople. Called only when the user accepted the Setup
// certificate page, after the files are in place and before the menu is
// registered. failureDetail (optional) receives a technical reason. Windows 11
// x64 only; a failure is a finish-page note, never a rollback.
bool InvokeShellSignerTrust(
    const std::wstring& x64Dir,
    const std::wstring& x86Dir,
    std::wstring* failureDetail = nullptr) noexcept;

// Result of the SecureUxTheme deactivation helper (kInstallerSecureUxThemeExport).
struct SecureUxThemeDeactivateRun
{
    bool ran = false;                  // the helper reached a decision
    bool rebootRequired = false;       // one-time System32 cleanup is pending
    bool themeNotRestored = false;     // custom visual style left active (deactivated anyway)
    bool skippedExternalInstall = false; // official SecureUxTheme owns the state; nothing changed
};

// Runs GlassSettings_SecureUxThemeSetupV1W of the helper matching the OS bitness
// (embedded x64 helper on x64, installed x86 helper on x86) and deactivates
// SecureUxTheme the way the GlassSettings toggle does. Called before GlassSettings
// products are removed (GlassSettings removal and full uninstall). Returns false when
// the helper is unavailable or reported a failure — the caller must abort with
// nothing removed.
bool InvokeSecureUxThemeDeactivate(
    const std::wstring& x64Dir,
    const std::wstring& x86Dir,
    SecureUxThemeDeactivateRun* run = nullptr,
    std::wstring* failureDetail = nullptr) noexcept;

} // namespace GlassInstallUtil
