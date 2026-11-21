#pragma once
#include "WndPch.h"

namespace WndCompat
{
    void CoolBar_Attach(HWND hWnd);
    void CoolBar_Detach(HWND hWnd);
    void CoolBar_DetachAll();
    [[nodiscard]] bool CoolBar_IsTracked(HWND hWnd) noexcept;

    // Догоняющий проход по поддереву root-окна: окна класса CoolBar,
    // созданные до инжекции, иначе остаются без сабкласса.
    void CoolBar_CatchUp(HWND hRoot) noexcept;
}
