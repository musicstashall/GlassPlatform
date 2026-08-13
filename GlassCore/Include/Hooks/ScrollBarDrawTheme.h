#pragma once

#include <Windows.h>

namespace GlassCore::ScrollBarDrawTheme
{
    // SBP_SIZEBOX (vssym32.h)
    inline constexpr int kScrollBarPartSizeBox = 10;

    [[nodiscard]] bool IsScrollBarTrackBackgroundPart(int iPartId) noexcept;
    [[nodiscard]] bool IsScrollBarInteractivePart(int iPartId) noexcept;

    // Пересечение pRect с pClipRect (если задан). false — нечего рисовать (пустое пересечение).
    [[nodiscard]] bool TryGetScrollBarDrawRect(LPCRECT pRect, LPCRECT pClipRect, RECT& outRect) noexcept;

    void LogScrollBarDrawThemeCall(LPCWSTR logTag, int iPartId, int iStateId) noexcept;

    [[nodiscard]] bool TryDrawScrollBarInteractiveGlass(
        HRESULT(WINAPI* orig)(HTHEME hTheme, HDC hdc, int iPartId, int iStateId, LPCRECT pRect, LPCRECT pClipRect),
        HTHEME      hTheme,
        HDC         hdcDest,
        int         iPartId,
        int         iStateId,
        const RECT& drawRc) noexcept;

    // SIZEBOX: BLACKNESS + orig поверх (путь для не вложенных скроллов — см. ScrollBarThemeHooks).
    [[nodiscard]] HRESULT DrawScrollBarSizeBoxBlackThenOrig(
        HRESULT(WINAPI* orig)(HTHEME hTheme, HDC hdc, int iPartId, int iStateId, LPCRECT pRect, LPCRECT pClipRect),
        HTHEME  hTheme,
        HDC     hdc,
        int     iPartId,
        int     iStateId,
        const RECT& r) noexcept;
}
