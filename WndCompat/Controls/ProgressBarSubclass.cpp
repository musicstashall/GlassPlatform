#include "WndPch.h"
#include "ProgressBarSubclass.h"
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
    void ProgressBar_Attach(HWND hWnd)
    {
        if (!IsWindow(hWnd))
            return;
        using namespace GlassShared;
        if (GlassGetWindowKind(hWnd) == GlassControlKind::ProgressBar)
            return;
        GlassMarkWindow(hWnd, GlassControlKind::ProgressBar);
        {
            std::lock_guard lock(g_mutex);
            g_windows.push_back(hWnd);
        }
        DebugLog(L"[WndCompat][ProgressBar] ProgressBar_Attach: hwnd=0x%p", hWnd);
    }

    void ProgressBar_Detach(HWND hWnd)
    {
        GlassShared::GlassUnmarkWindow(hWnd);
        {
            std::lock_guard lock(g_mutex);
            const auto it = std::find(g_windows.begin(), g_windows.end(), hWnd);
            if (it != g_windows.end())
                g_windows.erase(it);
        }
    }

    void ProgressBar_DetachAll()
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
