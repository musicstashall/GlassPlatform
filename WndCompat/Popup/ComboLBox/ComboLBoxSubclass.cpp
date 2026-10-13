#include "WndPch.h"
#include "ComboLBoxSubclass.h"
#include <DropDownEventProvider.h>
#include <DropDownContext.h>
#include <Subsystems/Settings.h>
#include <dwmapi.h>
#include <ColorUtils.h>
#include <wil.h>
#include <WndCompat.h>
#include <GlassControlKind.h>
#include <MenuUtils.h>
#include <Tracker/WinEventHandler.h>

#include <vector>
#include <algorithm>
#include <windowsx.h>

namespace
{
    // Отдельная SEH-обёртка: в ComboLBoxSubclassProc есть C++ объекты (C2712).
    LRESULT SafeDefSubclassProcNc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) noexcept
    {
        __try
        {
            return DefSubclassProc(hWnd, uMsg, wParam, lParam);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
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

    [[nodiscard]] bool IsFlyoutBorderDeferredForDropDown(
        HWND hWnd,
        const WndCompat::DropDown::DropDownEventProvider* provider) noexcept
    {
        return provider && provider->IsFlyoutBorderDeferred
            && provider->IsFlyoutBorderDeferred(hWnd);
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

    void PaintComboLBoxNcBlackFill(HDC hdc, const RECT& paintRect) noexcept
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

    void PaintComboLBoxNcRingOnDc(
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

        PaintComboLBoxNcBlackFill(hdc, paintRect);

        HWND hwndForOutline = nullptr;
        if (border.cornerType == DWMWCP_DONOTROUND && !forThumbnailCapture)
            hwndForOutline = hWnd;

        (void)Shared::MenuUtils::PaintFlyoutDefaultBorderOutline(
            hdc, hwndForOutline, paintRect, ToMenuBorder(border), darkMode);
    }

    [[nodiscard]] bool PaintComboLBoxBorderRingOverlay(
        HDC hdc,
        HWND hWnd,
        HRGN clipRgn,
        const GlassCore::Settings::BorderContext& border,
        bool darkMode) noexcept
    {
        if (!hdc || !hWnd)
            return false;

        if (clipRgn)
            SelectClipRgn(hdc, clipRgn);

        const int savedDC = SaveDC(hdc);

        RECT paintRect{};
        GetWindowRect(hWnd, &paintRect);
        OffsetRect(&paintRect, -paintRect.left, -paintRect.top);

        RECT clientInWindow{};
        GetClientRect(hWnd, &clientInWindow);
        MapWindowPoints(hWnd, HWND_DESKTOP, reinterpret_cast<LPPOINT>(&clientInWindow), 2);
        RECT windowScreen{};
        GetWindowRect(hWnd, &windowScreen);
        OffsetRect(&clientInWindow, -windowScreen.left, -windowScreen.top);
        ExcludeClipRect(hdc,
            clientInWindow.left,
            clientInWindow.top,
            clientInWindow.right,
            clientInWindow.bottom);

        SCROLLBARINFO sbi{};
        sbi.cbSize = sizeof(sbi);
        if (::GetScrollBarInfo(hWnd, OBJID_VSCROLL, &sbi)
            && (sbi.rgstate[0] & STATE_SYSTEM_INVISIBLE) == 0)
        {
            RECT sbRect = sbi.rcScrollBar;
            OffsetRect(&sbRect, -windowScreen.left, -windowScreen.top);
            ExcludeClipRect(hdc, sbRect.left, sbRect.top, sbRect.right, sbRect.bottom);
        }

        if (border.colorUseNone)
        {
            PatBlt(hdc, paintRect.left, paintRect.top,
                paintRect.right - paintRect.left,
                paintRect.bottom - paintRect.top,
                BLACKNESS);

            RestoreDC(hdc, savedDC);

            DebugLog(DBG_IMPORTANT,
                L"[WndCompat][ComboLBox] ComboLBoxBorderRingOverlay defer=1 noBorder=1 blackNc=1");
            return true;
        }

        bool painted = Shared::MenuUtils::PaintFlyoutDefaultBorderOutline(
            hdc,
            hWnd,
            paintRect,
            ToMenuBorder(border),
            darkMode);

        RestoreDC(hdc, savedDC);

        DebugLog(DBG_IMPORTANT,
            L"[WndCompat][ComboLBox] ComboLBoxBorderRingOverlay defer=1 painted=%d cornerType=%d",
            painted ? 1 : 0,
            static_cast<int>(border.cornerType));

        return painted;
    }

    LRESULT CALLBACK ComboLBoxSubclassProc(
        HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
        UINT_PTR /*uIdSubclass*/, DWORD_PTR /*dwRefData*/)
    {
        // Снятие субкласса по запросу из чужого потока (ComboLBox_DetachAll → SendMessageTimeout).
        // Обрабатываем до шлюза IsShuttingDown — это и есть цель shutdown-пути.
        // RemoveWindowSubclass здесь безопасен: мы в потоке-владельце ComboLBox-окна.
        if (uMsg == WndCompat::WM_GLASS_DETACH_COMBOLVOX)
        {
            RemoveWindowSubclass(hWnd, ComboLBoxSubclassProc, 1);
            DebugLog(L"[WndCompat][ComboLBox] WM_GLASS_DETACH_COMBOLVOX: субкласс снят hwnd=0x%p", hWnd);
            WndCompat::WinEventHook_SignalDetachComplete();
            return 0;
        }

        if (WndCompat::IsShuttingDown())
            return DefSubclassProc(hWnd, uMsg, wParam, lParam);

        switch (uMsg)
        {
        case WM_SHOWWINDOW:
        {
            if (!wParam)
            {
                // ComboLBox скрывается — снимаем DWM-эффекты (backdrop, corners, border).
                // В отличие от меню, ComboLBox НЕ уничтожается при скрытии и живёт дальше.
                // При повторном показе OnComboLBoxShow восстановит эффекты.
                DebugLog(L"[WndCompat][ComboLBox] ComboLBox: WM_SHOWWINDOW (hide) hwnd=0x%p", hWnd);
                auto* p = WndCompat::DropDown::GetDropDownEventProvider();
                if (p && p->OnComboLBoxDetach)
                    p->OnComboLBoxDetach(hWnd);
                return DefSubclassProc(hWnd, uMsg, wParam, lParam);
            }
            DebugLog(L"[WndCompat][ComboLBox] ComboLBox: WM_SHOWWINDOW (show) hwnd=0x%p", hWnd);

            // Системная настройка анимации ComboBox (BWM pattern)
            BOOL sysAnimation = FALSE;
            SystemParametersInfoW(SPI_GETCOMBOBOXANIMATION, 0, &sysAnimation, 0);

            auto* p = WndCompat::DropDown::GetDropDownEventProvider();
            if (p && p->OnComboLBoxShow)
                p->OnComboLBoxShow(hWnd, sysAnimation != FALSE);

            // BWM: DefSubclassProc + RedrawWindow после применения эффектов
            auto result = DefSubclassProc(hWnd, uMsg, wParam, lParam);
            RedrawWindow(hWnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
            
            return result;
        }

        /*case WM_PAINT:
            return DefSubclassProc(hWnd, uMsg, wParam, lParam);*/

        case WM_ERASEBKGND:
        {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            RECT clientRect{};
            GetClientRect(hWnd, &clientRect);
            PatBlt(hdc, clientRect.left, clientRect.top,
                clientRect.right - clientRect.left,
                clientRect.bottom - clientRect.top,
                BLACKNESS);
            return TRUE;
        }

        case WM_NCPAINT:
        {
            auto* provider = WndCompat::DropDown::GetDropDownEventProvider();
            const bool defer = IsFlyoutBorderDeferredForDropDown(hWnd, provider);

            HRGN clipRgn = nullptr;
            if (wParam != NULLREGION && wParam != ERROR)
                clipRgn = reinterpret_cast<HRGN>(wParam);

            if (defer)
            {
                (void)SafeDefSubclassProcNc(hWnd, uMsg, wParam, lParam);

                HDC hdc = GetWindowDC(hWnd);
                if (hdc)
                {
                    if (clipRgn)
                        SelectClipRgn(hdc, clipRgn);

                    GlassCore::Settings::BorderContext border{};
                    bool darkMode = false;
                    if (QueryDropDownBorderSnapshot(provider, border, darkMode))
                    {
                        (void)PaintComboLBoxBorderRingOverlay(hdc, hWnd, nullptr, border, darkMode);
                    }
                    else
                    {
                        RECT paintRect{};
                        GetWindowRect(hWnd, &paintRect);
                        OffsetRect(&paintRect, -paintRect.left, -paintRect.top);
                        PaintComboLBoxNcBlackFill(hdc, paintRect);
                    }

                    ReleaseDC(hWnd, hdc);
                }
                return 0;
            }

            HDC hdc = GetWindowDC(hWnd);
            if (hdc)
            {
                if (clipRgn)
                    SelectClipRgn(hdc, clipRgn);

                RECT paintRect{};
                GetWindowRect(hWnd, &paintRect);
                OffsetRect(&paintRect, -paintRect.left, -paintRect.top);

                GlassCore::Settings::BorderContext border{};
                bool darkMode = false;
                if (QueryDropDownBorderSnapshot(provider, border, darkMode))
                {
                    PaintComboLBoxNcRingOnDc(hdc, hWnd, clipRgn, false, border, darkMode);
                }
                else
                {
                    PaintComboLBoxNcBlackFill(hdc, paintRect);
                }

                ReleaseDC(hWnd, hdc);
            }

            return 0;
        }

        case WM_PRINT:
        {
            auto result = DefSubclassProc(hWnd, uMsg, wParam, lParam);
            auto* provider = WndCompat::DropDown::GetDropDownEventProvider();
            const bool defer = IsFlyoutBorderDeferredForDropDown(hWnd, provider);

            GlassCore::Settings::BorderContext border{};
            bool darkMode = false;
            const bool hasSnapshot = QueryDropDownBorderSnapshot(provider, border, darkMode);

            if (defer && (lParam & PRF_CLIENT))
            {
                if (hasSnapshot)
                {
                    (void)Shared::MenuUtils::PaintFlyoutClientEdgeBorderForThumbnail(
                        reinterpret_cast<HDC>(wParam),
                        hWnd,
                        ToMenuBorder(border),
                        darkMode);
                }
                else
                {
                    RECT clientRect{};
                    if (GetClientRect(hWnd, &clientRect))
                    {
                        FrameRect(
                            reinterpret_cast<HDC>(wParam),
                            &clientRect,
                            static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
                        DebugLog(DBG_IMPORTANT,
                            L"[WndCompat][ComboLBox] WM_PRINT client-edge fallback black hwnd=0x%p defer=1",
                            hWnd);
                    }
                }
            }
            else if (!defer && hasSnapshot && (lParam & PRF_NONCLIENT))
            {
                PaintComboLBoxNcRingOnDc(
                    reinterpret_cast<HDC>(wParam),
                    hWnd,
                    nullptr,
                    true,
                    border,
                    darkMode);
            }
            return result;
        }

        case WM_NCDESTROY:
        {
            auto* p = WndCompat::DropDown::GetDropDownEventProvider();
            if (p && p->OnComboLBoxDetach)
                p->OnComboLBoxDetach(hWnd);

            WndCompat::ComboLBox_Detach(hWnd);
            break;
        }
        }

        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }
}

namespace
{
    static std::vector<HWND> g_comboLBoxWindows;
}

namespace WndCompat
{
    bool ComboLBox_IsAttached(HWND hWnd)
    {
        return std::find(g_comboLBoxWindows.begin(), g_comboLBoxWindows.end(), hWnd)
            != g_comboLBoxWindows.end();
    }

    void ComboLBox_Attach(HWND hWnd)
    {
        if (!IsWindow(hWnd))
            return;

        // Idempotent: не дублируем
        if (std::find(g_comboLBoxWindows.begin(), g_comboLBoxWindows.end(), hWnd) != g_comboLBoxWindows.end())
            return;

        if (SetWindowSubclass(hWnd, ComboLBoxSubclassProc, 1, 0))
        {
            g_comboLBoxWindows.push_back(hWnd);
            GlassShared::GlassMarkWindow(hWnd, GlassShared::GlassControlKind::ComboLBox);
            WndCompat::RegisterTextGlowPopupClientWithGlassCore(hWnd, 0);
            DebugLog(L"[WndCompat][ComboLBox] ComboLBox_Attach: subclassed 0x%p", hWnd);
        }
        else
        {
            DebugLog(L"[WndCompat][ComboLBox] ComboLBox_Attach: FAILED 0x%p", hWnd);
        }
    }

    void ComboLBox_Detach(HWND hWnd)
    {
        WndCompat::UnregisterTextGlowClientWithGlassCore(hWnd);
        GlassShared::GlassUnmarkWindow(hWnd);
        if (IsWindow(hWnd))
            RemoveWindowSubclass(hWnd, ComboLBoxSubclassProc, 1);

        auto it = std::find(g_comboLBoxWindows.begin(), g_comboLBoxWindows.end(), hWnd);
        if (it != g_comboLBoxWindows.end())
            g_comboLBoxWindows.erase(it);

        DebugLog(L"[WndCompat][ComboLBox] ComboLBox_Detach: 0x%p", hWnd);
    }

    void ComboLBox_DetachAll()
    {
        auto windows = g_comboLBoxWindows;
        g_comboLBoxWindows.clear();

        const DWORD curTid = GetCurrentThreadId();

        for (HWND hWnd : windows)
        {
            if (!IsWindow(hWnd))
                continue;

            // GlassCore cleanup (сброс DWM-атрибутов) — DWM API потокобезопасен,
            // вызываем всегда до снятия субкласса.
            auto* p = WndCompat::DropDown::GetDropDownEventProvider();
            if (p && p->OnComboLBoxDetach)
                p->OnComboLBoxDetach(hWnd);

            const DWORD wndTid = GetWindowThreadProcessId(hWnd, nullptr);

            if (wndTid == curTid)
            {
                // Тот же поток — прямой вызов безопасен
                RemoveWindowSubclass(hWnd, ComboLBoxSubclassProc, 1);
                DebugLog(L"[WndCompat][ComboLBox] ComboLBox_DetachAll: detached 0x%p", hWnd);
            }
            else
            {
                WinEventHook_RegisterPendingDetach();
                BOOL posted = PostMessageW(hWnd, WM_GLASS_DETACH_COMBOLVOX, 0, 0);
                if (!posted)
                {
                    WinEventHook_SignalDetachComplete();
                    DebugLog(L"[WndCompat][ComboLBox] ComboLBox_DetachAll: PostMessageW FAILED for 0x%p", hWnd);
                }
                else
                {
                    DebugLog(L"[WndCompat][ComboLBox] ComboLBox_DetachAll: cross-thread detached hwnd=0x%p",
                        hWnd);
                }
            }
        }
    }
}
