#include "SharedPch.h"
#include <Helpers/ThemeHelper.h>
#include <Helpers/RegHelper.h>
#include <WinApiUtils.h>
#include <Helpers/ColorUtils.h>
#include <SystemHelper.h>
#include <DebugLog.h>

#include <dwmapi.h>
#include <vsstyle.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "dwmapi.lib")

namespace
{
    // TABITEMSTATES: TIS_NORMAL..TIS_FOCUSED = 5 колонок в типичном горизонтальном атласе msstyles.
    constexpr int kTabItemAtlasStateCount = 5;

    [[nodiscard]] bool SampleCenterPixelToColorref(HBITMAP hbm, COLORREF& outRgb) noexcept
    {
        BITMAP bm{};
        if (GetObjectW(hbm, sizeof(bm), &bm) != sizeof(bm) || bm.bmWidth <= 0 || bm.bmHeight == 0)
            return false;
        const int x = bm.bmWidth / 2;
        const int y = bm.bmHeight / 2;
        wil::unique_hdc hdc{ CreateCompatibleDC(nullptr) };
        if (!hdc)
            return false;
        HGDIOBJ old = SelectObject(hdc.get(), hbm);
        if (!old)
            return false;
        auto unsel = wil::scope_exit([&]() noexcept { (void)SelectObject(hdc.get(), old); });
        const COLORREF px = GetPixel(hdc.get(), x, y);
        if (px == CLR_INVALID)
            return false;
        outRgb = px & 0xFFFFFFu;
        return true;
    }

    [[nodiscard]] bool MedianChromaKeyFromBgraRegion(
        const void* bits,
        int         w,
        int         h,
        int         rowBytes,
        const RECT& area,
        COLORREF&   outRgb) noexcept
    {
        outRgb  = 0;
        if (!bits || w <= 0 || h <= 0 || rowBytes < w * 4)
            return false;
        if (area.left >= area.right || area.top >= area.bottom)
            return false;
        const int              rowQ = rowBytes / 4;
        std::array<BYTE, 32>  rs{};
        std::array<BYTE, 32>  gs{};
        std::array<BYTE, 32>  bs{};
        int                    n   = 0;
        for (int y = area.top; y < area.bottom; ++y)
        {
            for (int x = area.left; x < area.right; ++x)
            {
                if (x < 0 || y < 0 || x >= w || y >= h)
                    continue;
                if (n >= static_cast<int>(rs.size()))
                    break;
                const auto* base
                    = static_cast<const RGBQUAD*>(bits) + static_cast<size_t>(y) * static_cast<size_t>(rowQ);
                RGBQUAD     p   = base[static_cast<size_t>(x)];
                Shared::WinApiUtils::FixBgraForThemeChromaDibInPlace(p);
                if (p.rgbReserved == 0)
                    continue;
                const size_t i = static_cast<size_t>(n);
                rs[i]  = p.rgbRed;
                gs[i]  = p.rgbGreen;
                bs[i]  = p.rgbBlue;
                ++n;
            }
        }
        if (n <= 0)
            return false;
        const int nn  = n;
        std::sort(rs.begin(), rs.begin() + nn);
        std::sort(gs.begin(), gs.begin() + nn);
        std::sort(bs.begin(), bs.begin() + nn);
        const int mid = (nn - 1) / 2;
        outRgb
            = RGB(rs[static_cast<size_t>(mid)], gs[static_cast<size_t>(mid)], bs[static_cast<size_t>(mid)]);
        return true;
    }

    [[nodiscard]] bool RectCenterOddWindow(const RECT& bounds, int side, RECT& outArea) noexcept
    {
        const int w = bounds.right - bounds.left;
        const int h = bounds.bottom - bounds.top;
        if (w <= 0 || h <= 0)
            return false;
        const int cx  = (bounds.left + bounds.right) / 2;
        const int cy  = (bounds.top + bounds.bottom) / 2;
        const int half = side / 2;
        outArea.left   = cx - half;
        outArea.top    = cy - half;
        outArea.right  = outArea.left + side;
        outArea.bottom = outArea.top + side;
        if (w < side || h < side)
        {
            outArea         = bounds;
            return outArea.left < outArea.right && outArea.top < outArea.bottom;
        }
        if (outArea.left < bounds.left)
        {
            outArea.left  = bounds.left;
            outArea.right = bounds.left + side;
        }
        if (outArea.top < bounds.top)
        {
            outArea.top    = bounds.top;
            outArea.bottom = bounds.top + side;
        }
        if (outArea.right > bounds.right)
        {
            outArea.right  = bounds.right;
            outArea.left   = bounds.right - side;
        }
        if (outArea.bottom > bounds.bottom)
        {
            outArea.bottom = bounds.bottom;
            outArea.top    = bounds.bottom - side;
        }
        return outArea.left < outArea.right && outArea.top < outArea.bottom;
    }

    [[nodiscard]] bool HbitmapToTopDownBgra32(
        HBITMAP hbm, std::vector<std::uint8_t>& outBuf, int& w, int& h, int& rowBytes) noexcept
    {
        outBuf.clear();
        w        = 0;
        h        = 0;
        rowBytes = 0;
        BITMAP bm{};
        if (GetObjectW(hbm, sizeof(bm), &bm) != sizeof(bm) || bm.bmWidth <= 0)
            return false;
        const int absH = (bm.bmHeight < 0) ? -bm.bmHeight : bm.bmHeight;
        if (absH == 0)
            return false;
        w  = bm.bmWidth;
        h  = absH;
        rowBytes = ((w * 32 + 31) / 32) * 4;
        outBuf.assign(static_cast<size_t>(rowBytes) * static_cast<size_t>(h), 0u);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = w;
        bi.bmiHeader.biHeight      = -h;
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        wil::unique_hdc hdc{ CreateCompatibleDC(nullptr) };
        if (!hdc)
            return false;
        HGDIOBJ old = SelectObject(hdc.get(), hbm);
        if (!old)
            return false;
        const int lines
            = GetDIBits(hdc.get(), hbm, 0, h, outBuf.data(), &bi, DIB_RGB_COLORS);
        (void)SelectObject(hdc.get(), old);
        return lines == h;
    }

    [[nodiscard]] bool SampleDibAtlasCellMedian(
        const std::uint8_t* bits,
        int                   bmW,
        int                   bmH,
        int                   rowBytes,
        int                   nStates,
        int                   iStateId1Based,
        int                   usedTmt,
        COLORREF&             outRgb) noexcept
    {
        outRgb = 0;
        if (!bits || bmW <= 0 || bmH <= 0 || nStates <= 0)
            return false;
        const int col  = iStateId1Based - 1;
        if (col < 0 || col >= nStates)
            return false;
        const int cellW = bmW / nStates;
        if (cellW <= 0)
            return false;
        const int  x0  = col * cellW;
        const RECT cell{ x0, 0, x0 + cellW, bmH };
        RECT       sampleArea{};
        const int  side  = (cellW < 6 || bmH < 6) ? 3 : 5;
        if (!RectCenterOddWindow(cell, side, sampleArea))
            return false;
        if (!MedianChromaKeyFromBgraRegion(
                bits, bmW, bmH, rowBytes, sampleArea, outRgb))
            return false;
        DebugLog(DBG_NORMAL,
            L"[GlassShared][ThemeImageSample] class=\"Tab\" path=DibAtlas TMT=%d part=%d state=%d "
            L"bm=%dx%d nStates=%d col=%d medWin=%d COLORREF=0x%06X R=%u G=%u B=%u",
            usedTmt,
            TABP_TABITEM,
            iStateId1Based,
            bmW,
            bmH,
            nStates,
            col,
            side,
            (unsigned)(outRgb & 0xFFFFFFu),
            (unsigned)GetRValue(outRgb),
            (unsigned)GetGValue(outRgb),
            (unsigned)GetBValue(outRgb));
        return true;
    }

    [[nodiscard]] bool TryGetTabChromaKeyViaDibAtlasSlice(HWND themeAnchorWnd, int tisState, COLORREF& outRgb) noexcept
    {
        outRgb = 0;
        if (themeAnchorWnd && !IsWindow(themeAnchorWnd))
            return false;

        HTHEME hTheme = OpenThemeData(themeAnchorWnd, L"Tab");
        if (!hTheme)
        {
            DebugLog(DBG_WARNING, L"[GlassShared][ThemeImageSample] path=DibAtlas: OpenThemeData(Tab) failed");
            return false;
        }
        auto closeTh = wil::scope_exit([hTheme]() noexcept { CloseThemeData(hTheme); });

        static const int s_tmtOrder[] = { TMT_DIBDATA, TMT_GLYPHDIBDATA, TMT_HBITMAP };
        for (int propId : s_tmtOrder)
        {
            HBITMAP   hbm   = nullptr;
            const HRESULT hrGtb
                = GetThemeBitmap(hTheme, TABP_TABITEM, tisState, propId, GBF_DIRECT, &hbm);
            if (!SUCCEEDED(hrGtb) || !hbm)
            {
                if (hbm)
                {
                    DeleteObject(hbm);
                }
                continue;
            }
            int                      wb  = 0;
            int                      hb  = 0;
            int                      rb  = 0;
            std::vector<std::uint8_t> buf;
            if (HbitmapToTopDownBgra32(hbm, buf, wb, hb, rb)
                && SampleDibAtlasCellMedian(
                    buf.data(), wb, hb, rb, kTabItemAtlasStateCount, tisState, propId, outRgb))
            {
                DeleteObject(hbm);
                return true;
            }
            DeleteObject(hbm);
        }

        DebugLog(DBG_WARNING, L"[GlassShared][ThemeImageSample] path=DibAtlas: GetThemeBitmap/slice failed");
        return false;
    }

    [[nodiscard]] bool TryGetTabChromaKeyViaDrawThemeBackground(HWND themeAnchorWnd, int tisState, COLORREF& outRgb) noexcept
    {
        outRgb = 0;
        if (themeAnchorWnd && !IsWindow(themeAnchorWnd))
            return false;

        HTHEME hTheme = OpenThemeData(themeAnchorWnd, L"Tab");
        if (!hTheme)
        {
            DebugLog(DBG_WARNING, L"[GlassShared][ThemeImageSample] path=DrawTheme: OpenThemeData(Tab) failed");
            return false;
        }
        auto closeTh = wil::scope_exit([hTheme]() noexcept { CloseThemeData(hTheme); });

        SIZE sz{};
        if (FAILED(GetThemePartSize(hTheme, nullptr, TABP_TABITEM, tisState, nullptr, TS_DRAW, &sz))
            || sz.cx <= 0
            || sz.cy <= 0)
        {
            sz.cx = 120;
            sz.cy = 30;
        }
        if (sz.cx > 512)
            sz.cx = 512;
        if (sz.cy > 512)
            sz.cy = 512;
        if (sz.cx < 4)
            sz.cx = 4;
        if (sz.cy < 4)
            sz.cy = 4;

        HDC hdcScreen = GetDC(nullptr);
        if (!hdcScreen)
            return false;
        auto relDc = wil::scope_exit([hdcScreen]() noexcept { ReleaseDC(nullptr, hdcScreen); });

        BITMAPINFO bi{};
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = sz.cx;
        bi.bmiHeader.biHeight      = -sz.cy;
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        void*   bits  = nullptr;
        HBITMAP hdib  = CreateDIBSection(hdcScreen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!hdib || !bits)
        {
            if (hdib)
                DeleteObject(hdib);
            return false;
        }
        auto delDib = wil::scope_exit([hdib]() noexcept { (void)DeleteObject(hdib); });

        HDC hdcMem = CreateCompatibleDC(hdcScreen);
        if (!hdcMem)
            return false;
        auto delMem = wil::scope_exit([hdcMem]() noexcept { DeleteDC(hdcMem); });

        HGDIOBJ oldB = SelectObject(hdcMem, hdib);
        if (!oldB)
            return false;
        auto unselB = wil::scope_exit([hdcMem, oldB]() noexcept { (void)SelectObject(hdcMem, oldB); });

        int    rowBytes = sz.cx * 4;
        BITMAP bmGet{};
        if (GetObjectW(hdib, sizeof(bmGet), &bmGet) == sizeof(bmGet) && bmGet.bmWidthBytes > 0)
            rowBytes = bmGet.bmWidthBytes;
        const size_t nBytes = static_cast<size_t>(rowBytes) * static_cast<size_t>(sz.cy);
        std::memset(bits, 0, nBytes);

        RECT rc{ 0, 0, sz.cx, sz.cy };
        const HRESULT dhr
            = DrawThemeBackground(hTheme, hdcMem, TABP_TABITEM, tisState, &rc, nullptr);
        if (FAILED(dhr))
        {
            DebugLog(DBG_NORMAL,
                L"[GlassShared][ThemeImageSample] path=DrawTheme: DrawThemeBackground hr=0x%08X",
                (unsigned)dhr);
            return false;
        }

        RECT content = rc;
        (void)GetThemeBackgroundContentRect(hTheme, hdcMem, TABP_TABITEM, tisState, &rc, &content);
        if (!IntersectRect(&content, &content, &rc) || IsRectEmpty(&content))
        {
            content = rc;
        }
        const int cW  = content.right - content.left;
        const int cH  = content.bottom - content.top;
        if (cW < 1 || cH < 1)
            return false;

        bool  ok  = false;
        RECT  sampleArea{};
        const int side  = (cW < 6 || cH < 6) ? 3 : 5;
        if (RectCenterOddWindow(content, side, sampleArea))
        {
            ok = MedianChromaKeyFromBgraRegion(
                bits, sz.cx, sz.cy, rowBytes, sampleArea, outRgb);
        }
        if (!ok)
        {
            const int    pxX  = sz.cx / 2;
            const int    pxY  = sz.cy / 2;
            const int    rowQ = rowBytes / 4;
            const auto*  base
                = static_cast<const RGBQUAD*>(bits) + static_cast<size_t>(pxY) * static_cast<size_t>(rowQ);
            RGBQUAD      p    = base[static_cast<size_t>(pxX)];
            Shared::WinApiUtils::FixBgraForThemeChromaDibInPlace(p);
            if (p.rgbReserved == 0)
                return false;
            outRgb = RGB(p.rgbRed, p.rgbGreen, p.rgbBlue);
        }
        DebugLog(DBG_NORMAL,
            L"[GlassShared][ThemeImageSample] class=\"Tab\" path=DrawTheme part=%d state=%d %dx%d "
            L"content=(%d,%d)-(%d,%d) COLORREF=0x%06X R=%u G=%u B=%u",
            TABP_TABITEM,
            tisState,
            (int)sz.cx,
            (int)sz.cy,
            (int)content.left,
            (int)content.top,
            (int)content.right,
            (int)content.bottom,
            (unsigned)(outRgb & 0xFFFFFFu),
            (unsigned)GetRValue(outRgb),
            (unsigned)GetGValue(outRgb),
            (unsigned)GetBValue(outRgb));
        return true;
    }
} // namespace

namespace Shared::ThemeHelper
{
    HRESULT GetThemeClass(HTHEME hTheme, LPWSTR className, int cch)
    {
        using PfnGetThemeClass = HRESULT(WINAPI*)(HTHEME, LPWSTR, int);
        static const auto s_actualGetThemeClass = reinterpret_cast<PfnGetThemeClass>(
            GetProcAddress(GetModuleHandleW(L"uxtheme.dll"), MAKEINTRESOURCEA(74))
            );

        if (!s_actualGetThemeClass)
            return E_FAIL;

        return s_actualGetThemeClass(hTheme, className, cch);
    }

    HRESULT GetThemeColorFromClass(HTHEME hTheme, int iPartId, int iStateId, int iPropId, COLORREF& color)
    {
        if (!hTheme)
            return E_INVALIDARG;
        return GetThemeColor(hTheme, iPartId, iStateId, iPropId, &color);
    }

    bool IsThemeColor(COLORREF color, const wchar_t* classname) noexcept
    {
        HTHEME theme = OpenThemeData(nullptr, classname);
        if (!theme)
            return false;
        COLORREF themeColor{};
        HRESULT hr = GetThemeColor(theme, 0, 0, TMT_FILLCOLOR, &themeColor);
        CloseThemeData(theme);
        return SUCCEEDED(hr) && (themeColor == color);
    }

    bool ShouldAppsUseDarkMode() noexcept
    {
        using Pfn = bool(WINAPI*)();
        static const auto pfn = reinterpret_cast<Pfn>(
            GetProcAddress(GetModuleHandleW(L"uxtheme.dll"), MAKEINTRESOURCEA(132)));
        return pfn ? pfn() : false;
    }

    bool ShouldSystemUseDarkMode() noexcept
    {
        using Pfn = bool(WINAPI*)();
        static const auto pfn = reinterpret_cast<Pfn>(
            GetProcAddress(GetModuleHandleW(L"uxtheme.dll"), MAKEINTRESOURCEA(138)));
        return pfn ? pfn() : false;
    }

    bool IsDarkModeAllowedForApp() noexcept
    {
        using Pfn = bool(WINAPI*)();
        static const auto pfn = reinterpret_cast<Pfn>(
            GetProcAddress(GetModuleHandleW(L"uxtheme.dll"), MAKEINTRESOURCEA(139)));
        return pfn ? pfn() : false;
    }

    HRESULT SetSystemVisualStyle(
        LPCWSTR pszFilename, LPCWSTR pszColor, LPCWSTR pszSize, int flags) noexcept
    {
        using Pfn = HRESULT(WINAPI*)(LPCWSTR, LPCWSTR, LPCWSTR, int);
        static const auto pfn = reinterpret_cast<Pfn>(
            GetProcAddress(GetModuleHandleW(L"uxtheme.dll"), MAKEINTRESOURCEA(65)));
        if (!pfn)
            return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        return pfn(pszFilename, pszColor, pszSize, flags);
    }

    bool IsHighContrastActive() noexcept
    {
        HIGHCONTRASTW hc{ sizeof(hc) };
        if (!SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0))
            return false;
        return (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
    }

    bool IsSystemDarkMode() noexcept
    {
        return ShouldAppsUseDarkMode() && IsDarkModeAllowedForApp();
    }

    namespace
    {
        enum class PreferredAppMode : int
        {
            Default    = 0,
            AllowDark  = 1,
            ForceDark  = 2,
            ForceLight = 3,
        };

        using FnAllowDarkModeForWindow   = BOOL(WINAPI*)(HWND, BOOL);
        using FnAllowDarkModeForAppLegacy = BOOL(WINAPI*)(BOOL);
        using FnSetPreferredAppMode      = PreferredAppMode(WINAPI*)(PreferredAppMode);
        using FnRefreshImmersivePolicy   = void(WINAPI*)();
        using FnFlushMenuThemes          = void(WINAPI*)();

        [[nodiscard]] HMODULE GetUxThemeModule() noexcept
        {
            HMODULE hMod = GetModuleHandleW(L"uxtheme.dll");
            if (!hMod)
                hMod = LoadLibraryW(L"uxtheme.dll");
            return hMod;
        }

        [[nodiscard]] bool ReadAppsUseLightThemeIsDark() noexcept
        {
            DWORD value = 1;
            DWORD size  = sizeof(value);
            static constexpr wchar_t kPersonalizeKey[] =
                L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
            if (RegGetValueW(
                    HKEY_CURRENT_USER,
                    kPersonalizeKey,
                    L"AppsUseLightTheme",
                    RRF_RT_REG_DWORD,
                    nullptr,
                    &value,
                    &size) != ERROR_SUCCESS)
            {
                return false;
            }
            return value == 0;
        }

        [[nodiscard]] bool ResolveProcessDarkIntent() noexcept
        {
            if (IsSystemDarkMode())
                return true;
            return ReadAppsUseLightThemeIsDark();
        }

        void ApplyPreferredAppMode(HMODULE hUx, bool dark) noexcept
        {
            if (!hUx)
                return;

            const DWORD build = Shared::SystemHelper::g_buildNumber;
            if (build >= 18362)
            {
                const auto pfn = reinterpret_cast<FnSetPreferredAppMode>(
                    GetProcAddress(hUx, MAKEINTRESOURCEA(135)));
                if (pfn)
                {
                    const PreferredAppMode mode = dark ? PreferredAppMode::AllowDark
                                                       : PreferredAppMode::ForceLight;
                    (void)pfn(mode);
                }
                return;
            }

            const auto pfnLegacy = reinterpret_cast<FnAllowDarkModeForAppLegacy>(
                GetProcAddress(hUx, MAKEINTRESOURCEA(135)));
            if (pfnLegacy)
                (void)pfnLegacy(dark ? TRUE : FALSE);
        }

        void RefreshUxThemeAfterAppMode(HMODULE hUx) noexcept
        {
            if (!hUx)
                return;

            if (const auto pfn = reinterpret_cast<FnRefreshImmersivePolicy>(
                    GetProcAddress(hUx, MAKEINTRESOURCEA(104))))
            {
                pfn();
            }

            if (const auto pfn = reinterpret_cast<FnFlushMenuThemes>(
                    GetProcAddress(hUx, MAKEINTRESOURCEA(136))))
            {
                pfn();
            }
        }
    } // namespace

    bool IsTrayXamlDarkMode() noexcept
    {
        // #132 — системная политика apps. Не требуем #139 (process allow).
        if (ShouldAppsUseDarkMode())
            return true;
        return ReadAppsUseLightThemeIsDark();
    }

    bool ResolveEffectiveDarkMode() noexcept
    {
        if (GetSysColor(COLOR_WINDOW) < 0xFFFFFF)
            return true;
        if (IsSystemDarkMode())
            return true;
        return ReadAppsUseLightThemeIsDark();
    }

    bool IsWin32DarkModeKickEnabled() noexcept
    {
        const DWORD disabled = Shared::RegHelper::Get<DWORD>({}, L"Win32DarkModeKickDisable", 0);
        return disabled == 0;
    }

    void TryEnableProcessDarkMode() noexcept
    {
        if (!IsWin32DarkModeKickEnabled())
            return;

        static std::atomic<bool> s_applied{ false };
        if (s_applied.exchange(true, std::memory_order_acq_rel))
            return;

        const bool dark = ResolveProcessDarkIntent();
        HMODULE const hUx = GetUxThemeModule();
        if (!hUx)
            return;

        ApplyPreferredAppMode(hUx, dark);
        RefreshUxThemeAfterAppMode(hUx);

        DebugLog(DBG_IMPORTANT,
            L"[GlassShared][ThemeHelper] TryEnableProcessDarkMode: dark=%d build=%u COLOR_WINDOW=0x%06X",
            dark ? 1 : 0,
            Shared::SystemHelper::g_buildNumber,
            GetSysColor(COLOR_WINDOW) & 0xFFFFFF);
    }

    void EnableWindowDarkModeUxTheme(HWND hwnd, bool dark) noexcept
    {
        if (!hwnd || !IsWindow(hwnd) || !IsWin32DarkModeKickEnabled())
            return;

        HMODULE const hUx = GetUxThemeModule();
        if (!hUx)
            return;

        const auto pfn = reinterpret_cast<FnAllowDarkModeForWindow>(
            GetProcAddress(hUx, MAKEINTRESOURCEA(133)));
        if (!pfn)
            return;

        (void)pfn(hwnd, dark ? TRUE : FALSE);
    }

    bool TrySampleThemeImagePixelColor(
        HWND themeAnchorWnd,
        const ThemeImageSampleRequest& request,
        COLORREF& outRgb) noexcept
    {
        outRgb = 0;
        if (themeAnchorWnd && !IsWindow(themeAnchorWnd))
            return false;
        if (!request.pszlThemeClass || !*request.pszlThemeClass
            || !request.pBitmapPropIds || request.bitmapPropIdCount <= 0)
            return false;
        if (request.strategy != ThemePixelSampleStrategy::Center)
            return false;

        HTHEME hTheme = OpenThemeData(themeAnchorWnd, request.pszlThemeClass);
        if (!hTheme)
        {
            DebugLog(DBG_WARNING,
                L"[GlassShared][ThemeImageSample] OpenThemeData не удался: class=\"%s\" part=%d state=%d",
                request.pszlThemeClass,
                request.iPartId,
                request.iStateId);
            return false;
        }
        auto closeTh = wil::scope_exit([hTheme]() noexcept { CloseThemeData(hTheme); });

        HBITMAP   hbm      = nullptr;
        HRESULT  lastHr   = E_FAIL;
        int      usedTmt  = 0;
        for (int i = 0; i < request.bitmapPropIdCount; ++i)
        {
            const int propId = request.pBitmapPropIds[i];
            hbm = nullptr;
            lastHr = GetThemeBitmap(
                hTheme,
                request.iPartId,
                request.iStateId,
                propId,
                request.getThemeBitmapFlags,
                &hbm);
            if (SUCCEEDED(lastHr) && hbm)
            {
                usedTmt = propId;
                break;
            }
            if (hbm)
            {
                DeleteObject(hbm);
                hbm = nullptr;
            }
        }

        bool fromBitmap = false;
        if (hbm)
        {
            fromBitmap = SampleCenterPixelToColorref(hbm, outRgb);
            DeleteObject(hbm);
            hbm = nullptr;
            if (fromBitmap)
            {
                DebugLog(DBG_NORMAL,
                    L"[GlassShared][ThemeImageSample] class=\"%s\" part=%d state=%d TMT=%d (битмап) COLORREF=0x%06X R=%u G=%u B=%u",
                    request.pszlThemeClass,
                    request.iPartId,
                    request.iStateId,
                    usedTmt,
                    (unsigned)(outRgb & 0xFFFFFFu),
                    (unsigned)GetRValue(outRgb),
                    (unsigned)GetGValue(outRgb),
                    (unsigned)GetBValue(outRgb));
                return true;
            }
            DebugLog(DBG_NORMAL,
                L"[GlassShared][ThemeImageSample] class=\"%s\" part=%d state=%d TMT=%d — центр пикселя не прочитан, hr_lastbitmap=0x%08X",
                request.pszlThemeClass,
                request.iPartId,
                request.iStateId,
                usedTmt,
                (unsigned)lastHr);
        }
        else
        {
            DebugLog(DBG_NORMAL,
                L"[GlassShared][ThemeImageSample] class=\"%s\" part=%d state=%d — GetThemeBitmap без битмапа, hr=0x%08X",
                request.pszlThemeClass,
                request.iPartId,
                request.iStateId,
                (unsigned)lastHr);
        }

        if (request.useColorFallback)
        {
            COLORREF c{};
            const HRESULT hr = GetThemeColor(
                hTheme, request.iPartId, request.iStateId, request.colorFallbackPropId, &c);
            if (SUCCEEDED(hr))
            {
                outRgb = c & 0xFFFFFFu;
                DebugLog(DBG_NORMAL,
                    L"[GlassShared][ThemeImageSample] class=\"%s\" part=%d state=%d TMT_fallback=%d COLORREF=0x%06X R=%u G=%u B=%u",
                    request.pszlThemeClass,
                    request.iPartId,
                    request.iStateId,
                    request.colorFallbackPropId,
                    (unsigned)(outRgb & 0xFFFFFFu),
                    (unsigned)GetRValue(outRgb),
                    (unsigned)GetGValue(outRgb),
                    (unsigned)GetBValue(outRgb));
                return true;
            }
            DebugLog(DBG_WARNING,
                L"[GlassShared][ThemeImageSample] class=\"%s\" part=%d state=%d — GetThemeColor fallback hr=0x%08X",
                request.pszlThemeClass,
                request.iPartId,
                request.iStateId,
                (unsigned)hr);
        }

        DebugLog(DBG_WARNING,
            L"[GlassShared][ThemeImageSample] class=\"%s\" part=%d state=%d — сэмпл не удался",
            request.pszlThemeClass,
            request.iPartId,
            request.iStateId);
        return false;
    }

} // namespace Shared::ThemeHelper

namespace
{
    [[nodiscard]] bool DoTryGetTabChromaKeyFromThemeForState(HWND themeAnchorWnd, int tis1Based, COLORREF& outRgb) noexcept
    {
        if (TryGetTabChromaKeyViaDrawThemeBackground(themeAnchorWnd, tis1Based, outRgb))
            return true;
        if (TryGetTabChromaKeyViaDibAtlasSlice(themeAnchorWnd, tis1Based, outRgb))
            return true;
        return false;
    }
} // namespace

namespace Shared::ThemeHelper
{
    bool TryGetTabChromaKeyFromThemeForState(HWND themeAnchorWnd, int tis1Based, COLORREF& outRgb) noexcept
    {
        outRgb = 0;
        if (!DoTryGetTabChromaKeyFromThemeForState(themeAnchorWnd, tis1Based, outRgb))
        {
            DebugLog(DBG_WARNING,
                L"[GlassShared][ThemeImageSample] Tab chroma: state=%d — DrawTheme и DibAtlas оба не удались",
                tis1Based);
            return false;
        }
        return true;
    }

    bool TryGetTabInactiveChromaKeyFromTheme(HWND themeAnchorWnd, COLORREF& outRgb) noexcept
    {
        return TryGetTabChromaKeyFromThemeForState(themeAnchorWnd, TIS_NORMAL, outRgb);
    }

    DWORD GetThemeColorizationColor(std::wstring_view type) noexcept
    {
        using PFN_GetImmersiveColorFromColorSetEx = DWORD(WINAPI*)(DWORD, DWORD, bool, DWORD);
        using PFN_GetImmersiveColorTypeFromName = DWORD(WINAPI*)(LPCWSTR);
        using PFN_GetImmersiveUserColorSetPreference = DWORD(WINAPI*)(bool, bool);

        static const auto getColorFromSetEx = reinterpret_cast<PFN_GetImmersiveColorFromColorSetEx>(
            GetProcAddress(GetModuleHandleW(L"uxtheme.dll"), MAKEINTRESOURCEA(95)));
        static const auto getColorTypeFromName = reinterpret_cast<PFN_GetImmersiveColorTypeFromName>(
            GetProcAddress(GetModuleHandleW(L"uxtheme.dll"), MAKEINTRESOURCEA(96)));
        static const auto getUserColorSetPreference = reinterpret_cast<PFN_GetImmersiveUserColorSetPreference>(
            GetProcAddress(GetModuleHandleW(L"uxtheme.dll"), MAKEINTRESOURCEA(98)));

        DWORD argb = 0;
        if (getColorFromSetEx && getColorTypeFromName && getUserColorSetPreference)
        {
            const DWORD abgr = getColorFromSetEx(
                getUserColorSetPreference(FALSE, FALSE),
                getColorTypeFromName(type.data()),
                TRUE,
                0);
            argb = (abgr & 0xFF000000u)
                | ((abgr & 0xFFu) << 16)
                | (abgr & 0xFF00u)
                | ((abgr >> 16) & 0xFFu);
        }
        else
        {
            BOOL opaque = FALSE;
            (void)DwmGetColorizationColor(&argb, &opaque);
        }
        return argb;
    }
}
