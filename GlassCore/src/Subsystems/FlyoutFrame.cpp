#include "CorePch.h"
#include <Subsystems/FlyoutFrame.h>
#include <Subsystems/Settings.h>
#include <Subsystems/EffectHelper.h>
#include <Helpers/SystemHelper.h>
#include <MenuUtils.h>
#include <dwmapi.h>

#include <mutex>
#include <unordered_set>

namespace GlassCore::FlyoutFrame
{
    namespace
    {
        std::mutex g_hostBorderDeferredMutex;
        std::unordered_set<HWND> g_hostBorderDeferredHwnds;
        [[nodiscard]] WndCompat::Menu::BorderContext ToMenuBorder(
            const Settings::BorderContext& border) noexcept
        {
            WndCompat::Menu::BorderContext out{};
            out.colorUseNone    = border.colorUseNone;
            out.colorUseDefault = border.colorUseDefault;
            out.color           = border.color;
            out.cornerType      = border.cornerType;
            return out;
        }

        void RefreshDwmNc(HWND hwnd, unsigned effectType, DWM_WINDOW_CORNER_PREFERENCE cornerType) noexcept
        {
            // На Windows 10 (<22000) нет ни DWMSBT_* (effectType 6..8), ни DWMWA_WINDOW_CORNER_PREFERENCE.
            // При cornerType == DWMWCP_DONOTROUND (1 != DWMWCP_DEFAULT) вызов TriggerWindowNCRendering
            // (SetWindowPos SWP_DRAWFRAME) после uncloak синхронно шлёт WM_NCCALCSIZE/WM_ERASEBKGND,
            // стирая клиентский контент #32768/ComboLBox в 0x00000000 без перерисовки.
            if (Shared::SystemHelper::g_buildNumber < 22000)
                return;

            if (effectType >= 6 && effectType <= 8)
            {
                (void)DefWindowProcW(hwnd, WM_NCACTIVATE, TRUE, 0);
                EffectHelper::TriggerWindowNCRendering(hwnd);
            }
            else if (cornerType != DWMWCP_DEFAULT)
            {
                EffectHelper::TriggerWindowNCRendering(hwnd);
            }
        }

        void PaintNcRingGdi(HWND hwnd, bool darkMode, const Settings::BorderContext& border) noexcept
        {
            HDC hdc = GetWindowDC(hwnd);
            if (!hdc)
                return;

            auto releaseDC = wil::scope_exit([&] { ReleaseDC(hwnd, hdc); });

            constexpr MARGINS mr{ 1, 1, 1, 1 };
            RECT paintRect{};
            if (!GetWindowRect(hwnd, &paintRect))
                return;
            OffsetRect(&paintRect, -paintRect.left, -paintRect.top);

            {
                const int savedDC = SaveDC(hdc);
                ExcludeClipRect(hdc,
                    paintRect.left + mr.cxLeftWidth,
                    paintRect.top + mr.cyTopHeight,
                    paintRect.right - mr.cxRightWidth,
                    paintRect.bottom - mr.cyBottomHeight);
                PatBlt(hdc, paintRect.left, paintRect.top,
                    paintRect.right - paintRect.left,
                    paintRect.bottom - paintRect.top,
                    BLACKNESS);
                RestoreDC(hdc, savedDC);
            }

            if (border.cornerType == DWMWCP_DONOTROUND)
                return;

            (void)Shared::MenuUtils::PaintFlyoutNcOutlineGdi(
                hdc, paintRect, ToMenuBorder(border), darkMode);
        }

        void ApplyHostBorderFlush(HWND hwnd, FlyoutHostBorderFlush flush) noexcept
        {
            switch (flush)
            {
            case FlyoutHostBorderFlush::None:
                (void)DwmFlush();
                break;
            case FlyoutHostBorderFlush::FrameOnly:
                if (Shared::SystemHelper::g_buildNumber < 22000)
                {
                    // Win10 (<22000): синхронно отрисовываем отложенное 1-px NC-кольцо
                    // без SWP_DRAWFRAME / WM_ERASEBKGND, не затрагивая клиентскую область.
                    (void)SendMessageW(hwnd, WM_NCPAINT, static_cast<WPARAM>(1), 0);
                }
                else
                {
                    (void)RedrawWindow(
                        hwnd,
                        nullptr,
                        nullptr,
                        RDW_FRAME | RDW_NOCHILDREN);
                }
                (void)DwmFlush();
                break;
            case FlyoutHostBorderFlush::Full:
                (void)RedrawWindow(
                    hwnd,
                    nullptr,
                    nullptr,
                    RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOCHILDREN);
                break;
            }
        }
    } // namespace

    void SetHostBorderDeferred(HWND hWnd, bool deferred) noexcept
    {
        if (!hWnd)
            return;
        std::lock_guard lock(g_hostBorderDeferredMutex);
        if (deferred)
        {
            g_hostBorderDeferredHwnds.insert(hWnd);
            DebugLog(DBG_IMPORTANT,
                L"[GlassCore][FlyoutFrame] HostBorderDeferred hwnd=0x%p",
                hWnd);
        }
        else
        {
            g_hostBorderDeferredHwnds.erase(hWnd);
        }
    }

    bool IsHostBorderDeferred(HWND hWnd) noexcept
    {
        if (!hWnd)
            return false;
        std::lock_guard lock(g_hostBorderDeferredMutex);
        return g_hostBorderDeferredHwnds.find(hWnd) != g_hostBorderDeferredHwnds.end();
    }

    void ClearHostBorderDeferred(HWND hWnd) noexcept
    {
        if (!hWnd)
            return;
        std::lock_guard lock(g_hostBorderDeferredMutex);
        g_hostBorderDeferredHwnds.erase(hWnd);
    }

    void SyncPrimeFlyoutNcBlack(HWND hWnd) noexcept
    {
        if (!hWnd || !IsWindow(hWnd))
            return;

        (void)SendMessageW(hWnd, WM_NCPAINT, reinterpret_cast<WPARAM>(HRGN(1)), 0);
        (void)DwmFlush();

        DebugLog(DBG_IMPORTANT,
            L"[GlassCore][FlyoutFrame] PrimeFlyoutNcBlackBeforeCloak hwnd=0x%p",
            hWnd);
    }

    void PaintDeferredHostBorder(
        HWND hWnd,
        bool darkMode,
        const Settings::BorderContext& border,
        unsigned effectType,
        FlyoutHostBorderFlush flush) noexcept
    {
        if (!hWnd || !IsWindow(hWnd) || !IsHostBorderDeferred(hWnd))
            return;

        ClearHostBorderDeferred(hWnd);
        PaintHostBorder(hWnd, darkMode, border, effectType, flush);

        DebugLog(DBG_IMPORTANT,
            L"[GlassCore][FlyoutFrame] PaintDeferredHostBorder hwnd=0x%p flush=%u",
            hWnd,
            static_cast<unsigned>(flush));
    }

    void PaintHostBorder(
        HWND hwnd,
        bool darkMode,
        const Settings::BorderContext& border,
        unsigned effectType,
        FlyoutHostBorderFlush flush) noexcept
    {
        if (!hwnd || !IsWindow(hwnd))
            return;

        if (IsHostBorderDeferred(hwnd))
            return;

        Settings::ApplyBorderEffect(hwnd, darkMode, border);
        RefreshDwmNc(hwnd, effectType, border.cornerType);
        PaintNcRingGdi(hwnd, darkMode, border);
        ApplyHostBorderFlush(hwnd, flush);

        DebugLog(DBG_IMPORTANT,
            L"[GlassCore][FlyoutFrame] PaintHostBorder hwnd=0x%p corner=%d effectType=%u flush=%u",
            hwnd,
            static_cast<int>(border.cornerType),
            effectType,
            static_cast<unsigned>(flush));
    }

    void LogWin10PostUncloakSurfaceDiag(HWND hWnd, const wchar_t* tag) noexcept
    {
        if (Shared::SystemHelper::g_buildNumber >= 22000 || !hWnd || !IsWindow(hWnd))
            return;

        RECT wr{};
        if (!GetWindowRect(hWnd, &wr))
            return;
        const int w = wr.right - wr.left;
        const int h = wr.bottom - wr.top;

        DWORD cloaked = 0;
        const HRESULT hrCloak = DwmGetWindowAttribute(hWnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        const LONG_PTR style = GetWindowLongPtrW(hWnd, GWL_STYLE);
        const LONG_PTR exStyle = GetWindowLongPtrW(hWnd, GWL_EXSTYLE);

        HWND overlapHwnd = nullptr;
        wchar_t overlapCls[64]{};
        for (HWND prev = GetWindow(hWnd, GW_HWNDPREV); prev; prev = GetWindow(prev, GW_HWNDPREV))
        {
            if (!IsWindowVisible(prev))
                continue;
            RECT pr{};
            RECT inter{};
            if (GetWindowRect(prev, &pr) && IntersectRect(&inter, &wr, &pr))
            {
                overlapHwnd = prev;
                GetClassNameW(prev, overlapCls, static_cast<int>(std::size(overlapCls)));
                break;
            }
        }

        uint32_t px00 = 0;
        uint32_t pxMid = 0;
        unsigned clientNzRgb = 0;
        unsigned clientNzAlpha = 0;

        if (w > 0 && h > 0 && w <= 4096 && h <= 4096)
        {
            HDC hdcWin = GetWindowDC(hWnd);
            if (hdcWin)
            {
                HDC hdcMem = CreateCompatibleDC(hdcWin);
                if (hdcMem)
                {
                    BITMAPINFO bmi{};
                    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                    bmi.bmiHeader.biWidth = w;
                    bmi.bmiHeader.biHeight = -h;
                    bmi.bmiHeader.biPlanes = 1;
                    bmi.bmiHeader.biBitCount = 32;
                    bmi.bmiHeader.biCompression = BI_RGB;

                    void* bits = nullptr;
                    HBITMAP hbmp = CreateDIBSection(hdcWin, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
                    if (hbmp && bits)
                    {
                        HGDIOBJ oldBmp = SelectObject(hdcMem, hbmp);
                        if (BitBlt(hdcMem, 0, 0, w, h, hdcWin, 0, 0, SRCCOPY))
                        {
                            const auto* px = static_cast<const uint32_t*>(bits);
                            px00 = px[0];
                            pxMid = px[static_cast<size_t>(h / 2) * static_cast<size_t>(w) + static_cast<size_t>(w / 2)];
                            const int x0 = (w > 4) ? 2 : 0;
                            const int x1 = (w > 4) ? (w - 2) : w;
                            const int y0 = (h > 4) ? 2 : 0;
                            const int y1 = (h > 4) ? (h - 2) : h;
                            for (int y = y0; y < y1; ++y)
                            {
                                const uint32_t* row = px + static_cast<size_t>(y) * static_cast<size_t>(w);
                                for (int x = x0; x < x1; ++x)
                                {
                                    const uint32_t p = row[x];
                                    if ((p & 0x00FFFFFFu) != 0)
                                        ++clientNzRgb;
                                    if ((p & 0xFF000000u) != 0)
                                        ++clientNzAlpha;
                                }
                            }
                        }
                        SelectObject(hdcMem, oldBmp);
                    }
                    if (hbmp)
                        DeleteObject(hbmp);
                    DeleteDC(hdcMem);
                }
                ReleaseDC(hWnd, hdcWin);
            }
        }

        COLORREF scrMid = CLR_INVALID;
        if (HDC hdcScr = GetDC(nullptr))
        {
            scrMid = GetPixel(hdcScr, wr.left + w / 2, wr.top + h / 2);
            ReleaseDC(nullptr, hdcScr);
        }

        DebugLog(DBG_IMPORTANT,
            L"[GlassCore][Win10Diag] %s hwnd=0x%p %dx%d vis=%d cloaked=0x%X(hr=0x%08X) style=0x%08X ex=0x%08X px00=0x%08X pxMid=0x%08X nzRgb=%u nzA=%u scrMid=0x%08X overlap=0x%p(%s)",
            tag ? tag : L"?",
            hWnd,
            w, h,
            IsWindowVisible(hWnd) ? 1 : 0,
            cloaked,
            hrCloak,
            static_cast<unsigned>(style),
            static_cast<unsigned>(exStyle),
            px00,
            pxMid,
            clientNzRgb,
            clientNzAlpha,
            static_cast<unsigned>(scrMid),
            overlapHwnd,
            overlapCls[0] ? overlapCls : L"-");
    }
}
