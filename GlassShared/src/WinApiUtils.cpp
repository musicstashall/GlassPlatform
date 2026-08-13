#include "SharedPch.h"
#include <WinApiUtils.h>

#include <algorithm>
#include <cstring>

#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "msimg32.lib")

namespace Shared::WinApiUtils
{
    void FixBgraForThemeChromaDibInPlace(RGBQUAD& p) noexcept
    {
        if (p.rgbReserved == 0)
        {
            const unsigned rgb = static_cast<unsigned>(p.rgbRed) | static_cast<unsigned>(p.rgbGreen)
                | static_cast<unsigned>(p.rgbBlue);
            if (rgb != 0)
                p.rgbReserved = 255;
        }
    }

    // =========================================================================
    // Вспомогательные функции для CreateSolidColorBrushWithAlpha
    // =========================================================================

    // Pre-multiply цветовой канал на альфа
    static UCHAR PremultiplyColor(UCHAR color, UCHAR alpha = 255)
    {
        return static_cast<UCHAR>((color * (alpha + 1)) >> 8);
    }

    // Создает 32bpp DIB (Device Independent Bitmap)
    static HBITMAP CreateDIB(
        LONG width = 1,
        LONG height = -1,
        UCHAR** bits = nullptr)
    {
        BITMAPINFO bitmapInfo{};
        bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmapInfo.bmiHeader.biWidth = width;
        bitmapInfo.bmiHeader.biHeight = height;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;

        return CreateDIBSection(
            nullptr,
            &bitmapInfo,
            DIB_RGB_COLORS,
            reinterpret_cast<PVOID*>(bits),
            nullptr,
            0
        );
    }

    // =========================================================================
    // CreateSolidColorBrushWithAlpha
    // =========================================================================

    HBRUSH CreateSolidColorBrushWithAlpha(COLORREF color, UCHAR alpha)
    {
        // Создаем 1x1 32bpp DIB
        UCHAR* bits = nullptr;
        HBITMAP bitmap = CreateDIB(1, -1, &bits);

        if (!bitmap || !bits)
            return nullptr;

        // Заполняем единственный пиксель с pre-multiplied alpha
        // Формат BGRA (Windows DIB)
        bits[0] = PremultiplyColor(GetBValue(color), alpha); // Blue
        bits[1] = PremultiplyColor(GetGValue(color), alpha); // Green
        bits[2] = PremultiplyColor(GetRValue(color), alpha); // Red
        bits[3] = alpha;                                      // Alpha

        // Создаем pattern brush из этого bitmap
        HBRUSH brush = CreatePatternBrush(bitmap);

        // Удаляем bitmap (brush сделал внутреннюю копию)
        DeleteObject(bitmap);

        return brush;
    }

    // =========================================================================
    // IsThemed
    // =========================================================================

    bool IsThemed(HWND hWnd, std::wstring_view themeName)
    {
        if (themeName.empty())
            return false;

        HTHEME theme = OpenThemeData(hWnd, themeName.data());

        if (theme)
        {
            CloseThemeData(theme);
            return true;
        }

        return false;
    }

    std::wstring GetModuleFileBaseName(HMODULE hModule) noexcept
    {
        std::wstring path;
        path.resize(MAX_PATH);
        const DWORD n = GetModuleFileNameW(hModule, path.data(), static_cast<DWORD>(path.size()));
        if (!n || n >= path.size())
            return {};
        path.resize(n);
        const size_t slash = path.find_last_of(L'\\');
        if (slash != std::wstring::npos && slash + 1 < path.size())
            path.erase(0, slash + 1);
        return path;
    }

    HRESULT EnableBlurBehindWindow(HWND hWnd, bool bEnable, bool bTransition, HRGN hRgn)
    {
        DWM_BLURBEHIND pBlurBehind{};
        int iFlags = 0;

        if (hRgn)
        {
            iFlags += 2;
            pBlurBehind.hRgnBlur = hRgn;
        }
        pBlurBehind.dwFlags = iFlags | 0x05;
        pBlurBehind.fEnable = bEnable;
        pBlurBehind.fTransitionOnMaximized = bTransition;
        HRESULT hr = DwmEnableBlurBehindWindow(hWnd, &pBlurBehind);
        if (pBlurBehind.hRgnBlur)
            DeleteObject(pBlurBehind.hRgnBlur);
        return hr;
    }

    HRESULT ExtendFrameIntoEntireClientArea(HWND hWnd, bool bEnable) noexcept
    {
        MARGINS margins{};
        if (bEnable)
        {
            margins.cxLeftWidth    = -1;
            margins.cxRightWidth   = -1;
            margins.cyTopHeight    = -1;
            margins.cyBottomHeight = -1;
        }
        return DwmExtendFrameIntoClientArea(hWnd, &margins);
    }

    bool TryGetClassBackgroundSolidRgb(HWND hWnd, COLORREF& outRgb) noexcept
    {
        if (!hWnd)
            return false;

        const ULONG_PTR cb = GetClassLongPtrW(hWnd, GCLP_HBRBACKGROUND);
        if (cb == 0)
            return false;

        constexpr ULONG_PTR kMaxSysColorPlusOne = static_cast<ULONG_PTR>(COLOR_MENUBAR) + 1;
        if (cb >= 1 && cb <= kMaxSysColorPlusOne)
        {
            const int idx = static_cast<int>(cb) - 1;
            outRgb        = GetSysColor(idx) & 0xFFFFFFu;
            return true;
        }

        const HGDIOBJ hgo = reinterpret_cast<HGDIOBJ>(cb);
        if (GetObjectType(hgo) != OBJ_BRUSH)
            return false;

        LOGBRUSH lb{};
        if (GetObject(hgo, static_cast<int>(sizeof(lb)), &lb) != sizeof(lb))
            return false;
        if (lb.lbStyle != BS_SOLID)
            return false;

        outRgb = lb.lbColor & 0xFFFFFFu;
        return true;
    }

    // =========================================================================
    // DrawThemeBackground(Ex) + GdiAlphaBlend
    // =========================================================================

    bool DrawThemeBackgroundWithConstantAlpha(
        DrawThemeBackground_pfn orig,
        HTHEME hTheme,
        HDC hdcDest,
        int partId,
        int stateId,
        LPCRECT pRect,
        LPCRECT pClipRect,
        BYTE sourceConstantAlpha,
        size_t maxPixelCount) noexcept
    {
        if (!orig || !pRect || !hdcDest)
            return false;
        const int w = pRect->right - pRect->left;
        const int h = pRect->bottom - pRect->top;
        if (w <= 0 || h <= 0)
            return false;

        if (sourceConstantAlpha == 0)
        {
            RECT fill{};
            if (pClipRect)
            {
                if (!IntersectRect(&fill, pRect, pClipRect))
                    return true;
            }
            else
                fill = *pRect;
            PatBlt(hdcDest,
                fill.left,
                fill.top,
                fill.right - fill.left,
                fill.bottom - fill.top,
                BLACKNESS);
            return true;
        }

        if (sourceConstantAlpha == 255)
            return SUCCEEDED(orig(hTheme, hdcDest, partId, stateId, pRect, pClipRect));

        if (static_cast<size_t>(w) * static_cast<size_t>(h) > maxPixelCount)
            return false;

        BITMAPINFO bi{};
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = w;
        bi.bmiHeader.biHeight      = -h;
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        wil::unique_hbitmap dib{ CreateDIBSection(hdcDest, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) };
        if (!dib || !bits)
            return false;

        wil::unique_hdc hdcWork{ CreateCompatibleDC(hdcDest) };
        if (!hdcWork)
            return false;

        HGDIOBJ oldBmp = SelectObject(hdcWork.get(), dib.get());
        if (!oldBmp)
            return false;
        auto unsel = wil::scope_exit([&]() noexcept { (void)SelectObject(hdcWork.get(), oldBmp); });

        std::memset(bits, 0, static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);

        RECT rcMem{ 0, 0, w, h };
        RECT rcClipMem{};
        LPCRECT pClipMem = nullptr;
        if (pClipRect)
        {
            RECT inter{};
            if (IntersectRect(&inter, pRect, pClipRect))
            {
                rcClipMem.left   = inter.left - pRect->left;
                rcClipMem.top    = inter.top - pRect->top;
                rcClipMem.right  = inter.right - pRect->left;
                rcClipMem.bottom = inter.bottom - pRect->top;
                pClipMem         = &rcClipMem;
            }
        }

        if (FAILED(orig(hTheme, hdcWork.get(), partId, stateId, &rcMem, pClipMem)))
            return false;

        BLENDFUNCTION bf{};
        bf.BlendOp             = AC_SRC_OVER;
        bf.BlendFlags          = 0;
        bf.SourceConstantAlpha = sourceConstantAlpha;
        bf.AlphaFormat         = 0;

        return GdiAlphaBlend(
                   hdcDest,
                   pRect->left,
                   pRect->top,
                   w,
                   h,
                   hdcWork.get(),
                   0,
                   0,
                   w,
                   h,
                   bf)
            == TRUE;
    }

    bool DrawThemeBackgroundWithConstantAlphaAndChromaKey(
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
        size_t maxPixelCount,
        std::size_t* optChromaKeyedPixelCount) noexcept
    {
        if (!orig || !pRect || !hdcDest)
            return false;
        const int w = pRect->right - pRect->left;
        const int h = pRect->bottom - pRect->top;
        if (w <= 0 || h <= 0)
            return false;

        if (sourceConstantAlpha == 0)
        {
            RECT fill{};
            if (pClipRect)
            {
                if (!IntersectRect(&fill, pRect, pClipRect))
                    return true;
            }
            else
                fill = *pRect;
            PatBlt(hdcDest,
                fill.left,
                fill.top,
                fill.right - fill.left,
                fill.bottom - fill.top,
                BLACKNESS);
            return true;
        }

        if (static_cast<size_t>(w) * static_cast<size_t>(h) > maxPixelCount)
            return false;

        BITMAPINFO bi{};
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = w;
        bi.bmiHeader.biHeight      = -h;
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        wil::unique_hbitmap dib{ CreateDIBSection(hdcDest, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) };
        if (!dib || !bits)
            return false;

        wil::unique_hdc hdcWork{ CreateCompatibleDC(hdcDest) };
        if (!hdcWork)
            return false;

        HGDIOBJ oldBmp = SelectObject(hdcWork.get(), dib.get());
        if (!oldBmp)
            return false;
        auto unsel = wil::scope_exit([&]() noexcept { (void)SelectObject(hdcWork.get(), oldBmp); });

        BITMAP bm{};
        int rowBytes = w * 4;
        if (GetObjectW(dib.get(), sizeof(bm), &bm) == sizeof(bm) && bm.bmWidthBytes > 0)
            rowBytes = bm.bmWidthBytes;
        std::memset(bits, 0, static_cast<size_t>(rowBytes) * static_cast<size_t>(h));

        RECT rcMem{ 0, 0, w, h };
        RECT rcClipMem{};
        LPCRECT pClipMem = nullptr;
        if (pClipRect)
        {
            RECT inter{};
            if (IntersectRect(&inter, pRect, pClipRect))
            {
                rcClipMem.left   = inter.left - pRect->left;
                rcClipMem.top    = inter.top - pRect->top;
                rcClipMem.right  = inter.right - pRect->left;
                rcClipMem.bottom = inter.bottom - pRect->top;
                pClipMem         = &rcClipMem;
            }
        }

        if (FAILED(orig(hTheme, hdcWork.get(), partId, stateId, &rcMem, pClipMem)))
            return false;

        if (optChromaKeyedPixelCount)
            *optChromaKeyedPixelCount = 0u;

        // Локальная копия логики FixDrawEdgeDibAlphaChannel + хромакей + премультипликация для AC_SRC_ALPHA
        {
            auto*         quads  = static_cast<RGBQUAD*>(bits);
            const int     rowQ   = rowBytes / 4;
            const BYTE    rK     = GetRValue(chromaKey);
            const BYTE    gK     = GetGValue(chromaKey);
            const BYTE    bK     = GetBValue(chromaKey);
            const int     th     = static_cast<int>(chromaThreshold);

            for (int y = 0; y < h; ++y)
            {
                RGBQUAD* row = quads + static_cast<size_t>(y) * static_cast<size_t>(rowQ);
                for (int x = 0; x < w; ++x)
                {
                    RGBQUAD* p = row + x;
                    FixBgraForThemeChromaDibInPlace(*p);
                    if (p->rgbReserved == 0)
                        continue;
                    if (std::abs(static_cast<int>(p->rgbRed) - static_cast<int>(rK)) <= th
                        && std::abs(static_cast<int>(p->rgbGreen) - static_cast<int>(gK)) <= th
                        && std::abs(static_cast<int>(p->rgbBlue) - static_cast<int>(bK)) <= th)
                    {
                        p->rgbRed = p->rgbGreen = p->rgbBlue = p->rgbReserved = 0;
                        if (optChromaKeyedPixelCount)
                            ++*optChromaKeyedPixelCount;
                        continue;
                    }
                    const UINT a = p->rgbReserved;
                    p->rgbBlue  = static_cast<BYTE>((static_cast<UINT>(p->rgbBlue) * a) / 255u);
                    p->rgbGreen = static_cast<BYTE>((static_cast<UINT>(p->rgbGreen) * a) / 255u);
                    p->rgbRed   = static_cast<BYTE>((static_cast<UINT>(p->rgbRed) * a) / 255u);
                }
            }
        }

        BLENDFUNCTION bf{};
        bf.BlendOp             = AC_SRC_OVER;
        bf.BlendFlags          = 0;
        bf.SourceConstantAlpha = sourceConstantAlpha;
        bf.AlphaFormat         = AC_SRC_ALPHA;

        return GdiAlphaBlend(
                   hdcDest,
                   pRect->left,
                   pRect->top,
                   w,
                   h,
                   hdcWork.get(),
                   0,
                   0,
                   w,
                   h,
                   bf)
            == TRUE;
    }

    bool DrawThemeBackgroundExWithConstantAlpha(
        DrawThemeBackgroundEx_pfn orig,
        HTHEME hTheme,
        HDC hdcDest,
        int partId,
        int stateId,
        LPCRECT pRect,
        const DTBGOPTS* pOptions,
        BYTE sourceConstantAlpha,
        size_t maxPixelCount) noexcept
    {
        if (!orig || !pRect || !hdcDest)
            return false;
        const int w = pRect->right - pRect->left;
        const int h = pRect->bottom - pRect->top;
        if (w <= 0 || h <= 0)
            return false;

        if (sourceConstantAlpha == 0)
        {
            RECT fill = *pRect;
            if (pOptions && (pOptions->dwFlags & DTBG_CLIPRECT))
            {
                RECT inter{};
                if (!IntersectRect(&inter, pRect, &pOptions->rcClip))
                    return true;
                fill = inter;
            }
            PatBlt(hdcDest,
                fill.left,
                fill.top,
                fill.right - fill.left,
                fill.bottom - fill.top,
                BLACKNESS);
            return true;
        }

        if (sourceConstantAlpha == 255)
            return SUCCEEDED(orig(hTheme, hdcDest, partId, stateId, pRect, pOptions));

        if (static_cast<size_t>(w) * static_cast<size_t>(h) > maxPixelCount)
            return false;

        BITMAPINFO bi{};
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = w;
        bi.bmiHeader.biHeight      = -h;
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        wil::unique_hbitmap dib{ CreateDIBSection(hdcDest, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) };
        if (!dib || !bits)
            return false;

        wil::unique_hdc hdcWork{ CreateCompatibleDC(hdcDest) };
        if (!hdcWork)
            return false;

        HGDIOBJ oldBmp = SelectObject(hdcWork.get(), dib.get());
        if (!oldBmp)
            return false;
        auto unsel = wil::scope_exit([&]() noexcept { (void)SelectObject(hdcWork.get(), oldBmp); });

        std::memset(bits, 0, static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);

        RECT rcMem{ 0, 0, w, h };
        if (FAILED(orig(hTheme, hdcWork.get(), partId, stateId, &rcMem, pOptions)))
            return false;

        BLENDFUNCTION bf{};
        bf.BlendOp             = AC_SRC_OVER;
        bf.BlendFlags          = 0;
        bf.SourceConstantAlpha = sourceConstantAlpha;
        bf.AlphaFormat         = 0;

        return GdiAlphaBlend(
                   hdcDest,
                   pRect->left,
                   pRect->top,
                   w,
                   h,
                   hdcWork.get(),
                   0,
                   0,
                   w,
                   h,
                   bf)
            == TRUE;
    }

    namespace
    {
        void FixDrawEdgeDibAlphaChannel(RGBQUAD* bits, int w, int h, int rowBytes) noexcept
        {
            if (!bits || w <= 0 || h <= 0 || rowBytes < w * 4)
                return;
            const int rowQuads = rowBytes / 4;
            for (int y = 0; y < h; ++y)
            {
                RGBQUAD* row = bits + static_cast<size_t>(y) * static_cast<size_t>(rowQuads);
                for (int x = 0; x < w; ++x)
                {
                    RGBQUAD* p = row + x;
                    if (p->rgbReserved != 0)
                        continue;
                    const unsigned rgb = static_cast<unsigned>(p->rgbRed) | static_cast<unsigned>(p->rgbGreen)
                        | static_cast<unsigned>(p->rgbBlue);
                    if (rgb != 0)
                        p->rgbReserved = 255;
                }
            }
        }
    } // namespace

    bool DrawEdgeWithConstantAlpha(
        DrawEdge_pfn orig,
        HDC hdcDest,
        LPRECT pRect,
        UINT edge,
        UINT grfFlags,
        BYTE sourceConstantAlpha,
        int shrinkLeftWhenNoAdjust,
        size_t maxPixelCount) noexcept
    {
        if (!orig || !pRect || !hdcDest)
            return false;

        const RECT rcIn = *pRect;
        const int w = rcIn.right - rcIn.left;
        const int h = rcIn.bottom - rcIn.top;
        if (w <= 0 || h <= 0)
            return false;

        if (sourceConstantAlpha == 255)
            return orig(hdcDest, pRect, edge, grfFlags) == TRUE;

        if (sourceConstantAlpha == 0)
        {
            PatBlt(hdcDest, rcIn.left, rcIn.top, w, h, BLACKNESS);
            const LONG innerLeft = (std::min)(rcIn.left + shrinkLeftWhenNoAdjust, rcIn.right);
            pRect->left   = innerLeft;
            pRect->top    = rcIn.top;
            pRect->right  = rcIn.right;
            pRect->bottom = rcIn.bottom;
            return true;
        }

        if (static_cast<size_t>(w) * static_cast<size_t>(h) > maxPixelCount)
            return false;

        BITMAPINFO bi{};
        bi.bmiHeader.biSize          = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth         = w;
        bi.bmiHeader.biHeight        = -h;
        bi.bmiHeader.biPlanes        = 1;
        bi.bmiHeader.biBitCount      = 32;
        bi.bmiHeader.biCompression   = BI_RGB;

        void* bits = nullptr;
        wil::unique_hbitmap dib{ CreateDIBSection(hdcDest, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) };
        if (!dib || !bits)
            return false;

        wil::unique_hdc hdcWork{ CreateCompatibleDC(hdcDest) };
        if (!hdcWork)
            return false;

        HGDIOBJ oldBmp = SelectObject(hdcWork.get(), dib.get());
        if (!oldBmp)
            return false;
        auto unsel = wil::scope_exit([&]() noexcept { (void)SelectObject(hdcWork.get(), oldBmp); });

        BITMAP bm{};
        int rowBytes = w * 4;
        if (GetObjectW(dib.get(), sizeof(bm), &bm) == sizeof(bm) && bm.bmWidthBytes > 0)
            rowBytes = bm.bmWidthBytes;
        std::memset(bits, 0, static_cast<size_t>(rowBytes) * static_cast<size_t>(h));

        RECT rcLocal{ 0, 0, w, h };
        if (!orig(hdcWork.get(), &rcLocal, edge, grfFlags))
            return false;

        FixDrawEdgeDibAlphaChannel(static_cast<RGBQUAD*>(bits), w, h, rowBytes);

        BLENDFUNCTION bf{};
        bf.BlendOp             = AC_SRC_OVER;
        bf.BlendFlags          = 0;
        bf.SourceConstantAlpha = sourceConstantAlpha;
        bf.AlphaFormat         = AC_SRC_ALPHA;

        if (!GdiAlphaBlend(
                hdcDest,
                rcIn.left,
                rcIn.top,
                w,
                h,
                hdcWork.get(),
                0,
                0,
                w,
                h,
                bf))
            return false;

        if ((grfFlags & BF_ADJUST) != 0)
        {
            pRect->left   = rcIn.left + rcLocal.left;
            pRect->top    = rcIn.top + rcLocal.top;
            pRect->right  = rcIn.left + rcLocal.right;
            pRect->bottom = rcIn.top + rcLocal.bottom;
        }
        else
        {
            const LONG innerLeft = (std::min)(rcIn.left + shrinkLeftWhenNoAdjust, rcIn.right);
            pRect->left   = innerLeft;
            pRect->top    = rcIn.top;
            pRect->right  = rcIn.right;
            pRect->bottom = rcIn.bottom;
        }

        return true;
    }

} // namespace Shared::WinApiUtils