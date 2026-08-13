#pragma once

#include <cstdint>

#include <Windows.h>

namespace GlassCore::Hooks::DebugDetourHooks
{
    // Битовая маска слотов (1ull << N соответствует индексу N в SetEnabled / внутри DebugDetourHooks.cpp).
    // Номера слотов (N) — часть публичного API; сгруппированы ниже по смыслу, не по возрастанию N.
    // Тип маски — uint64_t (слоты 0..63); фактически используются 0..33.

    // --- Фон: тема и DirectUI ---
    inline constexpr uint64_t DDH_DrawThemeBackground = 1ull << 0;  // uxtheme: DrawThemeBackground
    // dui70.dll: DirectUI::Element::PaintBackground (декорированное имя; ABI: this, HDC, Value*, RECT×4).
    // Включать вручную: SetEnabled(DDH_PaintBackground | …, true).
    inline constexpr uint64_t DDH_PaintBackground = 1ull << 10;
    inline constexpr uint64_t DDH_DrawThemeBackgroundEx = 1ull << 22; // uxtheme: DrawThemeBackgroundEx (DTBGOPTS)

    // --- Текст: user32, gdi32, uxtheme ---
    inline constexpr uint64_t DDH_DrawThemeText = 1ull << 1;
    inline constexpr uint64_t DDH_DrawThemeTextEx = 1ull << 2;
    inline constexpr uint64_t DDH_DrawTextW = 1ull << 3;
    inline constexpr uint64_t DDH_DrawTextExW = 1ull << 4;
    inline constexpr uint64_t DDH_ExtTextOutW = 1ull << 5;
    inline constexpr uint64_t DDH_TextOutW = 1ull << 8;
    // uxtheme ordinal #126 — DrawTextWithGlow (сигнатура как в RenderText.cpp).
    inline constexpr uint64_t DDH_DrawTextWithGlow = 1ull << 23;

    // --- Пиксельные копии и заливки (GDI / user32) ---
    inline constexpr uint64_t DDH_BitBlt = 1ull << 6;
    inline constexpr uint64_t DDH_PatBlt = 1ull << 9;
    inline constexpr uint64_t DDH_Rectangle = 1ull << 27; // gdi32: Rectangle (тест UpDown buddy chrome)
    inline constexpr uint64_t DDH_FillRect = 1ull << 28;  // user32: FillRect
    inline constexpr uint64_t DDH_StretchBlt = 1ull << 29; // gdi32: StretchBlt

    // --- Buffered paint (uxtheme) — зонд compose / places-bar ---
    inline constexpr uint64_t DDH_BeginBufferedPaint = 1ull << 30;
    inline constexpr uint64_t DDH_EndBufferedPaint = 1ull << 31;
    inline constexpr uint64_t DDH_BufferedPaintClear = 1ull << 32;
    inline constexpr uint64_t DDH_BufferedPaintSetAlpha = 1ull << 33;

    // Набор для зонда buffered paint (idle/hover places-bar).
    inline constexpr uint64_t DDH_BufferedPaintProbe =
        DDH_BeginBufferedPaint | DDH_EndBufferedPaint |
        DDH_BufferedPaintClear | DDH_BufferedPaintSetAlpha;

    // --- Окно: анимация и класс ---
    inline constexpr uint64_t DDH_AnimateWindow = 1ull << 7;
    // user32: SetClassLongPtrW — GCLP_HBRBACKGROUND, GCL_STYLE и др.
    inline constexpr uint64_t DDH_SetClassLongPtrW = 1ull << 14;

    // --- DWM (dwmapi) ---
    inline constexpr uint64_t DDH_DwmExtendFrameIntoClientArea = 1ull << 11;
    inline constexpr uint64_t DDH_DwmSetWindowAttribute = 1ull << 13; // DWMWA, backdrop, цвета заголовка
    inline constexpr uint64_t DDH_DwmEnableBlurBehindWindow = 1ull << 25; // dwmapi: DwmEnableBlurBehindWindow (legacy blur)
    // user32: SetWindowCompositionAttribute (SWCA / accent policy и др.; GetProcAddress в Debug-сборке).
    inline constexpr uint64_t DDH_SetWindowCompositionAttribute = 1ull << 26;

    // --- Цвет и палитра: тема, система, кисти, сток (uxtheme, user32, gdi32) ---
    inline constexpr uint64_t DDH_GetThemeColor = 1ull << 12;
    inline constexpr uint64_t DDH_SetDCBrushColor = 1ull << 15;
    inline constexpr uint64_t DDH_SetBkColor = 1ull << 16;
    inline constexpr uint64_t DDH_CreateSolidBrush = 1ull << 18;
    inline constexpr uint64_t DDH_GetSysColorBrush = 1ull << 19;
    inline constexpr uint64_t DDH_GetStockObject = 1ull << 20;
    inline constexpr uint64_t DDH_GetSysColor = 1ull << 21;

    // --- Смена системной темы: .msstyles + схемы (uxtheme #65, ThemeHelper::SetSystemVisualStyle) ---
    inline constexpr uint64_t DDH_SetSystemVisualStyle = 1ull << 24;

    // --- Растр: DIB → bitmap (gdi32) ---
    inline constexpr uint64_t DDH_SetDIBits = 1ull << 17;

    // Трассировка отрисовки ComboLBox (без AnimateWindow — конфликт с Detour в ComboLBoxHooks).
    // Набор битов фиксирован; без DrawThemeBackgroundEx / DrawTextWithGlow (другие сценарии).
    inline constexpr uint64_t DDH_ComboLBoxPaintProbe =
        DDH_DrawThemeBackground | DDH_DrawThemeText | DDH_DrawThemeTextEx |
        DDH_DrawTextW | DDH_DrawTextExW | DDH_ExtTextOutW | DDH_BitBlt |
        DDH_TextOutW | DDH_PatBlt;

    // Все слоты (0..33).
    inline constexpr uint64_t DDH_All =
        DDH_DrawThemeBackground | DDH_PaintBackground | DDH_DrawThemeBackgroundEx |
        DDH_DrawThemeText | DDH_DrawThemeTextEx | DDH_DrawTextW | DDH_DrawTextExW |
        DDH_ExtTextOutW | DDH_TextOutW | DDH_DrawTextWithGlow |
        DDH_BitBlt | DDH_PatBlt | DDH_FillRect | DDH_StretchBlt | DDH_AnimateWindow | DDH_SetClassLongPtrW |
        DDH_DwmExtendFrameIntoClientArea | DDH_DwmSetWindowAttribute | DDH_DwmEnableBlurBehindWindow |
        DDH_SetWindowCompositionAttribute |
        DDH_GetThemeColor | DDH_SetDCBrushColor | DDH_SetBkColor | DDH_CreateSolidBrush |
        DDH_GetSysColorBrush | DDH_GetStockObject | DDH_GetSysColor | DDH_SetDIBits |
        DDH_SetSystemVisualStyle | DDH_Rectangle |
        DDH_BufferedPaintProbe;

    // Только DirectUI (dui70): Element::PaintBackground — для сравнения с GDI-трассировкой.
    inline constexpr uint64_t DDH_Dui70BackgroundProbe = DDH_PaintBackground;

#if defined(_DEBUG)
    // Включает или выключает перехват для каждого установленного в mask бита.
    // На слот ведётся ref-count: повторный SetEnabled(..., true) требует столько же false.
    [[nodiscard]] HRESULT SetEnabled(uint64_t mask, bool enable) noexcept;
#else
    [[nodiscard]] inline HRESULT SetEnabled(uint64_t /*mask*/, bool /*enable*/) noexcept
    {
        return S_OK;
    }
#endif
}
