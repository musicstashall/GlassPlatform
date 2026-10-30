#pragma once

#include <GlassPlatformTask.h>
#include <atomic>
#include <string>

namespace GlassInstallHost {

// A failed mutex query is not the same as a stopped platform.
bool QueryGlassPlatformRunning(GlassShared::GlassPlatformTask::PlatformArch arch, bool& running) noexcept;
// Stop the validated Scheduler task and wait for its host mutex. Once a stop
// was attempted, settle it even if cancellation arrives before returning.
bool StopGlassPlatform(GlassShared::GlassPlatformTask::PlatformArch arch,
    const std::wstring& installDir, const std::atomic_bool* cancelled = nullptr,
    bool* stopAttempted = nullptr, std::wstring* detail = nullptr) noexcept;
// Inspect real service/task status before sending any stop. Once requested,
// wait for the stop to settle even on cancellation so rollback can restore it.
bool StopOpenGlassHost(const std::atomic_bool* cancelled = nullptr,
                       bool* stopRequested = nullptr) noexcept;
bool StopAllForUninstall(
    const std::wstring& x64InstallDir,
    const std::wstring& x86InstallDir,
    bool stopX64,
    bool stopX86,
    bool stopOpenGlass,
    const std::atomic_bool* cancelled = nullptr,
    bool* stopAttemptedX64 = nullptr,
    bool* stopAttemptedX86 = nullptr,
    const bool* wasRunningX64 = nullptr,
    const bool* wasRunningX86 = nullptr) noexcept;

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
