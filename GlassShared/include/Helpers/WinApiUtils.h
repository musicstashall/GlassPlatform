#pragma once
#include <windows.h>
#include <uxtheme.h>
#include <cstddef>
#include <string>
#include <string_view>

namespace Shared::WinApiUtils
{
    // =========================================================================
    // Создание полупрозрачной кисти для альфа-композиции
    // =========================================================================

    // Создает 32bpp DIB с pre-multiplied alpha и возвращает pattern brush
    // Используется для рисования подсветки меню (COLOR_HIGHLIGHT) поверх DWM blur
    HBRUSH CreateSolidColorBrushWithAlpha(COLORREF color, UCHAR alpha);

    // =========================================================================
    // Проверка наличия темы оформления
    // =========================================================================

    // Проверяет, доступна ли тема оформления (UxTheme) для окна
    // themeName: "Menu", "Button", "Edit" и т.д.
    // Возвращает true, если OpenThemeData успешен
    bool IsThemed(HWND hWnd, std::wstring_view themeName);

    // Имя файла модуля без пути (для сравнения с L"USER32.dll", L"COMCTL32.dll" и т.д.)
    [[nodiscard]] std::wstring GetModuleFileBaseName(HMODULE hModule) noexcept;

    HRESULT EnableBlurBehindWindow(HWND hWnd, bool bEnable = true, bool bTransition = false, HRGN hRgn = 0);

    /// DwmExtendFrameIntoClientArea с margins {-1,-1,-1,-1} (всё окно) или сброс {0,0,0,0}.
    HRESULT ExtendFrameIntoEntireClientArea(HWND hWnd, bool bEnable) noexcept;

    // GCLP_HBRBACKGROUND → сплошной RGB: псевдо-индекс системной кисти (COLOR_*+1) или BS_SOLID HBRUSH.
    // false: NULL в классе, не OBJ_BRUSH, не BS_SOLID, невалидный дескриптор.
    [[nodiscard]] bool TryGetClassBackgroundSolidRgb(HWND hWnd, COLORREF& outRgb) noexcept;

    // =========================================================================
    // Оригинальная отрисовка темы в 32bpp DIB и композиция GdiAlphaBlend
    // =========================================================================

    using DrawThemeBackground_pfn = HRESULT(WINAPI*)(HTHEME, HDC, int, int, LPCRECT, LPCRECT);
    using DrawThemeBackgroundEx_pfn = HRESULT(WINAPI*)(HTHEME, HDC, int, int, LPCRECT, const DTBGOPTS*);

    // Рисует orig в offscreen DIB и накладывает на hdcDest с постоянной альфой источника.
    // sourceConstantAlpha == 0   — без DIB и без AlphaBlend: PatBlt(BLACKNESS) в пересечении pRect∩pClipRect.
    // sourceConstantAlpha == 255 — без DIB и без AlphaBlend: прямой вызов orig на hdcDest.
    // maxPixelCount — защита от чрезмерных буферов (по умолчанию 4e6 пикселей).
    [[nodiscard]] bool DrawThemeBackgroundWithConstantAlpha(
        DrawThemeBackground_pfn orig,
        HTHEME hTheme,
        HDC hdcDest,
        int partId,
        int stateId,
        LPCRECT pRect,
        LPCRECT pClipRect,
        BYTE sourceConstantAlpha,
        size_t maxPixelCount = 4'000'000u) noexcept;

    // Как в DrawThemeBackgroundWithConstantAlphaAndChromaKey для 32bpp DIB: A=0 и RGB!=0 -> A=255
    // (тот же first-pass, что сэмплит ThemeHelper::TryGetTabChromaKey*).
    void FixBgraForThemeChromaDibInPlace(RGBQUAD& p) noexcept;

    // Как DrawThemeBackgroundWithConstantAlpha, но после orig: выравнивание альфы (как у DrawEdge),
    // вырезание пикселей chromaKey ± chromaThreshold по RGB, премультипликация BGR и GdiAlphaBlend(AC_SRC_ALPHA).
    // Всегда offscreen (в т.ч. при sourceConstantAlpha==255), иначе маску по цвету не применить.
    // optChromaKeyedPixelCount: если задан, число пикселей, обнулённых как chroma.
    [[nodiscard]] bool DrawThemeBackgroundWithConstantAlphaAndChromaKey(
        DrawThemeBackground_pfn orig,
        HTHEME hTheme,
        HDC hdcDest,
        int partId,
        int stateId,
        LPCRECT pRect,
        LPCRECT pClipRect,
        COLORREF chromaKey,
        BYTE chromaThreshold,
        BYTE sourceConstantAlpha,
        size_t maxPixelCount = 4'000'000u,
        std::size_t* optChromaKeyedPixelCount = nullptr) noexcept;

    // Аналогично DrawThemeBackgroundWithConstantAlpha: 0 → PatBlt(BLACKNESS), 255 → orig на hdcDest.
    [[nodiscard]] bool DrawThemeBackgroundExWithConstantAlpha(
        DrawThemeBackgroundEx_pfn orig,
        HTHEME hTheme,
        HDC hdcDest,
        int partId,
        int stateId,
        LPCRECT pRect,
        const DTBGOPTS* pOptions,
        BYTE sourceConstantAlpha,
        size_t maxPixelCount = 4'000'000u) noexcept;

    using DrawEdge_pfn = BOOL(WINAPI*)(HDC, LPRECT, UINT, UINT);

    // Как DrawThemeBackgroundWithConstantAlpha: offscreen orig(DrawEdge) в 32bpp DIB, композиция GdiAlphaBlend.
    // Для пикселей с цветным RGB и rgbReserved==0 (типично для GDI на DIB) выставляется альфа 255 — иначе «пустые»
    // чёрные ячейки остаются полностью прозрачными при AC_SRC_ALPHA.
    // BF_ADJUST: *pRect обновляется по результату DrawEdge в локальных координатах; иначе — сдвиг левой границы
    // на shrinkLeftWhenNoAdjust (как у сплит-кнопки comctl32).
    [[nodiscard]] bool DrawEdgeWithConstantAlpha(
        DrawEdge_pfn orig,
        HDC hdcDest,
        LPRECT pRect,
        UINT edge,
        UINT grfFlags,
        BYTE sourceConstantAlpha,
        int shrinkLeftWhenNoAdjust = 2,
        size_t maxPixelCount = 4'000'000u) noexcept;
} // namespace Shared::WinApiUtils