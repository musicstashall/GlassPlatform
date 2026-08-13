// Простая маркировка ToolbarWindow32 для PaintDispatcher.
// Без SubclassProc — все хуки ставятся через ToolbarHooks (GlassCore).

#include "WndPch.h"
#include "ToolBarSubclass.h"
#include <WndCompat.h>
#include <GlassControlKind.h>
#include <RootPolicyQuery.h>
#include <RootEventProvider.h>
#include "Tracker/WinEventHandler.h"

#include <vector>
#include <mutex>
#include <algorithm>

namespace
{
    std::vector<HWND> g_toolbarWindows;
    std::mutex        g_toolbarMutex;
}

namespace WndCompat
{
    void ToolBar_Attach(HWND hWnd)
    {
        if (!IsWindow(hWnd))
            return;

        if (GlassShared::GlassGetWindowKind(hWnd) == GlassShared::GlassControlKind::Toolbar)
            return;

        GlassShared::GlassMarkWindow(hWnd, GlassShared::GlassControlKind::Toolbar);
        WndCompat::TryRegisterTextGlowClientWithGlassCore(hWnd);

        {
            std::lock_guard lock(g_toolbarMutex);
            g_toolbarWindows.push_back(hWnd);
        }
        
        DebugLog(L"[WndCompat][ToolBar] ToolBar_Attach: mark hwnd=0x%p", hWnd);
    }

    void ToolBar_Detach(HWND hWnd)
    {
        WndCompat::UnregisterTextGlowClientWithGlassCore(hWnd);
        GlassShared::GlassUnmarkWindow(hWnd);

        {
            std::lock_guard lock(g_toolbarMutex);
            auto it = std::find(g_toolbarWindows.begin(), g_toolbarWindows.end(), hWnd);
            if (it != g_toolbarWindows.end())
                g_toolbarWindows.erase(it);
        }

        DebugLog(L"[WndCompat][ToolBar] ToolBar_Detach: unmark hwnd=0x%p", hWnd);
    }

    void ToolBar_DetachAll()
    {
        std::vector<HWND> windows;
        {
            std::lock_guard lock(g_toolbarMutex);
            windows.swap(g_toolbarWindows);
        }

        for (HWND hWnd : windows)
        {
            GlassShared::GlassUnmarkWindow(hWnd);
            DebugLog(L"[WndCompat][ToolBar] ToolBar_DetachAll: unmark hwnd=0x%p", hWnd);
        }
    }

    void ToolBar_ApplyBackdropPolicyToTracked() noexcept
    {
        const bool allowed = WndCompat::IsGlassVisualsAllowed();

        std::vector<HWND> windows;
        {
            std::lock_guard lock(g_toolbarMutex);
            windows = g_toolbarWindows;
        }

        for (HWND hWnd : windows)
        {
            if (!hWnd || !IsWindow(hWnd))
                continue;

            RedrawWindow(hWnd, nullptr, nullptr,
                RDW_INVALIDATE | RDW_ERASE | RDW_FRAME);
        }

        DebugLog(DBG_IMPORTANT,
            L"[WndCompat][ToolBar] ToolBar_ApplyBackdropPolicy: allowed=%d count=%zu",
            allowed ? 1 : 0,
            windows.size());
    }
}
