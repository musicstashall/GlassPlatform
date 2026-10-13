#pragma once
#include <windows.h>
#include <wil/resource.h>
#include <Providers/MenuContext.h>

namespace GlassCore::GdiUtils
{
    // Перо из LOGPEN (косметические/геометрические штрихи) — общий код Root/ListView.
    [[nodiscard]] wil::unique_hpen PenFromLogPen(const LOGPEN& lpIn) noexcept;

    // Геометрическое перо с 1×1 pattern DIB (premultiplied BGRA) — штрихи с альфой для layered/DWM.
    // bitmap должен жить не короче пера: держите struct в одной области видимости, пока перо в DC.
    struct LayeredStrokePen
    {
        wil::unique_hbitmap dib;
        wil::unique_hpen pen;

        LayeredStrokePen() = default;
        LayeredStrokePen(LayeredStrokePen&&) noexcept = default;
        LayeredStrokePen& operator=(LayeredStrokePen&&) noexcept = default;
        LayeredStrokePen(const LayeredStrokePen&) = delete;
        LayeredStrokePen& operator=(const LayeredStrokePen&) = delete;

        [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(pen); }
        [[nodiscard]] HPEN get() const noexcept { return pen.get(); }

        // Только PS_SOLID; иначе или при ошибке — пустой объект (вызывайте PenFromLogPen).
        [[nodiscard]] static LayeredStrokePen TryCreateSolid(const LOGPEN& lpIn, BYTE alpha) noexcept;

        // SOLID/DASH/DOT/DASHDOT/DASHDOTDOT с альфой; пробелы пунктира прозрачны (не закрашиваются).
        // INSIDEFRAME → SOLID. NULL/USERSTYLE/ошибка — пустой объект (вызывайте PenFromLogPen).
        [[nodiscard]] static LayeredStrokePen TryCreateStyled(const LOGPEN& lpIn, BYTE alpha) noexcept;
    };

    // Подготовка HBITMAP к GdiAlphaBlend: Promise32BPP + опционально вырезание фона (как BWM HandleMenuBitmap).
    struct BitmapAlphaPrepareOptions
    {
        bool chromaKey = false;
        COLORREF chromaColor = 0;
        BYTE chromaThreshold = 10;
    };

    [[nodiscard]] bool PrepareBitmapForAlphaBlend(
        HBITMAP& source,
        wil::unique_hbitmap& storage,
        const BitmapAlphaPrepareOptions& opts = {}) noexcept;

    // Black-key: чёрные пиксели (RGB=0,0,0) → alpha=0, остальные → alpha=255.
    // Для 32bpp DIBSECTION (результат Promise32BPP). Аналог BWM GdipCreateBitmapAlpha(BLACKNESS).
    void ApplyBlackKeyAlpha(HBITMAP hbmp) noexcept;

    // Фон иконки контрола: настроенный chroma-key (Root\Control\{Kind}) либо black-key по умолчанию.
    // Битмап должен быть 32bpp DIBSECTION (Promise32BPP).
    void ApplyIconBackgroundAlpha(HBITMAP hbmp32, const WndCompat::Menu::IconBackgroundColorRemoval& removal) noexcept;
}
