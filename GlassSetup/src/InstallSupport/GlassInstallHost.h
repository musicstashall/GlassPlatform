#pragma once

#include <GlassPlatformTask.h>
#include <atomic>
#include <string>

namespace GlassInstallHost {

bool StopGlassPlatform(GlassShared::GlassPlatformTask::PlatformArch arch) noexcept;
bool StopOpenGlassHost() noexcept;
bool StopAllForUninstall(
    const std::wstring& x64InstallDir,
    const std::wstring& x86InstallDir,
    bool stopX64,
    bool stopX86,
    bool stopOpenGlass,
    const std::atomic_bool* cancelled = nullptr) noexcept;

bool EnsureGlassAutorunTask(
    GlassShared::GlassPlatformTask::PlatformArch arch,
    const std::wstring& installDir,
    bool enableTask,
    bool applyPowerIndependence) noexcept;

// Distinguishes a missing service from an access/configuration failure.
bool QueryOpenGlassServiceStartType(bool& exists, DWORD& startType) noexcept;

bool EnsureOpenGlassService(
    const std::wstring& openGlassDir,
    DWORD desiredStartType) noexcept;
bool StartOpenGlassService() noexcept;
// Start a previously running service without leaving a disabled startup type enabled.
bool StartOpenGlassServicePreservingStartType(DWORD startType) noexcept;
bool RestoreOpenGlassServiceStartType(DWORD startType) noexcept;

bool RemoveOpenGlassServiceAndTasks(bool removeTasks = true) noexcept;
bool RemovePlatformAutorunTasks(bool removeX64, bool removeX86) noexcept;

} // namespace GlassInstallHost
