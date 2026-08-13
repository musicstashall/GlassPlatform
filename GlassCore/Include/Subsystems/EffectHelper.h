#pragma once
#include <windows.h>
#include <dwmapi.h>
#include <cstdint>

namespace GlassCore::EffectHelper
{
    // Вспомогательные типы
    enum class EffectType
    {
        None,
        Transparent,
        Solid,
        Blur,
        AcrylicBlur,
        ModernAcrylicBlur,
        Acrylic,
        Mica,
        MicaAlt
    };

    // Установка фонового эффекта окна
    // dropShadow — включить тень в рамке
    // tintColor — ARGB цвет (0xAARRGGBB)
    // effect — тип эффекта
    // skipNcRendering — пропустить TriggerWindowNCRendering (для отложенного вызова)
    void SetWindowBackdrop(HWND hwnd, BOOL dropShadow, DWORD tintColor, EffectType effect, bool skipNcRendering = false, bool flyoutBackdrop = false);

    // Включить/выключить тёмную тему для окна (Immersive dark)
    void EnableWindowDarkMode(HWND hwnd, BOOL dark);

    // Сделать окно прозрачным (DWM blur behind)
    void DwmMakeWindowTransparent(HWND hwnd, BOOL enable);

    // Принудительный перерис NC (рамки)
    void TriggerWindowNCRendering(HWND hwnd);
}
