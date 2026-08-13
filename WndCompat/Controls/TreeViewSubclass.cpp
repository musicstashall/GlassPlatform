#include "WndPch.h"
#include "TreeViewSubclass.h"
#include <WndCompat.h>
#include <GlassControlKind.h>
#include <GlassBackdropMark.h>
#include <RootPolicyQuery.h>
#include <RootEventProvider.h>
#include <CommCtrl.h>
#include "Tracker/WinEventHandler.h"

#include <vector>
#include <mutex>
#include <unordered_map>
#include <algorithm>

namespace
{
    struct TreeViewClassBrushEntry
    {
        HBRUSH   hOrigBrush;
        LONG_PTR origClassStyle;
        int      refCount;
    };

    std::vector<HWND> g_treeViewWindows;
    std::mutex        g_treeViewMutex;
    std::unordered_map<ATOM, TreeViewClassBrushEntry> g_treeViewClassBrushes;

    void ApplyTreeViewClassBlackBrush(HWND hWnd) noexcept
    {
        const ATOM classAtom = static_cast<ATOM>(GetClassLongPtr(hWnd, GCW_ATOM));
        if (!classAtom)
            return;

        const LONG_PTR prevClassStyle = GetClassLongPtr(hWnd, GCL_STYLE);
        const auto hPrevBrush = reinterpret_cast<HBRUSH>(
            SetClassLongPtr(hWnd, GCLP_HBRBACKGROUND, reinterpret_cast<LONG_PTR>(GetStockObject(BLACK_BRUSH))));
        SetClassLongPtr(hWnd, GCL_STYLE, prevClassStyle | CS_HREDRAW | CS_VREDRAW);

        std::lock_guard lock(g_treeViewMutex);
        const auto [it, inserted] = g_treeViewClassBrushes.try_emplace(
            classAtom,
            TreeViewClassBrushEntry{ hPrevBrush, prevClassStyle, 1 });
        if (!inserted)
            ++it->second.refCount;
    }

    void RestoreTreeViewClassBrushForHwnd(HWND hWnd) noexcept
    {
        const ATOM classAtom = static_cast<ATOM>(GetClassLongPtr(hWnd, GCW_ATOM));
        if (!classAtom)
            return;

        std::lock_guard lock(g_treeViewMutex);
        const auto brushIt = g_treeViewClassBrushes.find(classAtom);
        if (brushIt == g_treeViewClassBrushes.end())
            return;

        SetClassLongPtr(hWnd, GCLP_HBRBACKGROUND, reinterpret_cast<LONG_PTR>(brushIt->second.hOrigBrush));
        SetClassLongPtr(hWnd, GCL_STYLE, brushIt->second.origClassStyle);
        if (--brushIt->second.refCount <= 0)
            g_treeViewClassBrushes.erase(brushIt);
    }

    void ApplyTreeViewGlassVisuals(HWND hWnd) noexcept
    {
        ApplyTreeViewClassBlackBrush(hWnd);
        TreeView_SetBkColor(hWnd, 0);
    }

    void RestoreTreeViewGlassVisuals(HWND hWnd) noexcept
    {
        TreeView_SetBkColor(hWnd, CLR_NONE);
        RestoreTreeViewClassBrushForHwnd(hWnd);
    }

    void RedrawTreeViewClient(HWND hWnd) noexcept
    {
        RedrawWindow(hWnd, nullptr, nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }
}

namespace WndCompat
{
    void TreeView_Attach(HWND hWnd)
    {
        if (!IsWindow(hWnd))
            return;

        using namespace GlassShared;

        bool tracked = false;
        {
            std::lock_guard lock(g_treeViewMutex);
            tracked = std::find(g_treeViewWindows.begin(), g_treeViewWindows.end(), hWnd)
                != g_treeViewWindows.end();
        }
        if (GlassGetWindowKind(hWnd) == GlassControlKind::TreeView && tracked)
            return;

        if (GlassGetWindowKind(hWnd) != GlassControlKind::TreeView)
            GlassMarkWindow(hWnd, GlassControlKind::TreeView);

        if (!tracked)
        {
            std::lock_guard lock(g_treeViewMutex);
            if (std::find(g_treeViewWindows.begin(), g_treeViewWindows.end(), hWnd)
                == g_treeViewWindows.end())
            {
                g_treeViewWindows.push_back(hWnd);
            }
        }

        if (GlassShared::GlassIsBackdropAllowedForHwnd(hWnd))
            ApplyTreeViewGlassVisuals(hWnd);

        WndCompat::TryRegisterTextGlowClientWithGlassCore(hWnd);

        DebugLog(L"[WndCompat][TreeView] TreeView_Attach: hwnd=0x%p", hWnd);
    }

    void TreeView_Detach(HWND hWnd)
    {
        WndCompat::UnregisterTextGlowClientWithGlassCore(hWnd);
        GlassShared::GlassUnmarkWindow(hWnd);

        {
            std::lock_guard lock(g_treeViewMutex);
            auto it = std::find(g_treeViewWindows.begin(), g_treeViewWindows.end(), hWnd);
            if (it != g_treeViewWindows.end())
                g_treeViewWindows.erase(it);
        }

        DebugLog(L"[WndCompat][TreeView] TreeView_Detach: hwnd=0x%p", hWnd);
    }

    bool TreeView_IsTracked(HWND hWnd) noexcept
    {
        std::lock_guard lock(g_treeViewMutex);
        return std::find(g_treeViewWindows.begin(), g_treeViewWindows.end(), hWnd)
            != g_treeViewWindows.end();
    }

    void TreeView_DetachAll()
    {
        std::vector<HWND> windows;
        {
            std::lock_guard lock(g_treeViewMutex);
            windows.swap(g_treeViewWindows);
            g_treeViewClassBrushes.clear();
        }

        for (HWND hWnd : windows)
        {
            if (!IsWindow(hWnd))
                continue;

            TreeView_SetBkColor(hWnd, CLR_NONE);
            GlassShared::GlassUnmarkWindow(hWnd);
            DebugLog(L"[WndCompat][TreeView] TreeView_DetachAll: restored 0x%p", hWnd);
        }
    }

    void TreeView_ApplyBackdropPolicyToTracked() noexcept
    {
        const bool allowed = WndCompat::IsGlassVisualsAllowed();

        std::vector<HWND> windows;
        {
            std::lock_guard lock(g_treeViewMutex);
            windows = g_treeViewWindows;
        }

        for (HWND hWnd : windows)
        {
            if (!hWnd || !IsWindow(hWnd))
                continue;
            if (GlassShared::GlassGetWindowKind(hWnd) != GlassShared::GlassControlKind::TreeView)
                continue;

            if (allowed)
                ApplyTreeViewGlassVisuals(hWnd);
            else
                RestoreTreeViewGlassVisuals(hWnd);

            RedrawTreeViewClient(hWnd);
        }
    }
}
