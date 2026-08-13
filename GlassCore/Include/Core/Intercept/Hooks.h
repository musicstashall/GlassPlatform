#pragma once
#include <windows.h>

namespace GlassCore::Hooks
{
    // Инициализация инфраструктуры хуков/модульных уведомлений; возвращает true при успехе
    bool Install();
    void Uninstall();

    // Регистрация callback'ов по окнам (оставляем как локальный механизм для будущих сценариев)
    using HookCallback = void(*)(HWND);
    void RegisterWindowCallback(HookCallback cb);
}
