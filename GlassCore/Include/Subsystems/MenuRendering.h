#pragma once
#include <windows.h>

namespace GlassCore::Menu::Rendering
{
    // Возвращает true, если отрисовка перехвачена и оригинал вызывать не нужно
    using PfnDrawThemeBackground = HRESULT(WINAPI*)(HTHEME, HDC, int, int, LPCRECT, LPCRECT);

    bool HandleDrawThemeBackground(
        HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
        LPCRECT pRect, LPCRECT pClipRect,
        decltype(&DrawThemeBackground) actualDTB);

    bool HandleCustomRendering(
        HDC hdc,
        int iPartId,
        int iStateId,
        const RECT& clipRect,
        const RECT& rect);

    // Внутренние popup‑хуки (E8)
    bool HandlePopupDrawItem(
        void* popupThis,
        HDC hdc,
        RECT* rc,
        int index);

    bool HandlePopupDrawItemCheck(
        void* popupThis,
        HDC hdc,
        RECT* rc,
        int index);

    bool HandlePopupDrawClientArea(
        void* popupThis,
        HDC hdc,
        RECT* rc);

    bool HandlePopupDrawNonClientArea(
        void* popupThis,
        HDC hdc,
        RECT* rc);
}
