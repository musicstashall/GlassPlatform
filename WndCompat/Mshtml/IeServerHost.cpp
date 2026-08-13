// InternetExplorer_Server: только маркер GlassMarkWindow для GlassCore (IAT StretchBlt).
// Субкласс и WM_APP не используются; очистка маркера в EVENT_OBJECT_DESTROY и DetachAll.
#include "WndPch.h"
#include "Mshtml/IeServerHost.h"

#include <GlassControlKind.h>
#include <Tracker/WinEventHandler.h>

#include <algorithm>
#include <mutex>
#include <vector>

namespace
{
    std::mutex        g_ieMutex;
    std::vector<HWND> g_ieWindows;
}

namespace WndCompat
{
    void IeServer_Attach(HWND hWnd)
    {
        if (WinEventHook_GetMode() != Shared::InjectionMode::Full)
            return;
        if (!hWnd)
            return;
        if (!IsWindow(hWnd))
            return;

        using namespace GlassShared;
        if (GlassGetWindowKind(hWnd) == GlassControlKind::InternetExplorer_Server)
            return;

        {
            std::lock_guard lock(g_ieMutex);
            if (std::find(g_ieWindows.begin(), g_ieWindows.end(), hWnd) != g_ieWindows.end())
                return;
            g_ieWindows.push_back(hWnd);
        }

        GlassMarkWindow(hWnd, GlassControlKind::InternetExplorer_Server);
    }

    void IeServer_Detach(HWND hWnd)
    {
        {
            std::lock_guard lock(g_ieMutex);
            const auto it = std::find(g_ieWindows.begin(), g_ieWindows.end(), hWnd);
            if (it != g_ieWindows.end())
                g_ieWindows.erase(it);
        }
        if (IsWindow(hWnd))
            GlassShared::GlassUnmarkWindow(hWnd);
    }

    void IeServer_DetachAll()
    {
        std::vector<HWND> windows;
        {
            std::lock_guard lock(g_ieMutex);
            windows.swap(g_ieWindows);
        }

        for (HWND hWnd : windows)
        {
            if (IsWindow(hWnd))
                GlassShared::GlassUnmarkWindow(hWnd);
        }
    }
}
