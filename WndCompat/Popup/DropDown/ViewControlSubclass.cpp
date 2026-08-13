#include "WndPch.h"
#include <WndCompat.h>
#include "ViewControlSubclass.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <ColorUtils.h>
#include <wil.h>
#include <Tracker/WindowClassUtils.h>
#include <Tracker/WinEventHandler.h>
#include <DropDownContext.h>
#include <DropDownEventProvider.h>
#include <MenuUtils.h>

namespace
{
    [[nodiscard]] bool IsFlyoutBorderDeferredForDropDown(
        HWND hWnd,
        const WndCompat::DropDown::DropDownEventProvider* provider) noexcept
    {
        return provider && provider->IsFlyoutBorderDeferred
            && provider->IsFlyoutBorderDeferred(hWnd);
    }

    [[nodiscard]] WndCompat::Menu::BorderContext ToMenuBorder(
        const GlassCore::Settings::BorderContext& border) noexcept
    {
        WndCompat::Menu::BorderContext out{};
        out.colorUseNone     = border.colorUseNone;
        out.colorUseDefault  = border.colorUseDefault;
        out.color            = border.color;
        out.cornerType       = border.cornerType;
        return out;
    }

    [[nodiscard]] bool QueryDropDownBorderSnapshot(
        const WndCompat::DropDown::DropDownEventProvider* provider,
        GlassCore::Settings::BorderContext& border,
        bool& darkMode) noexcept
    {
        WndCompat::DropDownContext ctx{};
        if (provider && provider->QueryDropDownContext && provider->QueryDropDownContext(&ctx))
        {
            border = ctx.border;
            darkMode = ctx.useDarkMode;
            return true;
        }
        return false;
    }

    void PaintViewControlNcBlackFill(HDC hdc, const RECT& paintRect) noexcept
    {
        constexpr MARGINS mr{ 1, 1, 1, 1 };
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

    void PaintViewControlNcRingOnDc(
        HDC hdc,
        HWND hWnd,
        HRGN clipRgn,
        bool forThumbnailCapture,
        const GlassCore::Settings::BorderContext& border,
        bool darkMode) noexcept
    {
        if (!hdc || !hWnd)
            return;

        if (clipRgn)
            SelectClipRgn(hdc, clipRgn);

        RECT paintRect{};
        GetWindowRect(hWnd, &paintRect);
        OffsetRect(&paintRect, -paintRect.left, -paintRect.top);

        PaintViewControlNcBlackFill(hdc, paintRect);

        HWND hwndForOutline = nullptr;
        if (border.cornerType == DWMWCP_DONOTROUND && !forThumbnailCapture)
            hwndForOutline = hWnd;

        (void)Shared::MenuUtils::PaintFlyoutDefaultBorderOutline(
            hdc, hwndForOutline, paintRect, ToMenuBorder(border), darkMode);

        if (border.cornerType != DWMWCP_DONOTROUND && !forThumbnailCapture)
        {
            (void)RedrawWindow(hWnd, nullptr, nullptr,
                RDW_FRAME | RDW_UPDATENOW | RDW_NOCHILDREN);
        }
    }

    void PaintViewControlNcRing(HWND hWnd, HRGN clipRgn,
        const GlassCore::Settings::BorderContext& border,
        bool darkMode) noexcept
    {
        HDC hdc = GetWindowDC(hWnd);
        if (!hdc)
            return;

        auto releaseDC = wil::scope_exit([&] { ReleaseDC(hWnd, hdc); });
        PaintViewControlNcRingOnDc(hdc, hWnd, clipRgn, false, border, darkMode);
    }

    LRESULT CALLBACK ViewControlSubclassProc(
        HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
        UINT_PTR /*uIdSubclass*/, DWORD_PTR /*dwRefData*/)
    {
        switch (uMsg)
        {
        case WM_NCPAINT:
        {
            auto* provider = WndCompat::DropDown::GetDropDownEventProvider();
            HRGN clipRgn = nullptr;
            if (wParam != NULLREGION && wParam != ERROR)
                clipRgn = reinterpret_cast<HRGN>(wParam);

            if (IsFlyoutBorderDeferredForDropDown(hWnd, provider))
            {
                HDC hdc = GetWindowDC(hWnd);
                if (hdc)
                {
                    if (clipRgn)
                        SelectClipRgn(hdc, clipRgn);

                    RECT paintRect{};
                    GetWindowRect(hWnd, &paintRect);
                    OffsetRect(&paintRect, -paintRect.left, -paintRect.top);
                    PaintViewControlNcBlackFill(hdc, paintRect);
                    ReleaseDC(hWnd, hdc);
                }
                return 0;
            }

            GlassCore::Settings::BorderContext border{};
            bool darkMode = false;
            if (!QueryDropDownBorderSnapshot(provider, border, darkMode))
            {
                HDC hdc = GetWindowDC(hWnd);
                if (hdc)
                {
                    if (clipRgn)
                        SelectClipRgn(hdc, clipRgn);

                    RECT paintRect{};
                    GetWindowRect(hWnd, &paintRect);
                    OffsetRect(&paintRect, -paintRect.left, -paintRect.top);
                    PaintViewControlNcBlackFill(hdc, paintRect);
                    ReleaseDC(hWnd, hdc);
                }
                return 0;
            }

            PaintViewControlNcRing(hWnd, clipRgn, border, darkMode);
            return 0;
        }

        case WM_PAINT:
        {
            auto* provider = WndCompat::DropDown::GetDropDownEventProvider();
            LRESULT result = DefSubclassProc(hWnd, uMsg, wParam, lParam);
            if (!IsFlyoutBorderDeferredForDropDown(hWnd, provider))
            {
                GlassCore::Settings::BorderContext border{};
                bool darkMode = false;
                if (QueryDropDownBorderSnapshot(provider, border, darkMode))
                    PaintViewControlNcRing(hWnd, nullptr, border, darkMode);
            }
            return result;
        }

        case WM_PRINT:
        {
            auto result = DefSubclassProc(hWnd, uMsg, wParam, lParam);
            auto* provider = WndCompat::DropDown::GetDropDownEventProvider();
            const bool defer = IsFlyoutBorderDeferredForDropDown(hWnd, provider);

            GlassCore::Settings::BorderContext border{};
            bool darkMode = false;
            if (!QueryDropDownBorderSnapshot(provider, border, darkMode))
                return result;

            if (defer && (lParam & PRF_CLIENT))
            {
                (void)Shared::MenuUtils::PaintFlyoutClientEdgeBorderForThumbnail(
                    reinterpret_cast<HDC>(wParam),
                    hWnd,
                    ToMenuBorder(border),
                    darkMode);
            }
            else if (!defer && (lParam & PRF_NONCLIENT))
            {
                PaintViewControlNcRingOnDc(
                    reinterpret_cast<HDC>(wParam),
                    hWnd,
                    nullptr,
                    true,
                    border,
                    darkMode);
            }
            return result;
        }

        case WM_ERASEBKGND:
        {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            RECT clipRect{};
            GetClipBox(hdc, &clipRect);
            PatBlt(hdc, clipRect.left, clipRect.top,
                clipRect.right - clipRect.left,
                clipRect.bottom - clipRect.top,
                BLACKNESS);
            return TRUE;
        }

        case WM_PRINTCLIENT:
        {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            RECT clipRect{};
            GetClipBox(hdc, &clipRect);
            PatBlt(hdc, clipRect.left, clipRect.top,
                clipRect.right - clipRect.left,
                clipRect.bottom - clipRect.top,
                BLACKNESS);
            return TRUE;
        }

        case WM_CTLCOLORSTATIC:
        {
            DefSubclassProc(hWnd, uMsg, wParam, lParam);

            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetBkColor(hdc, 0);
            return reinterpret_cast<LRESULT>(GetStockObject(BLACK_BRUSH));
        }

        case WM_NOTIFY:
        {
            auto* nmhdr = reinterpret_cast<LPNMHDR>(lParam);
            if (nmhdr && nmhdr->code == NM_CUSTOMDRAW)
            {
                auto* nmcd = reinterpret_cast<LPNMCUSTOMDRAW>(lParam);
                HWND hwndFrom = nmhdr->hwndFrom;

                if (WndCompat::IsWindowClass(hwndFrom, L"msctls_trackbar32")
                    && WndCompat::IsGlassVisualsAllowed())
                {
                    switch (nmcd->dwDrawStage)
                    {
                    case CDDS_PREPAINT:
                        return CDRF_NOTIFYITEMDRAW;

                    case CDDS_ITEMPREPAINT:
                        if (nmcd->dwItemSpec == TBCD_TICS)
                        {
                            HDC  hdc = nmcd->hdc;
                            RECT rc = nmcd->rc;
                            PatBlt(hdc, rc.left, rc.top,
                                rc.right - rc.left, rc.bottom - rc.top,
                                BLACKNESS);
                            return CDRF_SKIPDEFAULT;
                        }
                        break;
                    }
                }
            }
            break;
        }

        case WM_NCDESTROY:
        {
            auto* provider = WndCompat::DropDown::GetDropDownEventProvider();
            if (provider && provider->ClearFlyoutBorderDeferred)
                provider->ClearFlyoutBorderDeferred(hWnd);
            WndCompat::ViewControl_Detach(hWnd);
            break;
        }
        }

        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }
}

namespace WndCompat
{
    void ViewControl_Attach(HWND hWnd)
    {
        if (!IsWindow(hWnd))
            return;

        auto* p = WndCompat::DropDown::GetDropDownEventProvider();
        if (p && p->OnViewControlAttach)
            p->OnViewControlAttach(hWnd);

        if (SetWindowSubclass(hWnd, ViewControlSubclassProc, 1, 0))
        {
            WndCompat::RegisterTextGlowPopupClientWithGlassCore(hWnd, 0);
            DebugLog(L"[WndCompat][ViewControl] ViewControl_Attach: subclassed 0x{:x}",
                reinterpret_cast<std::uintptr_t>(hWnd));
        }
        else
        {
            DebugLog(L"[WndCompat][ViewControl] ViewControl_Attach: FAILED 0x{:x}",
                reinterpret_cast<std::uintptr_t>(hWnd));
        }
    }

    void ViewControl_AttachIfNeeded(HWND hWnd)
    {
        if (!IsWindow(hWnd) || WndCompat::IsShuttingDown())
            return;

        DWORD_PTR ref{};
        if (::GetWindowSubclass(hWnd, ViewControlSubclassProc, 1, &ref) != FALSE)
            return;

        ViewControl_Attach(hWnd);
    }

    void ViewControl_Detach(HWND hWnd)
    {
        WndCompat::UnregisterTextGlowClientWithGlassCore(hWnd);
        if (IsWindow(hWnd))
            RemoveWindowSubclass(hWnd, ViewControlSubclassProc, 1);

        DebugLog(L"[WndCompat][ViewControl] ViewControl_Detach: 0x{:x}",
            reinterpret_cast<std::uintptr_t>(hWnd));
    }
}
