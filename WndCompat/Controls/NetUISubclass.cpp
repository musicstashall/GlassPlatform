#include "WndPch.h"
#include "NetUISubclass.h"
#include "Tracker/WindowClassifier.h"
#include <GlassControlKind.h>
#include <DropDownEventProvider.h>
#include <algorithm>
#include <vector>

namespace
{
    static std::vector<HWND> g_netUiWindows;
    static std::vector<HWND> g_netUiToolWindows;
}

namespace WndCompat
{
    void NetUI_Attach(HWND hWnd)
    {
        if (!IsWindow(hWnd))
            return;

        using namespace GlassShared;
        if (GlassGetWindowKind(hWnd) == GlassControlKind::NetUI)
            return;

        GlassMarkWindow(hWnd, GlassControlKind::NetUI);

        if (std::find(g_netUiWindows.begin(), g_netUiWindows.end(), hWnd)
            == g_netUiWindows.end())
            g_netUiWindows.push_back(hWnd);
        
        DebugLog(L"[WndCompat][NetUI] NetUI_Attach: 0x%p (total=%zu)",
            hWnd, g_netUiWindows.size());

        // Оболочка «Net UI Tool Window» / Layered: CREATE на shell отдельно не обрабатываем — attach здесь.
        for (HWND cur = GetParent(hWnd); cur; cur = GetParent(cur))
        {
            if (WindowClassifier::IsNetUiRibbonToolWindow(cur))
            {
                DebugLog(L"[WndCompat][NetUI] NetUiToolWindow_Attach (parent chain): shell=0x%p netUi=0x%p",
                    cur,
                    hWnd);
                NetUiToolWindow_Attach(cur);
                break;
            }
        }
        const HWND root = GetAncestor(hWnd, GA_ROOT);
        if (root && WindowClassifier::IsNetUiRibbonToolWindow(root))
        {
            DebugLog(L"[WndCompat][NetUI] NetUiToolWindow_Attach (GA_ROOT): shell=0x%p netUi=0x%p", root, hWnd);
            NetUiToolWindow_Attach(root);
        }
    }

    void NetUI_Detach(HWND hWnd)
    {
        GlassShared::GlassUnmarkWindow(hWnd);

        auto it = std::find(g_netUiWindows.begin(), g_netUiWindows.end(), hWnd);
        if (it != g_netUiWindows.end())
            g_netUiWindows.erase(it);

        DebugLog(L"[WndCompat][NetUI] NetUI_Detach: 0x%p (total=%zu)",
            hWnd, g_netUiWindows.size());
    }

    void NetUI_DetachAll()
    {
        std::vector<HWND> windows;
        windows.swap(g_netUiWindows);

        for (HWND hWnd : windows)
        {
            if (!IsWindow(hWnd))
                continue;
            GlassShared::GlassUnmarkWindow(hWnd);
            DebugLog(L"[WndCompat][NetUI] NetUI_DetachAll: unmarked 0x%p", hWnd);
        }
    }

    void NetUiToolWindow_Attach(HWND hWnd)
    {
        if (!IsWindow(hWnd))
            return;

        using namespace GlassShared;
        if (GlassGetWindowKind(hWnd) == GlassControlKind::NetUiToolWindow)
            return;

        if (std::find(g_netUiToolWindows.begin(), g_netUiToolWindows.end(), hWnd)
            != g_netUiToolWindows.end())
            return;

        GlassMarkWindow(hWnd, GlassControlKind::NetUiToolWindow);
        g_netUiToolWindows.push_back(hWnd);

        DebugLog(L"[WndCompat][NetUI] NetUiToolWindow_Attach: 0x%p (total=%zu)",
            hWnd, g_netUiToolWindows.size());

        if (const auto* p = WndCompat::DropDown::GetDropDownEventProvider())
        {
            if (p->OnNetUiToolWindowHooksAttach)
                p->OnNetUiToolWindowHooksAttach(hWnd);
            if (p->OnNetUiToolWindowAttach)
                p->OnNetUiToolWindowAttach(hWnd);
        }
    }

    void NetUiToolWindow_Detach(HWND hWnd)
    {
        auto it = std::find(g_netUiToolWindows.begin(), g_netUiToolWindows.end(), hWnd);
        if (it != g_netUiToolWindows.end())
            g_netUiToolWindows.erase(it);

        if (const auto* p = WndCompat::DropDown::GetDropDownEventProvider())
        {
            if (p->OnNetUiToolWindowDetach)
                p->OnNetUiToolWindowDetach(hWnd);
        }
        GlassShared::GlassUnmarkWindow(hWnd);

        DebugLog(L"[WndCompat][NetUI] NetUiToolWindow_Detach: 0x%p (total=%zu)",
            hWnd, g_netUiToolWindows.size());
    }

    void NetUiToolWindow_DetachAll()
    {
        std::vector<HWND> windows;
        windows.swap(g_netUiToolWindows);

        for (HWND hWnd : windows)
        {
            if (!IsWindow(hWnd))
                continue;
            NetUiToolWindow_Detach(hWnd);
        }
    }
}
