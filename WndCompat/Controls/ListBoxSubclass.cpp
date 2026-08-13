// Маркировка ListBox / WinForms .LISTBOX для PaintDispatcher (GlassCore ListBoxHooks).
#include "WndPch.h"
#include "ListBoxSubclass.h"
#include <WndCompat.h>
#include <GlassControlKind.h>
#include <algorithm>
#include <mutex>
#include <vector>

namespace
{
    std::vector<HWND> g_listBoxWindows;
    std::mutex        g_listBoxMutex;
}

namespace WndCompat
{
    void ListBox_Attach(HWND hWnd, GlassShared::GlassControlKind kind)
    {
        if (!IsWindow(hWnd))
            return;
        if (kind != GlassShared::GlassControlKind::ListBox
            && kind != GlassShared::GlassControlKind::WinForms_ListBox)
            return;

        using namespace GlassShared;
        if (GlassGetWindowKind(hWnd) == kind)
            return;

        GlassMarkWindow(hWnd, kind);
        WndCompat::TryRegisterTextGlowClientWithGlassCore(hWnd);
        {
            std::lock_guard lock(g_listBoxMutex);
            g_listBoxWindows.push_back(hWnd);
        }
        DebugLog(L"[WndCompat][ListBox] ListBox_Attach: hwnd=0x%p kind=%u", hWnd,
            static_cast<unsigned>(static_cast<ULONG_PTR>(kind)));
    }

    void ListBox_Detach(HWND hWnd)
    {
        WndCompat::UnregisterTextGlowClientWithGlassCore(hWnd);
        GlassShared::GlassUnmarkWindow(hWnd);
        {
            std::lock_guard lock(g_listBoxMutex);
            const auto it = std::find(g_listBoxWindows.begin(), g_listBoxWindows.end(), hWnd);
            if (it != g_listBoxWindows.end())
                g_listBoxWindows.erase(it);
        }
        DebugLog(L"[WndCompat][ListBox] ListBox_Detach: hwnd=0x%p", hWnd);
    }

    void ListBox_DetachAll()
    {
        std::vector<HWND> copy;
        {
            std::lock_guard lock(g_listBoxMutex);
            copy = g_listBoxWindows;
            g_listBoxWindows.clear();
        }
        for (HWND h : copy)
            GlassShared::GlassUnmarkWindow(h);
    }
}
