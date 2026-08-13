// ScrollBar (класс окна ScrollBar): без субкласса — только маркер GlassControlKind::ScrollBar
// для GlassCore: фильтр хостов и standalone-полосы (BitBlt-кадр темы + маска трека + GdiAlphaBlend).
#include "WndPch.h"
#include "ScrollBarHost.h"

#include <GlassControlKind.h>
#include <Tracker/WinEventHandler.h>

#include <algorithm>
#include <mutex>
#include <vector>

namespace
{
    std::mutex        g_sbMutex;
    std::vector<HWND> g_sbWindows;
}

namespace WndCompat
{
    void ScrollBar_Attach(HWND hWnd)
    {
        if (WinEventHook_GetMode() != Shared::InjectionMode::Full)
            return;
        if (!hWnd || !IsWindow(hWnd))
            return;

        using namespace GlassShared;
        if (GlassGetWindowKind(hWnd) == GlassControlKind::ScrollBar)
            return;

        {
            std::lock_guard lock(g_sbMutex);
            if (std::find(g_sbWindows.begin(), g_sbWindows.end(), hWnd) != g_sbWindows.end())
                return;
            g_sbWindows.push_back(hWnd);
        }

        GlassMarkWindow(hWnd, GlassControlKind::ScrollBar);
    }

    void ScrollBar_Detach(HWND hWnd)
    {
        {
            std::lock_guard lock(g_sbMutex);
            const auto it = std::find(g_sbWindows.begin(), g_sbWindows.end(), hWnd);
            if (it != g_sbWindows.end())
                g_sbWindows.erase(it);
        }
        if (IsWindow(hWnd))
            GlassShared::GlassUnmarkWindow(hWnd);
    }

    void ScrollBar_DetachAll()
    {
        std::vector<HWND> windows;
        {
            std::lock_guard lock(g_sbMutex);
            windows.swap(g_sbWindows);
        }
        for (HWND hWnd : windows)
        {
            if (IsWindow(hWnd))
                GlassShared::GlassUnmarkWindow(hWnd);
        }
    }

    void ScrollBar_ApplyBackdropPolicyToTracked() noexcept
    {
        std::vector<HWND> windows;
        {
            std::lock_guard lock(g_sbMutex);
            windows = g_sbWindows;
        }

        for (HWND hWnd : windows)
        {
            if (!hWnd || !IsWindow(hWnd))
                continue;
            RedrawWindow(hWnd, nullptr, nullptr,
                RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        }
    }
}
