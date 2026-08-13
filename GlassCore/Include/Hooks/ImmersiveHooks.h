// GlassCore/Include/Hooks/ImmersiveHooks.h
#pragma once
#include <windows.h>

namespace GlassCore::Hooks::Immersive
{
    void Prepare();
    void Startup();
    void Shutdown();

    // baseAddress — базовый адрес модуля, в чьём IAT ставим хуки.
    // Для Immersive меню (Explorer) обычно explorerframe.dll,
    // EnableHooks внутри перенаправляет на shell32.dll + explorer.exe.
    void EnableHooks(PVOID baseAddress, bool enable);
    void DisableHooks();
}