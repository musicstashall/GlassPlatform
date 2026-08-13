// ScrollBarDrawTheme — общие примитивы для темы «ScrollBar».
// Политику по part/state задаёт каждый хук DrawThemeBackground (ListView, TreeView, …),
// здесь только diff-маска для интерактива, утилиты прямоугольника и SIZEBOX+orig для корневого пути.

#include "CorePch.h"

#include <Hooks/ScrollBarDrawTheme.h>
#include <Hooks/ScrollBarTheme.h>

#include <DebugLog.h>

#include <uxtheme.h>
#include <wil/resource.h>

#include <algorithm>

#pragma comment(lib, "msimg32.lib")

namespace GlassCore::ScrollBarDrawTheme
{
    namespace detail
    {
        namespace ScrollBarPart
        {
            constexpr int kArrowBtn        = 1;
            constexpr int kThumbHorz     = 2;
            constexpr int kThumbVert     = 3;
            constexpr int kGripperHorz     = 8;
            constexpr int kGripperVert     = 9;
        }

        [[nodiscard]] bool IsVerticalScrollBarPart(int iPartId, int iStateId) noexcept
        {
            switch (iPartId)
            {
            case ScrollBarPart::kThumbVert:
            case ScrollBarPart::kGripperVert:
                return true;
            case ScrollBarPart::kThumbHorz:
            case ScrollBarPart::kGripperHorz:
                return false;
            case ScrollBarPart::kArrowBtn:
                return iStateId >= 1 && iStateId <= 8;
            default:
                return false;
            }
        }

        void ClearColumn(void* bits, int col, int width, int height, int pitch) noexcept
        {
            if (!bits || col < 0 || col >= width)
                return;
            for (int y = 0; y < height; ++y)
            {
                auto* row = reinterpret_cast<DWORD*>(
                    static_cast<BYTE*>(bits) + static_cast<size_t>(y) * pitch);
                row[col] = 0u;
            }
        }

        void ClearRow(void* bits, int row, int width, int height, int pitch) noexcept
        {
            if (!bits || row < 0 || row >= height)
                return;
            auto* p = reinterpret_cast<DWORD*>(
                static_cast<BYTE*>(bits) + static_cast<size_t>(row) * pitch);
            for (int x = 0; x < width; ++x)
                p[x] = 0u;
        }

        void ClearOuterBorder1px(void* bits, int width, int height, int pitch) noexcept
        {
            if (!bits || width <= 0 || height <= 0)
                return;
            ClearRow(bits, 0, width, height, pitch);
            ClearRow(bits, height - 1, width, height, pitch);
            ClearColumn(bits, 0, width, height, pitch);
            ClearColumn(bits, width - 1, width, height, pitch);
        }

        constexpr int kDiffAmplifier = 4;

        void ApplyDiffMask32(
            void*       renderedBits,
            const void* baselineBits,
            int         width,
            int         height,
            int         pitchRendered,
            int         pitchBaseline) noexcept
        {
            if (!renderedBits || !baselineBits || width <= 0 || height <= 0)
                return;

            for (int y = 0; y < height; ++y)
            {
                auto* pR = reinterpret_cast<DWORD*>(
                    static_cast<BYTE*>(renderedBits) + static_cast<size_t>(y) * pitchRendered);
                const auto* pBL = reinterpret_cast<const DWORD*>(
                    static_cast<const BYTE*>(baselineBits) + static_cast<size_t>(y) * pitchBaseline);

                for (int x = 0; x < width; ++x)
                {
                    const DWORD dr  = pR[x];
                    const DWORD dbl = pBL[x];

                    const int rR = static_cast<int>((dr >> 16) & 0xFF);
                    const int gR = static_cast<int>((dr >> 8) & 0xFF);
                    const int bR = static_cast<int>(dr & 0xFF);

                    const int rBL = static_cast<int>((dbl >> 16) & 0xFF);
                    const int gBL = static_cast<int>((dbl >> 8) & 0xFF);
                    const int bBL = static_cast<int>(dbl & 0xFF);

                    const int delta = std::max({
                        std::abs(rR - rBL),
                        std::abs(gR - gBL),
                        std::abs(bR - bBL)
                    });

                    const int alpha = std::min(delta * kDiffAmplifier, 255);

                    if (alpha == 0)
                    {
                        pR[x] = 0u;
                        continue;
                    }

                    const BYTE rOut = static_cast<BYTE>(std::clamp(rR * alpha / 255, 0, 255));
                    const BYTE gOut = static_cast<BYTE>(std::clamp(gR * alpha / 255, 0, 255));
                    const BYTE bOut = static_cast<BYTE>(std::clamp(bR * alpha / 255, 0, 255));

                    pR[x] = (static_cast<DWORD>(static_cast<BYTE>(alpha)) << 24)
                        | (static_cast<DWORD>(rOut) << 16)
                        | (static_cast<DWORD>(gOut) << 8)
                        | static_cast<DWORD>(bOut);
                }
            }
        }

        [[nodiscard]] bool TryDrawScrollBarInteractiveGlassImpl(
            HRESULT(WINAPI* orig)(HTHEME, HDC, int, int, LPCRECT, LPCRECT),
            HTHEME      hTheme,
            HDC         hdcDest,
            int         iPartId,
            int         iStateId,
            const RECT& drawRc) noexcept
        {
            const int w = drawRc.right - drawRc.left;
            const int h = drawRc.bottom - drawRc.top;
            if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || !hdcDest)
                return false;

            BITMAPINFO bi{};
            bi.bmiHeader.biSize          = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth         = w;
            bi.bmiHeader.biHeight        = -h;
            bi.bmiHeader.biPlanes        = 1;
            bi.bmiHeader.biBitCount      = 32;
            bi.bmiHeader.biCompression   = BI_RGB;

            void* bitsRendered = nullptr;
            void* bitsBaseline = nullptr;
            wil::unique_hbitmap dibRendered{
                CreateDIBSection(hdcDest, &bi, DIB_RGB_COLORS, &bitsRendered, nullptr, 0) };
            wil::unique_hbitmap dibBaseline{
                CreateDIBSection(hdcDest, &bi, DIB_RGB_COLORS, &bitsBaseline, nullptr, 0) };
            if (!dibRendered || !dibBaseline || !bitsRendered || !bitsBaseline)
                return false;

            wil::unique_hdc memDc{ CreateCompatibleDC(hdcDest) };
            if (!memDc)
                return false;

            const COLORREF bgColor = GetSysColor(COLOR_BTNFACE);
            const RECT     localRc{ 0, 0, w, h };

            wil::unique_hbrush fillBr{ CreateSolidBrush(bgColor) };
            if (!fillBr)
                return false;

            {
                const HGDIOBJ old = SelectObject(memDc.get(), dibBaseline.get());
                const auto  restore = wil::scope_exit([&]() noexcept { SelectObject(memDc.get(), old); });
                RECT        rc = localRc;
                FillRect(memDc.get(), &rc, fillBr.get());
            }

            {
                const HGDIOBJ old = SelectObject(memDc.get(), dibRendered.get());
                const auto  restore = wil::scope_exit([&]() noexcept { SelectObject(memDc.get(), old); });
                RECT        rc = localRc;
                FillRect(memDc.get(), &rc, fillBr.get());

                const GlassCore::ScrollBarTheme::DrawThemeOrigCallDepth depth;
                const HRESULT hrDtb = orig(hTheme, memDc.get(), iPartId, iStateId, &localRc, nullptr);
                if (FAILED(hrDtb))
                {
                    DebugLog(
                        DBG_WARNING,
                        L"[GlassCore][ScrollBarDrawTheme] offscreen tema: part=%d state=%d hr=0x%08X w=%d h=%d",
                        iPartId,
                        iStateId,
                        hrDtb,
                        w,
                        h);
                    return false;
                }
            }

            DIBSECTION dsR{}, dsBL{};
            if (!GetObject(dibRendered.get(), sizeof(dsR), &dsR) || dsR.dsBm.bmBitsPixel != 32)
                return false;
            if (!GetObject(dibBaseline.get(), sizeof(dsBL), &dsBL) || dsBL.dsBm.bmBitsPixel != 32)
                return false;

            ApplyDiffMask32(
                bitsRendered,
                bitsBaseline,
                w,
                h,
                dsR.dsBm.bmWidthBytes,
                dsBL.dsBm.bmWidthBytes);

            if (iPartId == ScrollBarPart::kArrowBtn)
                ClearOuterBorder1px(bitsRendered, w, h, dsR.dsBm.bmWidthBytes);
            else if (IsVerticalScrollBarPart(iPartId, iStateId))
                ClearColumn(bitsRendered, 0, w, h, dsR.dsBm.bmWidthBytes);
            else
                ClearRow(bitsRendered, 0, w, h, dsR.dsBm.bmWidthBytes);

            const HGDIOBJ oldBmp = SelectObject(memDc.get(), dibRendered.get());
            const auto restoreBmp = wil::scope_exit([&]() noexcept { SelectObject(memDc.get(), oldBmp); });

            SetLastError(0);
            const BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            if (!GdiAlphaBlend(
                    hdcDest,
                    drawRc.left,
                    drawRc.top,
                    w,
                    h,
                    memDc.get(),
                    0,
                    0,
                    w,
                    h,
                    bf))
            {
                DebugLog(
                    DBG_WARNING,
                    L"[GlassCore][ScrollBarDrawTheme] GdiAlphaBlend: part=%d gle=%lu w=%d h=%d hdcDest=%p",
                    iPartId,
                    GetLastError(),
                    w,
                    h,
                    hdcDest);
                return false;
            }
            return true;
        }
    } // namespace detail

    bool IsScrollBarTrackBackgroundPart(int iPartId) noexcept
    {
        return iPartId >= 4 && iPartId <= 7;
    }

    bool IsScrollBarInteractivePart(int iPartId) noexcept
    {
        return iPartId == 1 || iPartId == 2 || iPartId == 3 || iPartId == 8 || iPartId == 9;
    }

    bool TryGetScrollBarDrawRect(LPCRECT pRect, LPCRECT pClipRect, RECT& outRect) noexcept
    {
        if (!pRect || IsRectEmpty(pRect))
            return false;
        RECT r = *pRect;
        if (pClipRect)
        {
            RECT inter{};
            if (!IntersectRect(&inter, &r, pClipRect))
                return false;
            outRect = inter;
        }
        else
            outRect = r;
        return !IsRectEmpty(&outRect);
    }

    void LogScrollBarDrawThemeCall(LPCWSTR logTag, int iPartId, int iStateId) noexcept
    {
        DebugLog(
            L"%ls DrawThemeBackground: ScrollBar part=%d state=%d",
            logTag ? logTag : L"[GlassCore][ScrollBarDrawTheme]",
            iPartId,
            iStateId);
    }

    bool TryDrawScrollBarInteractiveGlass(
        HRESULT(WINAPI* orig)(HTHEME, HDC, int, int, LPCRECT, LPCRECT),
        HTHEME      hTheme,
        HDC         hdcDest,
        int         iPartId,
        int         iStateId,
        const RECT& drawRc) noexcept
    {
        return detail::TryDrawScrollBarInteractiveGlassImpl(orig, hTheme, hdcDest, iPartId, iStateId, drawRc);
    }

    HRESULT DrawScrollBarSizeBoxBlackThenOrig(
        HRESULT(WINAPI* orig)(HTHEME, HDC, int, int, LPCRECT, LPCRECT),
        HTHEME        hTheme,
        HDC           hdc,
        int           iPartId,
        int           iStateId,
        const RECT&   r) noexcept
    {
        PatBlt(hdc, r.left, r.top, r.right - r.left, r.bottom - r.top, BLACKNESS);
        const GlassCore::ScrollBarTheme::DrawThemeOrigCallDepth depth;
        RECT draw = r;
        return orig(hTheme, hdc, iPartId, iStateId, &draw, &draw);
    }
} // namespace GlassCore::ScrollBarDrawTheme
