#pragma once
#include <Windows.h>

namespace GlassSettingsNative
{
    /// Unlocker-стиль: CreateRemoteThread(FreeLibrary) для Glass*.dll в целевом PID.
    /// Порядок: Explorer → Taskbar → Wnd → Core → Compat. Несколько проходов.
    [[nodiscard]] bool ForceUnloadGlassModules(DWORD pid) noexcept;
}
