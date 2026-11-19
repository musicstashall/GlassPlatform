// Трекбар: субкласс доаттаченного слайдера только для WM_ERASEBKGND (прозрачный фон),
// GlassMarkWindow; TBCD_TICS — parent NM_CUSTOMDRAW (ViewControlSubclassProc).
#include "WndPch.h"
#include "TrackBarHost.h"

#include "WinUiBridge/WinUiContentBridgeHost.h"
#include <WndCompat.h>
#include <GlassControlKind.h>
#include <GlassMessages.h>
#include <Tracker/WinEventHandler.h>
#include <WinApiUtils.h>
#include "../Tracker/WindowClassUtils.h"
#include <Helpers/ThemeHelper.h>
#include <CommCtrl.h>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace
{
    std::vector<HWND> g_trackBarWindows;

    // Идентификатор субкласса трекбара (не пересекается с ComboLBox(1)/DropDownNotify(2)).
    constexpr UINT_PTR kTrackBarSubclassId = 3;

    // Трекбар может находиться не непосредственно в ViewControlClass, а глубже
    // (внутри панели/сатичного контейнера попапа «Настройки вида»).
    // Поднимаемся по цепочке родителей — ограниченная глубина, окно может быть живым
    // на момент CREATE и стать "сиротой" на границе shutdown.
    bool IsUnderViewControlPopup(HWND hWnd) noexcept
    {
        HWND cur = GetParent(hWnd);
        for (int depth = 0; cur && depth < 8; ++depth)
        {
            if (!IsWindow(cur))
                return false;
            if (WndCompat::IsWindowClass(cur, L"ViewControlClass"))
                return true;
            cur = GetParent(cur);
        }
        return false;
    }

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

    LRESULT CALLBACK TrackBarSubclassProc(
        HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
        UINT_PTR /*uIdSubclass*/, DWORD_PTR /*dwRefData*/)
    {
        // Отложенное снятие по запросу из чужого потока (TrackBar_DetachAll).
        if (uMsg == WndCompat::WM_GLASS_DETACH_TRACKBAR)
        {
            RemoveWindowSubclass(hWnd, TrackBarSubclassProc, kTrackBarSubclassId);
            DebugLog(L"[WndCompat][TrackBarHost] WM_GLASS_DETACH_TRACKBAR: субкласс снят hwnd=0x%p", hWnd);
            WndCompat::WinEventHook_SignalDetachComplete();
            return 0;
        }

        if (WndCompat::IsShuttingDown())
            return DefSubclassProc(hWnd, uMsg, wParam, lParam);

        switch (uMsg)
        {
        case WM_ERASEBKGND:
            // Дефолтный класс msctls_trackbar32 стирает клиент кистью (COLOR_WINDOW) —
            // непрозрачная плашка поверх стеклянного попапа. TBS_TRANSPARENTBKGND
            // отвечает только за WM_PAINT; стирание подавляем здесь целиком.
            return TRUE;

        case WM_NCDESTROY:
            RemoveWindowSubclass(hWnd, TrackBarSubclassProc, kTrackBarSubclassId);
            return DefSubclassProc(hWnd, uMsg, wParam, lParam);
        }

        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }

    // Снятие субкласса: тот же поток — прямым RemoveWindowSubclass, чужой — через
    // WM_GLASS_DETACH_TRACKBAR на поток-владелец (callback: TrackBarSubclassProc).
    void RemoveTrackBarSubclass(HWND hWnd) noexcept
    {
        if (!hWnd || !IsWindow(hWnd))
            return;

        const DWORD curTid = GetCurrentThreadId();
        const DWORD wndTid = GetWindowThreadProcessId(hWnd, nullptr);

        if (wndTid == curTid)
        {
            RemoveWindowSubclass(hWnd, TrackBarSubclassProc, kTrackBarSubclassId);
            return;
        }

        WndCompat::WinEventHook_RegisterPendingDetach();
        if (!PostMessageW(hWnd, WndCompat::WM_GLASS_DETACH_TRACKBAR, 0, 0))
        {
            WndCompat::WinEventHook_SignalDetachComplete();
            DebugLog(L"[WndCompat][TrackBarHost] WM_GLASS_DETACH_TRACKBAR: PostMessageW FAILED hwnd=0x%p", hWnd);
        }
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

        // Full: как раньше (IsGlassVisualsAllowed = GlassRendering + Full).
        // PopupOnly: слайдер внутри ViewControlClass-попапа («Настройки вида») — тоже,
        // иначе opaque-фон трекбара остаётся плашкой, и «слайдер не получает патч».
        const bool isViewControlChild = IsUnderViewControlPopup(hWnd);
        const bool allowGlass = WndCompat::IsGlassVisualsAllowed()
            || (WndCompat::WinEventHook_GetMode() == Shared::InjectionMode::PopupOnly
                && isViewControlChild);
        if (allowGlass)
        {
            SetTrackBarTransparentBkgnd(hWnd, true);
            // Без Root_Attach на попапе (PopupOnly) dark-mode для конкретного контрола
            // никто не включит — иначе трек/ползунок рисуются светлой темой на тёмном стекле.
            Shared::ThemeHelper::EnableWindowDarkModeUxTheme(
                hWnd, Shared::ThemeHelper::ResolveEffectiveDarkMode());
            if (!SetWindowSubclass(hWnd, TrackBarSubclassProc, kTrackBarSubclassId, 0))
            {
                DebugLog(DBG_IMPORTANT,
                    L"[WndCompat][TrackBarHost] TrackBar_Attach: FAILED to subclass 0x%p", hWnd);
            }
        }

        DebugLog(DBG_IMPORTANT,
            L"[WndCompat][TrackBarHost] TrackBar_Attach: hwnd=0x%p glassAllowed=%d viewControlChild=%d",
            hWnd, allowGlass ? 1 : 0, isViewControlChild ? 1 : 0);

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
            RemoveTrackBarSubclass(hWnd);
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
            RemoveTrackBarSubclass(hWnd);
            GlassShared::GlassUnmarkWindow(hWnd);
            InvalidateTrackBarChrome(hWnd);

            DebugLog(L"[WndCompat][TrackBarHost] TrackBar_DetachAll: detached 0x%p", hWnd);
        }
    }

    void TrackBar_ApplyBackdropPolicyToTracked() noexcept
    {
        const bool glassPolicy = WndCompat::IsGlassVisualsAllowed();
        const auto windows = g_trackBarWindows;

        for (HWND hWnd : windows)
        {
            if (!hWnd || !IsWindow(hWnd))
                continue;
            const bool isViewControlChild = IsUnderViewControlPopup(hWnd);
            const bool allowed = glassPolicy
                || (WndCompat::WinEventHook_GetMode() == Shared::InjectionMode::PopupOnly
                    && isViewControlChild);
            SetTrackBarTransparentBkgnd(hWnd, allowed);
            if (allowed)
            {
                if (!::GetWindowSubclass(hWnd, TrackBarSubclassProc, kTrackBarSubclassId, nullptr))
                    (void)SetWindowSubclass(hWnd, TrackBarSubclassProc, kTrackBarSubclassId, 0);
                Shared::ThemeHelper::EnableWindowDarkModeUxTheme(
                    hWnd, Shared::ThemeHelper::ResolveEffectiveDarkMode());
            }
            else
            {
                RemoveTrackBarSubclass(hWnd);
            }
            RedrawTrackBarChrome(hWnd);
        }

        DebugLog(DBG_IMPORTANT,
            L"[WndCompat][TrackBarHost] TrackBar_ApplyBackdropPolicy: glassPolicy=%d count=%zu",
            glassPolicy ? 1 : 0,
            windows.size());
    }
}
