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
std::wstring TrimTrailingSlashes(std::wstring path);
// Best-effort cleanup of empty directories only; never deletes files, follows
// reparse points, or enters a directory retained by another product.
void PruneEmptyDirectories(const std::wstring& root,
    const std::vector<std::wstring>& protectedRoots = {}) noexcept;
bool SameInstallPath(const std::wstring& left, const std::wstring& right) noexcept;

bool QueryPlatformInstallPath(bool x64, std::wstring& out) noexcept;
void WritePlatformInstallPath(bool x64, const std::wstring& installDir) noexcept;
bool ClearPlatformInstallPath(bool x64) noexcept;

bool QueryOpenGlassInstallPath(std::wstring& out) noexcept;
void WriteOpenGlassInstallPath(const std::wstring& x64RootDir, const std::wstring& openGlassDir) noexcept;
bool ClearOpenGlassInstallPath() noexcept;

bool StartMenuShortcutExists() noexcept;
bool CreateStartMenuShortcut(const std::wstring& targetExe, const std::wstring& workingDir) noexcept;
void DeleteStartMenuShortcut() noexcept;

bool InvokeShellExtRegistration(
    bool registerExtension,
    const std::wstring& x64Dir,
    const std::wstring& x86Dir,
    bool* registrationMayHaveChanged = nullptr) noexcept;

} // namespace GlassInstallUtil
