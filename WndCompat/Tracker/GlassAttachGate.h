#pragma once

#include <windows.h>

namespace WndCompat::GlassAttachGate
{
    // Full: запрет attach поддерева под top-level из жёсткого списка (см. RootBackdropPolicy).
    // Исключение: класс ComboLBox — всегда true (popup списка, не привязан к exempt root).
    // Не Full: сразу true (без GetAncestor / реестра).
    // Live OFF (NoBackdrop): attach/регистрация HWND сохраняется; визуалы — через paint-gate и sweep.
    [[nodiscard]] bool ShouldAttachGlassToHwnd(HWND hWnd) noexcept;
}
