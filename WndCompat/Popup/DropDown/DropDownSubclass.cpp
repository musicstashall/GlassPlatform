#include "WndPch.h"
#include "DropDownSubclass.h"
#include "DropDownContext.h"
#include <DropDownEventProvider.h>
#include <WndCompat.h>
#include <Subsystems/Settings.h>
#include <MenuUtils.h>
#include <dwmapi.h>
#include <ColorUtils.h>
#include <wil.h>

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

    void PaintDropDownNcBlackFill(HDC hdc, const RECT& paintRect) noexcept
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

    void PaintDropDownNcRingOnDc(
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

        PaintDropDownNcBlackFill(hdc, paintRect);

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

    void PaintDropDownNcRing(HWND hWnd, HRGN clipRgn,
        const GlassCore::Settings::BorderContext& border,
        bool darkMode) noexcept
    {
        HDC hdc = GetWindowDC(hWnd);
        if (!hdc)
            return;

        auto releaseDC = wil::scope_exit([&] { ReleaseDC(hWnd, hdc); });
        PaintDropDownNcRingOnDc(hdc, hWnd, clipRgn, false, border, darkMode);
    }

    LRESULT CALLBACK DropDownSubclassProc(
        HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
        UINT_PTR /*uIdSubclass*/, DWORD_PTR /*dwRefData*/)
    {
        switch (uMsg)
        {
        case WM_SHOWWINDOW:
            if (wParam)
            {
                DebugLog(L"[WndCompat][DropDown] DropDown: WM_SHOWWINDOW (show) hwnd=0x%p", hWnd);

                auto* p = WndCompat::DropDown::GetDropDownEventProvider();
                if (p && p->OnDropDownShow)
                    p->OnDropDownShow(hWnd);
                DebugLog(L"[WndCompat][DropDown] DropDown: provider=0x%p, OnDropDownShow=0x%p",
                    (void*)p,
                    p ? (void*)p->OnDropDownShow : nullptr);
            }
            break;

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
                    PaintDropDownNcBlackFill(hdc, paintRect);
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
                    PaintDropDownNcBlackFill(hdc, paintRect);
                    ReleaseDC(hWnd, hdc);
                }
                return 0;
            }

            PaintDropDownNcRing(hWnd, clipRgn, border, darkMode);
            return 0;
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
                PaintDropDownNcRingOnDc(
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

        case WM_NCDESTROY:
        {
            auto* provider = WndCompat::DropDown::GetDropDownEventProvider();
            if (provider && provider->ClearFlyoutBorderDeferred)
                provider->ClearFlyoutBorderDeferred(hWnd);
            WndCompat::DropDown_Detach(hWnd);
            break;
        }
        }

        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }
}

namespace WndCompat
{
    void DropDown_Attach(HWND hWnd)
    {
        if (!IsWindow(hWnd))
            return;

        if (SetWindowSubclass(hWnd, DropDownSubclassProc, 1, 0))
        {
            WndCompat::RegisterTextGlowPopupClientWithGlassCore(hWnd, 0);
            DebugLog(L"[WndCompat][DropDown] DropDown_Attach: subclassed 0x{:x}",
                reinterpret_cast<std::uintptr_t>(hWnd));
        }
        else
        {
            DebugLog(L"[WndCompat][DropDown] DropDown_Attach: FAILED 0x{:x}",
                reinterpret_cast<std::uintptr_t>(hWnd));
        }
    }

    void DropDown_AttachIfNeeded(HWND hWnd)
    {
        if (!IsWindow(hWnd))
            return;

        DWORD_PTR ref{};
        if (::GetWindowSubclass(hWnd, DropDownSubclassProc, 1, &ref) != FALSE)
            return;

        DropDown_Attach(hWnd);
    }

    void DropDown_Detach(HWND hWnd)
    {
        WndCompat::UnregisterTextGlowClientWithGlassCore(hWnd);
        if (IsWindow(hWnd))
            RemoveWindowSubclass(hWnd, DropDownSubclassProc, 1);

        DebugLog(L"[WndCompat][DropDown] DropDown_Detach: 0x{:x}",
            reinterpret_cast<std::uintptr_t>(hWnd));
    }
}
