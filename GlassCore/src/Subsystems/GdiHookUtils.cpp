#include "CorePch.h"
#include <Subsystems/GdiHookUtils.h>
#include <BitmapUtils.h>
#include <ColorUtils.h>
#include <cstdlib>

namespace GlassCore::GdiUtils
{
    static BYTE PremulChannel(BYTE c, BYTE alpha) noexcept
    {
        return static_cast<BYTE>((static_cast<unsigned>(c) * (alpha + 1U)) >> 8);
    }

    LayeredStrokePen LayeredStrokePen::TryCreateSolid(const LOGPEN& lpIn, BYTE alpha) noexcept
    {
        LayeredStrokePen out{};
        if (alpha == 0)
            return out;

        LOGPEN lp = lpIn;
        if (lp.lopnStyle == PS_INSIDEFRAME)
            lp.lopnStyle = PS_SOLID;
        const BYTE st = static_cast<BYTE>(lp.lopnStyle & 0xFU);
        if (st != PS_SOLID)
            return out;

        int w = lp.lopnWidth.x <= 0 ? 1 : lp.lopnWidth.x;
        if (w > 24)
            return out;

        const COLORREF rgb = lp.lopnColor & 0xFFFFFF;

        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = 1;
        bi.bmiHeader.biHeight = -1;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        UCHAR* bits = nullptr;
        out.dib.reset(CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&bits), nullptr, 0));
        if (!out.dib || !bits)
        {
            out.dib.reset();
            return out;
        }

        bits[0] = PremulChannel(GetBValue(rgb), alpha);
        bits[1] = PremulChannel(GetGValue(rgb), alpha);
        bits[2] = PremulChannel(GetRValue(rgb), alpha);
        bits[3] = alpha;

        LOGBRUSH lb{};
        lb.lbStyle = BS_PATTERN;
        lb.lbColor = 0;
        lb.lbHatch = reinterpret_cast<ULONG_PTR>(out.dib.get());

        constexpr DWORD geo = PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND;
        out.pen.reset(ExtCreatePen(geo, static_cast<DWORD>(w), &lb, 0, nullptr));
        if (!out.pen)
            out.dib.reset();
        return out;
    }

    LayeredStrokePen LayeredStrokePen::TryCreateStyled(const LOGPEN& lpIn, BYTE alpha) noexcept
    {
        LayeredStrokePen out{};
        if (alpha == 0)
            return out;

        LOGPEN lp = lpIn;
        if (lp.lopnStyle == PS_INSIDEFRAME)
            lp.lopnStyle = PS_SOLID;
        const BYTE st = static_cast<BYTE>(lp.lopnStyle & 0xFU);
        DWORD styleFlag = 0;
        switch (st)
        {
        case PS_SOLID:      styleFlag = PS_SOLID;      break;
        case PS_DASH:       styleFlag = PS_DASH;       break;
        case PS_DOT:        styleFlag = PS_DOT;        break;
        case PS_DASHDOT:    styleFlag = PS_DASHDOT;    break;
        case PS_DASHDOTDOT: styleFlag = PS_DASHDOTDOT; break;
        default:            return out;
        }

        int w = lp.lopnWidth.x <= 0 ? 1 : lp.lopnWidth.x;
        if (w > 24)
            return out;

        const COLORREF rgb = lp.lopnColor & 0xFFFFFF;

        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = 1;
        bi.bmiHeader.biHeight = -1;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        UCHAR* bits = nullptr;
        out.dib.reset(CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&bits), nullptr, 0));
        if (!out.dib || !bits)
        {
            out.dib.reset();
            return out;
        }

        bits[0] = PremulChannel(GetBValue(rgb), alpha);
        bits[1] = PremulChannel(GetGValue(rgb), alpha);
        bits[2] = PremulChannel(GetRValue(rgb), alpha);
        bits[3] = alpha;

        LOGBRUSH lb{};
        lb.lbStyle = BS_PATTERN;
        lb.lbColor = 0;
        lb.lbHatch = reinterpret_cast<ULONG_PTR>(out.dib.get());

        constexpr DWORD geoBase = PS_GEOMETRIC | PS_ENDCAP_ROUND | PS_JOIN_ROUND;
        out.pen.reset(ExtCreatePen(geoBase | styleFlag, static_cast<DWORD>(w), &lb, 0, nullptr));
        if (!out.pen)
            out.dib.reset();
        return out;
    }

    wil::unique_hpen PenFromLogPen(const LOGPEN& lpIn) noexcept
    {
        LOGPEN lp = lpIn;
        if (lp.lopnStyle == PS_INSIDEFRAME)
            lp.lopnStyle = PS_SOLID;
        const BYTE st = static_cast<BYTE>(lp.lopnStyle & 0xFU);
        if (st == PS_NULL)
            return {};

        if (lp.lopnWidth.x <= 0)
            lp.lopnWidth.x = 1;

        const int       wRaw = lp.lopnWidth.x;
        const LOGBRUSH lb{ BS_SOLID, lp.lopnColor, 0 };

        if (wRaw <= 1)
            return wil::unique_hpen(CreatePen(st, 1, lp.lopnColor));

        const DWORD w = static_cast<DWORD>(wRaw);
        constexpr DWORD geoBase = PS_GEOMETRIC | PS_JOIN_ROUND | PS_ENDCAP_ROUND;

        static const DWORD patDash[] = { 15, 5 };
        static const DWORD patDot[] = { 3, 3 };
        static const DWORD patDashDot[] = { 7, 5, 3, 5 };
        static const DWORD patDashDotDot[] = { 7, 3, 2, 3, 2, 3 };

        HPEN h = nullptr;
        switch (st)
        {
        case PS_SOLID:
            h = ExtCreatePen(geoBase | PS_SOLID, w, &lb, 0, nullptr);
            break;
        case PS_DASH:
            h = ExtCreatePen(geoBase | PS_USERSTYLE, w, &lb, 2, patDash);
            break;
        case PS_DOT:
            h = ExtCreatePen(geoBase | PS_USERSTYLE, w, &lb, 2, patDot);
            break;
        case PS_DASHDOT:
            h = ExtCreatePen(geoBase | PS_USERSTYLE, w, &lb, 4, patDashDot);
            break;
        case PS_DASHDOTDOT:
            h = ExtCreatePen(geoBase | PS_USERSTYLE, w, &lb, 6, patDashDotDot);
            break;
        default:
            h = ExtCreatePen(geoBase | PS_SOLID, w, &lb, 0, nullptr);
            break;
        }
        if (!h)
            h = CreatePen(PS_SOLID, 1, lp.lopnColor);
        return wil::unique_hpen(h);
    }

    bool PrepareBitmapForAlphaBlend(
        HBITMAP& source,
        wil::unique_hbitmap& storage,
        const BitmapAlphaPrepareOptions& opts) noexcept
    {
        if (!source || Shared::BitmapUtils::IsOemBitmap(source))
            return false;

        storage.reset(Shared::BitmapUtils::Promise32BPP(source));
        if (!storage)
            return false;

        if (opts.chromaKey)
        {
            Shared::BitmapUtils::SpriteEffect eff(opts.chromaColor, opts.chromaThreshold);
            eff.Apply(storage.get());
        }

        source = storage.get();
        return true;
    }

    void ApplyBlackKeyAlpha(HBITMAP hbmp) noexcept
    {
        DIBSECTION ds{};
        if (!GetObject(hbmp, sizeof(ds), &ds) || !ds.dsBm.bmBits)
            return;

        auto* pixels = static_cast<DWORD*>(ds.dsBm.bmBits);
        const int count = ds.dsBm.bmWidth * std::abs(ds.dsBm.bmHeight);

        for (int i = 0; i < count; ++i)
        {
            const DWORD rgb = pixels[i] & 0x00FFFFFF;
            // Чёрный = фон ImageList → полностью прозрачный,
            // цветной = рисунок иконки → полностью непрозрачный.
            pixels[i] = rgb ? (rgb | 0xFF000000u) : 0;
        }
    }

    void ApplyIconBackgroundAlpha(HBITMAP hbmp32, const WndCompat::Menu::IconBackgroundColorRemoval& removal) noexcept
    {
        if (!removal.enable)
        {
            ApplyBlackKeyAlpha(hbmp32);
            return;
        }

        Shared::BitmapUtils::SpriteEffect eff(
            Shared::ColorUtils::ARGBToCOLORREF(removal.colorTreatAsTransparent),
            static_cast<BYTE>(removal.colorTreatAsTransparentThreshold));
        eff.Apply(hbmp32);
    }
} // namespace GlassCore::GdiUtils
