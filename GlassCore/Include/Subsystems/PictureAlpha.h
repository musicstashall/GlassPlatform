#pragma once
#include <windows.h>
#include <cstddef>

namespace GlassCore::GdiUtils
{
    // An icon or a cursor an application draws itself into a window on glass
    // lands with A=0 wherever it carries no alpha of its own. These helpers
    // draw it with its transparency. None of them knows a window: the caller
    // checks the class, the session and the DC. (A picture copied from a bitmap
    // gets A=255 from OpaqueBitmapSource and is drawn as it is: nothing is cut
    // out of it by colour.)

    // An icon or a cursor larger than this is the application's own call.
    inline constexpr int kMaxIconSide = 1024;

    // The size DrawIconEx draws at: what the caller asked for, the system icon
    // size where it asked for the default, else the size of the icon itself.
    [[nodiscard]] inline bool IconDrawSize(HICON icon, int cx, int cy, UINT flags, int& width, int& height) noexcept
    {
        if (!cx || !cy)
        {
            if (flags & DI_DEFAULTSIZE)
            {
                if (!cx) cx = GetSystemMetrics(SM_CXICON);
                if (!cy) cy = GetSystemMetrics(SM_CYICON);
            }
            else
            {
                ICONINFO shape{};
                if (!GetIconInfo(icon, &shape)) return false;
                BITMAP bitmap{};
                const HBITMAP sized = shape.hbmColor ? shape.hbmColor : shape.hbmMask;
                const bool known = sized && GetObjectW(sized, sizeof(bitmap), &bitmap) == sizeof(bitmap);
                // A monochrome icon has no colour bitmap: its two masks lie one above the other.
                const bool colour = shape.hbmColor != nullptr;
                if (shape.hbmColor) DeleteObject(shape.hbmColor);
                if (shape.hbmMask) DeleteObject(shape.hbmMask);
                if (!known) return false;
                if (!cx) cx = bitmap.bmWidth;
                if (!cy) cy = colour ? bitmap.bmHeight : bitmap.bmHeight / 2;
            }
        }
        if (cx <= 0 || cy <= 0 || cx > kMaxIconSide || cy > kMaxIconSide) return false;
        width = cx; height = cy;
        return true;
    }

    // One pixel of an icon drawn over black and over white, as premultiplied
    // BGRA. What differs between the two is the background showing through, so
    // the difference is the pixel's transparency whatever the icon is made of -
    // alpha, a mask, a palette - and the picture over black is already
    // multiplied by its coverage. The most covered channel decides: no channel
    // is then above the alpha. A pixel brighter over black than over white
    // inverts what is under it (a cursor): it is drawn as it looks over black.
    [[nodiscard]] inline DWORD IconPixel(DWORD onBlack, DWORD onWhite) noexcept
    {
        unsigned alpha = 0;
        for (int shift = 0; shift < 24; shift += 8)
        {
            const unsigned dark = (onBlack >> shift) & 0xFFu, light = (onWhite >> shift) & 0xFFu;
            if (light < dark) return 0xFF000000u | (onBlack & 0x00FFFFFFu);
            const unsigned covered = 255u - (light - dark);
            if (covered > alpha) alpha = covered;
        }
        return (alpha << 24) | (onBlack & 0x00FFFFFFu);
    }

    // Draws the icon into `dc` with its transparency: `original` (DrawIconEx)
    // draws it twice into buffers of our own, and the picture they give is
    // blended. The DC is not touched before that blend. False: nothing was
    // drawn, and the call is still the caller's to make.
    template <typename Original>
    [[nodiscard]] bool DrawIconAlpha(const Original& original, HDC dc, int x, int y, HICON icon,
        int width, int height, UINT step, UINT flags) noexcept
    {
        const HDC memory = CreateCompatibleDC(dc);
        if (!memory) return false;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* blackBits = nullptr;
        void* whiteBits = nullptr;
        const HBITMAP black = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &blackBits, nullptr, 0);
        const HBITMAP white = black ? CreateDIBSection(memory, &info, DIB_RGB_COLORS, &whiteBits, nullptr, 0) : nullptr;
        bool drawn = false;
        if (blackBits && whiteBits) // a buffer that was not created has no bits either
        {
            const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
            auto* const onBlack = static_cast<DWORD*>(blackBits);
            auto* const onWhite = static_cast<DWORD*>(whiteBits);
            for (size_t i = 0; i < count; ++i) { onBlack[i] = 0; onWhite[i] = 0x00FFFFFFu; }
            const HGDIOBJ previous = SelectObject(memory, black);
            if (previous && previous != HGDI_ERROR)
            {
                bool twice = original(memory, 0, 0, icon, width, height, step, nullptr, flags) != FALSE;
                if (twice)
                {
                    const HGDIOBJ first = SelectObject(memory, white);
                    twice = first && first != HGDI_ERROR
                        && original(memory, 0, 0, icon, width, height, step, nullptr, flags) != FALSE;
                }
                if (twice && GdiFlush())
                {
                    for (size_t i = 0; i < count; ++i) onBlack[i] = IconPixel(onBlack[i], onWhite[i]);
                    const HGDIOBJ second = SelectObject(memory, black);
                    if (second && second != HGDI_ERROR)
                    {
                        const BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
                        drawn = GdiAlphaBlend(dc, x, y, width, height, memory, 0, 0, width, height, blend) != FALSE;
                    }
                }
            }
        }
        // The DC first: it lets go of whichever buffer it still holds.
        DeleteDC(memory);
        if (black) DeleteObject(black);
        if (white) DeleteObject(white);
        return drawn;
    }
}
