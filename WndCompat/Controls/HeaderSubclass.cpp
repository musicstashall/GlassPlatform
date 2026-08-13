// Простая маркировка SysHeader32 для PaintDispatcher.
//
// Без SubclassProc — все хуки на SysHeader32 ставятся через HeaderHooks (GlassCore).
// Маркер GlassControlKind::Header используется в OnBeginPaintGlobal для определения
// набора хуков (themed vs unthemed).
//
// Образец: ListViewSubclass.cpp

#include "WndPch.h"
#include "HeaderSubclass.h"
#include <WndCompat.h>
#include <GlassControlKind.h>

#include <vector>
#include <mutex>
#include <algorithm>

namespace
{
    std::vector<HWND> g_headerWindows;
    std::mutex        g_headerMutex;
}

namespace WndCompat
{
    void Header_Attach(HWND hWnd)
    {
        if (!IsWindow(hWnd))
            return;

        using namespace GlassShared;
        if (GlassGetWindowKind(hWnd) == GlassControlKind::Header)
            return;

        GlassMarkWindow(hWnd, GlassControlKind::Header);
        WndCompat::TryRegisterTextGlowClientWithGlassCore(hWnd);

        {
            std::lock_guard lock(g_headerMutex);
            g_headerWindows.push_back(hWnd);
        }

        DebugLog(L"[WndCompat][Header] Header_Attach: hwnd=0x%p", hWnd);
    }

    void Header_Detach(HWND hWnd)
    {
        WndCompat::UnregisterTextGlowClientWithGlassCore(hWnd);
        GlassShared::GlassUnmarkWindow(hWnd);

        {
            std::lock_guard lock(g_headerMutex);
            auto it = std::find(g_headerWindows.begin(), g_headerWindows.end(), hWnd);
            if (it != g_headerWindows.end())
                g_headerWindows.erase(it);
        }

        DebugLog(L"[WndCompat][Header] Header_Detach: hwnd=0x%p", hWnd);
    }

    void Header_DetachAll()
    {
        std::vector<HWND> windows;
        {
            std::lock_guard lock(g_headerMutex);
            windows.swap(g_headerWindows);
        }

        for (HWND hWnd : windows)
        {
            if (!IsWindow(hWnd))
                continue;

            GlassShared::GlassUnmarkWindow(hWnd);
            DebugLog(L"[WndCompat][Header] Header_DetachAll: unmarked 0x%p", hWnd);
        }
    }
}
