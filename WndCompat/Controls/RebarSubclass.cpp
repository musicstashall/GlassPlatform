#include "WndPch.h"
#include "RebarSubclass.h"
#include <Tracker/WinEventHandler.h>
#include <WndCompat.h>
#include <InjectionMode.h>
#include <GlassMessages.h>
#include <GlassControlKind.h>
#include <RebarEventProvider.h>
#include <GlassBackdropMark.h>
#include <RootPolicyQuery.h>
#include <RootEventProvider.h>

#include <vector>
#include <algorithm>

namespace
{
    std::vector<HWND> g_rebarWindows;

    LRESULT CALLBACK RebarSubclassProc(
        HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
        UINT_PTR /*id*/, DWORD_PTR /*data*/)
    {
        // Снятие субкласса из чужого потока (Rebar_DetachAll -> SendMessageTimeout).
        if (uMsg == WndCompat::WM_GLASS_DETACH_REBAR)
        {
            WndCompat::Rebar_Detach(hWnd);
            DebugLog(L"[WndCompat][Rebar] WM_GLASS_DETACH_REBAR: снят hwnd=0x%p", hWnd);
            WndCompat::WinEventHook_SignalDetachComplete();
            return 0;
        }

        if (WndCompat::IsShuttingDown())
            return DefSubclassProc(hWnd, uMsg, wParam, lParam);

        switch (uMsg)
        {
        case WM_PAINT:
        {
            if (!GlassShared::GlassIsBackdropAllowedForHwnd(hWnd))
                return DefSubclassProc(hWnd, uMsg, wParam, lParam);

            // Включаем DrawThemeBackground хук -> DefSubclassProc -> выключаем.
            // Хук заменяет тему "Rebar" на PatBlt(BLACKNESS).
            auto* p = WndCompat::Rebar::GetRebarEventProvider();
            if (p && p->OnPaintBegin)
                p->OnPaintBegin(hWnd);

            LRESULT result = DefSubclassProc(hWnd, uMsg, wParam, lParam);

            if (p && p->OnPaintEnd)
                p->OnPaintEnd();
            return result;
        }

        case WM_ERASEBKGND:
        {
            if (!GlassShared::GlassIsBackdropAllowedForHwnd(hWnd))
                return DefSubclassProc(hWnd, uMsg, wParam, lParam);

            // Заливаем фон чёрным (прозрачным) — стекло родительского окна просвечивает.
            auto hdc = reinterpret_cast<HDC>(wParam);
            RECT clipRect{};
            GetClipBox(hdc, &clipRect);
            PatBlt(hdc, clipRect.left, clipRect.top,
                clipRect.right - clipRect.left,
                clipRect.bottom - clipRect.top, BLACKNESS);
            return TRUE;
        }

        case WM_PRINTCLIENT:
        {
            if (!GlassShared::GlassIsBackdropAllowedForHwnd(hWnd))
                return DefSubclassProc(hWnd, uMsg, wParam, lParam);

            // Чёрная заливка для off-screen рендеринга (анимации и т.д.).
            auto hdc = reinterpret_cast<HDC>(wParam);
            RECT rc{};
            GetClientRect(hWnd, &rc);
            PatBlt(hdc, rc.left, rc.top,
                rc.right - rc.left, rc.bottom - rc.top, BLACKNESS);
            return TRUE;
        }

        case WM_NCDESTROY:
            WndCompat::Rebar_Detach(hWnd);
            break;
        }

        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }
}

namespace WndCompat
{
    void Rebar_Attach(HWND hWnd)
    {
        if (WinEventHook_GetMode() != Shared::InjectionMode::Full)
            return;
        if (!IsWindow(hWnd))
            return;
        if (std::find(g_rebarWindows.begin(), g_rebarWindows.end(), hWnd) != g_rebarWindows.end())
            return;
        if (!SetWindowSubclass(hWnd, RebarSubclassProc, 1, 0))
        {
            DebugLog(DBG_WARNING, L"[WndCompat][Rebar] Rebar_Attach: SetWindowSubclass FAILED hwnd=0x%p", hWnd);
            return;
        }

        GlassShared::GlassMarkWindow(hWnd, GlassShared::GlassControlKind::Rebar);
        WndCompat::TryRegisterTextGlowClientWithGlassCore(hWnd);
        g_rebarWindows.push_back(hWnd);
        DebugLog(L"[WndCompat][Rebar] Rebar_Attach: субкласс установлен hwnd=0x%p", hWnd);
    }

    void Rebar_Detach(HWND hWnd)
    {
        WndCompat::UnregisterTextGlowClientWithGlassCore(hWnd);
        GlassShared::GlassUnmarkWindow(hWnd);

        auto it = std::find(g_rebarWindows.begin(), g_rebarWindows.end(), hWnd);
        if (it != g_rebarWindows.end())
            g_rebarWindows.erase(it);

        if (IsWindow(hWnd))
            RemoveWindowSubclass(hWnd, RebarSubclassProc, 1);

        DebugLog(L"[WndCompat][Rebar] Rebar_Detach: hwnd=0x%p", hWnd);
    }

    void Rebar_DetachAll()
    {
        auto windows = g_rebarWindows;
        g_rebarWindows.clear();

        const DWORD curTid = GetCurrentThreadId();

        for (HWND hWnd : windows)
        {
            if (!IsWindow(hWnd))
                continue;

            const DWORD wndTid = GetWindowThreadProcessId(hWnd, nullptr);
            if (wndTid == curTid)
            {
                RemoveWindowSubclass(hWnd, RebarSubclassProc, 1);
                DebugLog(L"[WndCompat][Rebar] Rebar_DetachAll: detached 0x%p", hWnd);
            }
            else
            {
                WinEventHook_RegisterPendingDetach();
                BOOL posted = PostMessageW(hWnd, WM_GLASS_DETACH_REBAR, 0, 0);
                if (!posted)
                {
                    WinEventHook_SignalDetachComplete();
                    DebugLog(L"[WndCompat][Rebar] Rebar_DetachAll: PostMessageW FAILED for 0x%p", hWnd);
                }
                else
                {
                    DebugLog(L"[WndCompat][Rebar] Rebar_DetachAll: cross-thread detached hwnd=0x%p", hWnd);
                }
            }
        }
    }

    void Rebar_ApplyBackdropPolicyToTracked() noexcept
    {
        const bool allowed = WndCompat::IsGlassVisualsAllowed();
        const auto windows = g_rebarWindows;

        for (HWND hWnd : windows)
        {
            if (!hWnd || !IsWindow(hWnd))
                continue;

            RedrawWindow(hWnd, nullptr, nullptr,
                RDW_INVALIDATE | RDW_ERASE | RDW_FRAME);
        }

        DebugLog(DBG_IMPORTANT,
            L"[WndCompat][Rebar] Rebar_ApplyBackdropPolicy: allowed=%d count=%zu",
            allowed ? 1 : 0,
            windows.size());
    }
}
