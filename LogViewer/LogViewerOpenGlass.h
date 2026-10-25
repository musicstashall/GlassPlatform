#pragma once

// Запуск/останов OpenGlass: HKCU|HKLM\Software\StanislavKa\GlassPlatform\InstallPath\OpenGlass\OpenGlass.dll;
// иначе каталог LogViewer.exe\OpenGlass\OpenGlass.dll. Rundll32 runas.
namespace LogViewer::OpenGlass
{
    [[nodiscard]] bool TryLaunchOpenGlassStartup() noexcept;
    [[nodiscard]] bool TryLaunchOpenGlassShutdown() noexcept;
    [[nodiscard]] bool IsLegacyHostTaskRunning() noexcept;
}
