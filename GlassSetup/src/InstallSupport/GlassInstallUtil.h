#pragma once

#include <windows.h>
#include <atomic>
#include <GlassPlatformTask.h>
#include <string>

namespace GlassInstallUtil {

void ConfigureEmbeddedUnlocker(HINSTANCE instance, UINT x64ResourceId) noexcept;
void CleanupEmbeddedUnlocker() noexcept;
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
    bool* started = nullptr, bool* completed = nullptr) noexcept;
bool RunHidden(const wchar_t* file, const std::wstring& args) noexcept;

void CloseGlassSettingsProcesses() noexcept;
bool ImageRunning(const wchar_t* imageName) noexcept;

bool IsOs64Bit() noexcept;
std::wstring TrimTrailingSlashes(std::wstring path);

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
