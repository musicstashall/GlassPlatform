#include "WndPch.h"
#include "StatusBarSubclass.h"
#include <WndCompat.h>
#include <GlassControlKind.h>
#include <algorithm>
#include <mutex>
#include <vector>

namespace
{
    std::vector<HWND> g_windows;
    std::mutex        g_mutex;
}

namespace WndCompat
{
    void StatusBar_Attach(HWND hWnd)
    {
        if (!IsWindow(hWnd))
            return;
        using namespace GlassShared;
        if (GlassGetWindowKind(hWnd) == GlassControlKind::StatusBar)
            return;
        GlassMarkWindow(hWnd, GlassControlKind::StatusBar);
        WndCompat::TryRegisterTextGlowClientWithGlassCore(hWnd);
        {
            std::lock_guard lock(g_mutex);
            g_windows.push_back(hWnd);
        }
        DebugLog(L"[WndCompat][StatusBar] StatusBar_Attach: hwnd=0x%p", hWnd);
    }

    void StatusBar_Detach(HWND hWnd)
    {
        WndCompat::UnregisterTextGlowClientWithGlassCore(hWnd);
        GlassShared::GlassUnmarkWindow(hWnd);
        {
            std::lock_guard lock(g_mutex);
            const auto it = std::find(g_windows.begin(), g_windows.end(), hWnd);
            if (it != g_windows.end())
                g_windows.erase(it);
        }
    }

    void StatusBar_DetachAll()
    {
        std::vector<HWND> copy;
        {
            std::lock_guard lock(g_mutex);
            copy = g_windows;
            g_windows.clear();
        }
        for (HWND h : copy)
            GlassShared::GlassUnmarkWindow(h);
    }
}
