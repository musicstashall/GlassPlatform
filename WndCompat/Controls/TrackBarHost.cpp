// Трекбар: без субкласса — маркер GlassControlKind::TrackBar; TBCD_TICS — parent NM_CUSTOMDRAW.
#include "WndPch.h"
#include "TrackBarHost.h"

#include "WinUiBridge/WinUiContentBridgeHost.h"
#include <WndCompat.h>
#include <GlassControlKind.h>
#include <Tracker/WinEventHandler.h>
#include <WinApiUtils.h>
#include <CommCtrl.h>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace
{
    std::vector<HWND> g_trackBarWindows;

    void SetTrackBarTransparentBkgnd(HWND hWnd, bool enable) noexcept
    {
        if (!hWnd || !IsWindow(hWnd))
            return;
        const LONG style = GetWindowLongW(hWnd, GWL_STYLE);
        const LONG next = enable
            ? (style | TBS_TRANSPARENTBKGND)
            : (style & ~TBS_TRANSPARENTBKGND);
        if (next != style)
            SetWindowLongW(hWnd, GWL_STYLE, next);
    }

    // Live-перерисовка: синхронный paint допустим.
    void RedrawTrackBarChrome(HWND hWnd) noexcept
    {
        if (!hWnd || !IsWindow(hWnd))
            return;
        RedrawWindow(hWnd, nullptr, nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    }

    // Shutdown/Detach: без UPDATENOW — иначе sync paint в полуразрушенный RootSubclass (AV в ntdll).
    void InvalidateTrackBarChrome(HWND hWnd) noexcept
    {
        if (!hWnd || !IsWindow(hWnd))
            return;
        InvalidateRect(hWnd, nullptr, TRUE);
    }
}

namespace WndCompat
{
    void TrackBar_Attach(HWND hWnd)
    {
        if (!IsWindow(hWnd))
            return;
        if (std::find(g_trackBarWindows.begin(), g_trackBarWindows.end(), hWnd) != g_trackBarWindows.end())
            return;

        g_trackBarWindows.push_back(hWnd);
        GlassShared::GlassMarkWindow(hWnd, GlassShared::GlassControlKind::TrackBar);

        const bool allowGlass = WndCompat::IsGlassVisualsAllowed();
        if (allowGlass)
            SetTrackBarTransparentBkgnd(hWnd, true);

        if (WndCompat::WinUi_IsUnderShellTrayRoot(hWnd))
            WndCompat::WinUi_TrayTrackBar_ReportIxdeIntent(hWnd);

        // Уже открытое окно: иначе остаётся тема до первой инвалидации.
        if (allowGlass)
            RedrawTrackBarChrome(hWnd);
    }

    void TrackBar_Detach(HWND hWnd)
    {
        bool wasTracked = false;
        auto it = std::find(g_trackBarWindows.begin(), g_trackBarWindows.end(), hWnd);
        if (it != g_trackBarWindows.end())
        {
            wasTracked = true;
            g_trackBarWindows.erase(it);
        }

        if (wasTracked && IsWindow(hWnd) && WndCompat::WinUi_IsUnderShellTrayRoot(hWnd))
            WndCompat::WinUi_TrayTrackBar_ReleaseIxdeIntent(hWnd);

        if (IsWindow(hWnd))
        {
            SetTrackBarTransparentBkgnd(hWnd, false);
            GlassShared::GlassUnmarkWindow(hWnd);
            InvalidateTrackBarChrome(hWnd);
        }

        DebugLog(L"[WndCompat][TrackBarHost] TrackBar_Detach: hwnd=0x%p", hWnd);
    }

    void TrackBar_DetachAll()
    {
        const auto windows = g_trackBarWindows;
        g_trackBarWindows.clear();

        for (HWND hWnd : windows)
        {
            if (!IsWindow(hWnd))
                continue;

            if (WndCompat::WinUi_IsUnderShellTrayRoot(hWnd))
                WndCompat::WinUi_TrayTrackBar_ReleaseIxdeIntent(hWnd);

            SetTrackBarTransparentBkgnd(hWnd, false);
            GlassShared::GlassUnmarkWindow(hWnd);
            InvalidateTrackBarChrome(hWnd);

            DebugLog(L"[WndCompat][TrackBarHost] TrackBar_DetachAll: detached 0x%p", hWnd);
        }
    }

    void TrackBar_ApplyBackdropPolicyToTracked() noexcept
    {
        const bool allowed = WndCompat::IsGlassVisualsAllowed();
        const auto windows = g_trackBarWindows;

        for (HWND hWnd : windows)
        {
            if (!hWnd || !IsWindow(hWnd))
                continue;
            SetTrackBarTransparentBkgnd(hWnd, allowed);
            RedrawTrackBarChrome(hWnd);
        }

        DebugLog(DBG_IMPORTANT,
            L"[WndCompat][TrackBarHost] TrackBar_ApplyBackdropPolicy: allowed=%d count=%zu",
            allowed ? 1 : 0,
            windows.size());
    }
}
