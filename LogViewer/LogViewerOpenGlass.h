#pragma once

// Запуск/останов OpenGlass: HKCU|HKLM\Software\StanislavKa\BlackWindowManager\InstallPath + \OpenGlass\OpenGlass.dll;
// иначе fallback — OpenGlass.dll рядом с LogViewer.exe. Rundll32 runas.
namespace LogViewer::OpenGlass
{
    [[nodiscard]] bool TryLaunchOpenGlassStartup() noexcept;
    [[nodiscard]] bool TryLaunchOpenGlassShutdown() noexcept;
    [[nodiscard]] bool IsLegacyHostTaskRunning() noexcept;
}
