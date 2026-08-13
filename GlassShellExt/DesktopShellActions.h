#pragma once

#include <Windows.h>

#include <GlassPlatformTask.h>

#include <string>

namespace GlassDesktopActions
{
    void BindModule(HINSTANCE hInst) noexcept;

    [[nodiscard]] std::wstring LoadMenuString(UINT resourceId, const wchar_t* fallback) noexcept;

    /// <summary>OS x64 + ShellExt x64: два пункта меню (x86 и x64).</summary>
    [[nodiscard]] bool ShowDualPlatformMenu() noexcept;

    [[nodiscard]] std::wstring GetPlatformToggleTitle() noexcept;
    [[nodiscard]] std::wstring GetPlatformToggleTitle(
        GlassShared::GlassPlatformTask::PlatformArch arch) noexcept;

    [[nodiscard]] bool IsPlatformToggleMenuEnabled() noexcept;
    [[nodiscard]] bool IsPlatformToggleMenuEnabled(
        GlassShared::GlassPlatformTask::PlatformArch arch) noexcept;

    [[nodiscard]] UINT GetPlatformToggleMenuState() noexcept;
    [[nodiscard]] UINT GetPlatformToggleMenuState(
        GlassShared::GlassPlatformTask::PlatformArch arch) noexcept;

    void ExecutePlatformToggle(HWND owner) noexcept;
    void ExecutePlatformToggle(
        HWND owner,
        GlassShared::GlassPlatformTask::PlatformArch arch) noexcept;

    [[nodiscard]] bool IsOpenGlassMenuVisible() noexcept;

    [[nodiscard]] std::wstring GetOpenGlassToggleTitle() noexcept;

    [[nodiscard]] UINT GetOpenGlassToggleMenuState() noexcept;

    void ExecuteOpenGlassToggle(HWND owner) noexcept;

    void LaunchGlassSettings(HWND owner) noexcept;
}
