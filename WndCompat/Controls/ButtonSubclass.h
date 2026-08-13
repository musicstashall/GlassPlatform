#pragma once
#include "WndPch.h"
#include <GlassControlKind.h>
#include <GlassMessages.h>

namespace WndCompat
{
    // kind: только GlassControlKind::Button или WinForms_Button (без субкласса — только SetProp для PaintDispatcher).
    void Button_Attach(HWND hWnd, GlassShared::GlassControlKind kind);
    void Button_Detach(HWND hWnd);
    void Button_DetachAll();
    [[nodiscard]] bool Button_IsTracked(HWND hWnd) noexcept;
    void Button_ApplyBackdropPolicyToTracked() noexcept;
}
