#pragma once
#include "WndPch.h"
#include "uah.h"

constexpr UINT MN_SIZEWINDOW{ 0x01E2 };
constexpr UINT MN_SELECTITEM{ 0x01E5 };
constexpr UINT MN_BUTTONUP{ 0x01EF };

namespace WndCompat
{
    // Подключить сабкласс к popup-меню
    void Menu_Attach(HWND hWnd);

    // Идемпотентно: если сабкласс уже стоит — не дублировать (для EVENT_OBJECT_SHOW).
    void Menu_AttachIfNeeded(HWND hWnd);

    // Снять сабкласс
    void Menu_Detach(HWND hWnd);

    /// После uncloak flyout-анимации: TraverseLog — immersive NC; остальные типы — no-op (рамка из GlassCore FlyoutFrame).
    void Menu_FlyoutBorderRefresh(HWND hWnd) noexcept;
}
