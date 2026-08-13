#pragma once
#include <windows.h>

namespace GlassCore::Hooks::TaskbarHooks
{
    void OnTaskbarModuleLoaded(HMODULE module, PCWSTR baseName) noexcept;
    void OnTaskbarModuleUnloaded(HMODULE module, PCWSTR baseName) noexcept;
    /// IAT SWCA на Taskbar.dll: DWM — защита Glass accent; XAML — force clear (без тинта под зонами).
    void SyncHooksForTrayPaintPath() noexcept;
    void Shutdown() noexcept;
}
