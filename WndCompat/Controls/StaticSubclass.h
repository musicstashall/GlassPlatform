#pragma once
#include "WndPch.h"
#include <GlassControlKind.h>

namespace WndCompat
{
    // kind: GlassControlKind::Static или WinForms_Static (без субкласса — только SetProp для PaintDispatcher).
    void Static_Attach(HWND hWnd, GlassShared::GlassControlKind kind);
    void Static_Detach(HWND hWnd);
    void Static_DetachAll();

    // Субкласс только для Static c изображением (SS_BITMAP/SS_ICON): перехват STM_SETIMAGE
    // и конвертация битмапа без альфы в 32bpp (Promise32BPP + опциональный chroma Root\Control\Static).
    // Ставится в дополнение к Static_Attach; проверять StaticHasImageStyle перед вызовом.
    [[nodiscard]] bool StaticHasImageStyle(HWND hWnd) noexcept;
    void StaticImage_Attach(HWND hWnd);
    void StaticImage_Detach(HWND hWnd);
    void StaticImage_DetachAll();
}
