#pragma once

#include <GlassPlatformTask.h>
#include <string>

namespace GlassInstallHost {

bool StopGlassPlatform(GlassShared::GlassPlatformTask::PlatformArch arch) noexcept;
bool StopOpenGlassHost() noexcept;
bool StopAllForUninstall(
    const std::wstring& x64InstallDir,
    const std::wstring& x86InstallDir,
    bool stopX64,
    bool stopX86,
    bool stopOpenGlass) noexcept;

bool EnsureGlassAutorunTask(
    GlassShared::GlassPlatformTask::PlatformArch arch,
    const std::wstring& installDir,
    bool enableTask,
    bool applyPowerIndependence) noexcept;

bool EnsureOpenGlassService(
    const std::wstring& openGlassDir,
    DWORD desiredStartType) noexcept;
bool StartOpenGlassService() noexcept;

bool RemoveOpenGlassServiceAndTasks(bool removeTasks = true) noexcept;
bool RemovePlatformAutorunTasks(bool removeX64, bool removeX86) noexcept;

} // namespace GlassInstallHost
