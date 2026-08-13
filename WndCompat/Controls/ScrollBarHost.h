#pragma once
#include <windows.h>

namespace WndCompat
{
    // Класс ScrollBar: без субкласса — только GlassMarkWindow (InjectionMode::Full).
    void ScrollBar_Attach(HWND hWnd);
    void ScrollBar_Detach(HWND hWnd);
    void ScrollBar_DetachAll();
    void ScrollBar_ApplyBackdropPolicyToTracked() noexcept;
}
