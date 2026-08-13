// GlassShared/src/BitmapUtils.cpp
//
// FIX (Bug 1): Promise32BPP переписан на чистый GDI (GetDIBits + CreateDIBSection).
//
// Проблема GDI+ подхода:
//   Gdiplus::Bitmap(HBITMAP, nullptr) трактует любой 32bpp DIB как PixelFormat32bppRGB,
//   то есть игнорирует 4-й байт (альфа). При последующем DrawImage/GetHBITMAP альфа-канал
//   обнуляется → иконки становятся полностью непрозрачными → поверх BLACKNESS
//   прозрачные пиксели рисуются чёрным (опак=false → альфа=0 → blitter видит чёрный фон).
//
// GDI-подход (GetDIBits):
//   Читает сырые байты пикселей напрямую из bitmap-памяти — 4-й байт (альфа) сохраняется.
//   Для источников с битглубиной < 32 принудительно ставим alpha=0xFF (полная непрозрачность).

#include "SharedPch.h"
#include <BitmapUtils.h>

namespace Shared::BitmapUtils
{
    bool IsOemBitmap(HBITMAP hbmp)
    {
        UINT_PTR ptr = reinterpret_cast<UINT_PTR>(hbmp);
        return (ptr >= 1 && ptr <= 5);
    }

    // -------------------------------------------------------------------------
    // Promise32BPP — конвертирует любой HBITMAP в 32bpp top-down DIBSECTION,
    // сохраняя альфа-канал для источников с битглубиной 32.
    // -------------------------------------------------------------------------
    HBITMAP Promise32BPP(HBITMAP hbmp)
    {
        if (!hbmp)
            return nullptr;

        // Получаем метаданные источника
        BITMAP bm{};
        if (!GetObject(hbmp, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0)
            return nullptr;

        HDC hdcScreen = GetDC(nullptr);
        if (!hdcScreen)
            return nullptr;

        // Описатель нового 32bpp top-down DIB
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = bm.bmWidth;
        bi.bmiHeader.biHeight = -bm.bmHeight; // отрицательное = top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        void* pvBits = nullptr;
        HBITMAP hbmpResult = CreateDIBSection(
            hdcScreen, &bi, DIB_RGB_COLORS, &pvBits, nullptr, 0);

        if (!hbmpResult || !pvBits)
        {
            ReleaseDC(nullptr, hdcScreen);
            return nullptr;
        }

        // Читаем пиксели источника напрямую в буфер.
        // Для 32bpp-источников GDI копирует все 4 байта (BGRA) без изменений —
        // альфа-канал сохраняется.
        if (!GetDIBits(hdcScreen, hbmp, 0, static_cast<UINT>(bm.bmHeight),
            pvBits, &bi, DIB_RGB_COLORS))
        {
            DeleteObject(hbmpResult);
            ReleaseDC(nullptr, hdcScreen);
            return nullptr;
        }

        ReleaseDC(nullptr, hdcScreen);

        // Для источников без альфа-канала (< 32bpp) GetDIBits заполнит
        // 4-й байт нулём (alpha=0 = полностью прозрачный). Принудительно
        // делаем все пиксели непрозрачными.
        if (bm.bmBitsPixel < 32)
        {
            DWORD* pixels = static_cast<DWORD*>(pvBits);
            const int count = bm.bmWidth * bm.bmHeight;
            for (int i = 0; i < count; ++i)
                pixels[i] |= 0xFF000000u; // alpha = 255
        }

        return hbmpResult;
    }

    // -------------------------------------------------------------------------
    // HasAlphaChannel — проверяет, содержит ли 32bpp HBITMAP ненулевой альфа.
    //
    // Чистый GDI-аналог BWM WinHandler::IsBitmapSupportAlpha, без багов:
    //   - BWM использует static BITMAPINFO (потоконебезопасно)
    //   - BWM не различает DIBSECTION и DDB
    //
    // Быстрый путь: для DIBSECTION проверяем bmBits напрямую (без GetDIBits).
    // Медленный путь: для DDB читаем пиксели через GetDIBits.
    // -------------------------------------------------------------------------
    bool HasAlphaChannel(HBITMAP hbmp) noexcept
    {
        if (!hbmp)
            return false;

        BITMAP bm{};
        if (!GetObject(hbmp, sizeof(bm), &bm) || bm.bmBitsPixel != 32)
            return false;

        const int pixelCount = bm.bmWidth * bm.bmHeight;
        if (pixelCount <= 0)
            return false;

        // Быстрый путь: DIBSECTION — пиксели доступны напрямую
        DIBSECTION ds{};
        if (GetObject(hbmp, sizeof(ds), &ds) && ds.dsBm.bmBits)
        {
            const BYTE* pixels = static_cast<const BYTE*>(ds.dsBm.bmBits);
            for (int i = 0; i < pixelCount; ++i)
            {
                if (pixels[i * 4 + 3] != 0)
                    return true;
            }
            return false;
        }

        // Медленный путь: DDB — читаем через GetDIBits
        HDC hdcScreen = GetDC(nullptr);
        if (!hdcScreen)
            return false;

        BITMAPINFO bi{};
        bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth       = bm.bmWidth;
        bi.bmiHeader.biHeight      = -bm.bmHeight; // top-down
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        const int bufSize = pixelCount * 4;
        auto pixelBuf = std::make_unique<BYTE[]>(bufSize);

        bool hasAlpha = false;
        if (GetDIBits(hdcScreen, hbmp, 0, static_cast<UINT>(bm.bmHeight),
                      pixelBuf.get(), &bi, DIB_RGB_COLORS))
        {
            for (int i = 0; i < pixelCount; ++i)
            {
                if (pixelBuf[i * 4 + 3] != 0)
                {
                    hasAlpha = true;
                    break;
                }
            }
        }

        ReleaseDC(nullptr, hdcScreen);
        return hasAlpha;
    }

    // -------------------------------------------------------------------------
    // SpriteEffect — убирает пиксели заданного цвета (± порог),
    // обнуляя их альфа-канал.
    // -------------------------------------------------------------------------
    SpriteEffect::SpriteEffect(COLORREF colorToReplace, BYTE threshold)
        : m_colorToReplace(colorToReplace), m_threshold(threshold)
    {
    }

    void SpriteEffect::Apply(HBITMAP hbmp) const
    {
        if (!hbmp)
            return;

        BITMAP bm{};
        if (!GetObject(hbmp, sizeof(bm), &bm) || bm.bmBitsPixel != 32)
            return;

        // Для DIBSECTION bmBits указывает прямо на пиксели
        DIBSECTION ds{};
        if (!GetObject(hbmp, sizeof(ds), &ds) || !ds.dsBm.bmBits)
            return;

        BYTE* pixels = static_cast<BYTE*>(ds.dsBm.bmBits);
        const int count = bm.bmWidth * bm.bmHeight;

        const BYTE rRef = GetRValue(m_colorToReplace);
        const BYTE gRef = GetGValue(m_colorToReplace);
        const BYTE bRef = GetBValue(m_colorToReplace);

        for (int i = 0; i < count; ++i)
        {
            BYTE* p = pixels + i * 4; // BGRA
            const BYTE b = p[0], g = p[1], r = p[2];
            if (std::abs(static_cast<int>(r) - rRef) <= m_threshold &&
                std::abs(static_cast<int>(g) - gRef) <= m_threshold &&
                std::abs(static_cast<int>(b) - bRef) <= m_threshold)
            {
                p[3] = 0; // alpha = transparent
            }
        }
    }

    // -------------------------------------------------------------------------
    // ApplyEffects — конвертирует в 32bpp и применяет набор эффектов.
    // Работает на месте: модифицирует pvBits результата Promise32BPP.
    // -------------------------------------------------------------------------
    HBITMAP ApplyEffects(HBITMAP hbmp, const std::vector<std::shared_ptr<SpriteEffect>>& effects)
    {
        if (!hbmp || effects.empty())
            return nullptr;

        // Promise32BPP уже даёт нам DIBSECTION с pvBits
        HBITMAP hbmp32 = Promise32BPP(hbmp);
        if (!hbmp32)
            return nullptr;

        for (const auto& effect : effects)
            effect->Apply(hbmp32);

        return hbmp32;
    }

    void EarlyShutdown()
    {
        // GDI-ресурсы не требуют явного shutdown
    }
}