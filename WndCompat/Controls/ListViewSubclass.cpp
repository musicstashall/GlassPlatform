#include "WndPch.h"
#include "ListViewSubclass.h"
#include <WndCompat.h>
#include <GlassControlKind.h>
#include <GlassBackdropMark.h>
#include <RootPolicyQuery.h>
#include <RootEventProvider.h>
#include "Tracker/WinEventHandler.h"

#include <vector>
#include <mutex>
#include <algorithm>

namespace
{
    std::vector<HWND> g_listViewWindows;
    std::mutex        g_listViewMutex;

    // Десктопный ListView (иконки рабочего стола) — родитель SHELLDLL_DefView.
    // Его рендеринг идёт через DComp/DWM и несовместим с нашими PatBlt(BLACKNESS) хуками.
    bool IsDesktopListView(HWND hWnd) noexcept
    {
        HWND parent = GetParent(hWnd);
        if (!parent) return false;

        WCHAR cls[64]{};
        GetClassNameW(parent, cls, _countof(cls));
        return _wcsicmp(cls, L"SHELLDLL_DefView") == 0;
    }

    // WinForms ListView (.SysListView32) — имя класса начинается с точки.
    bool IsWinFormsListView(HWND hWnd) noexcept
    {
        WCHAR cls[64]{};
        GetClassNameW(hWnd, cls, _countof(cls));
        return cls[0] == L'.';
    }

    void ApplyListViewGlassVisuals(HWND hWnd, GlassShared::GlassControlKind kind) noexcept
    {
        if (kind == GlassShared::GlassControlKind::ListView)
        {
            ListView_SetBkColor(hWnd, 0);
            ListView_SetTextBkColor(hWnd, 0);
        }
    }

    void RestoreListViewGlassVisuals(HWND hWnd, GlassShared::GlassControlKind kind) noexcept
    {
        if (kind == GlassShared::GlassControlKind::ListView)
        {
            ListView_SetBkColor(hWnd, CLR_NONE);
            ListView_SetTextBkColor(hWnd, CLR_NONE);
        }
    }

    void RedrawListViewClient(HWND hWnd) noexcept
    {
        RedrawWindow(hWnd, nullptr, nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }
}

namespace WndCompat
{
    void ListView_Attach(HWND hWnd)
    {
        if (!IsWindow(hWnd))
            return;

        if (IsDesktopListView(hWnd))
        {
            DebugLog(L"[WndCompat][ListView] ListView_Attach: SKIP desktop ListView hwnd=0x%p", hWnd);
            return;
        }

        using namespace GlassShared;
        const auto existingKind = GlassGetWindowKind(hWnd);
        bool tracked = false;
        {
            std::lock_guard lock(g_listViewMutex);
            tracked = std::find(g_listViewWindows.begin(), g_listViewWindows.end(), hWnd)
                != g_listViewWindows.end();
        }
        if ((existingKind == GlassControlKind::ListView ||
                existingKind == GlassControlKind::WinForms_ListView) && tracked)
            return;

        // .SysListView32 маркируется как WinForms_ListView — для него другой набор хуков
        const auto kind = IsWinFormsListView(hWnd)
            ? GlassControlKind::WinForms_ListView
            : GlassControlKind::ListView;
        if (existingKind != kind)
            GlassMarkWindow(hWnd, kind);

        if (!tracked)
        {
            std::lock_guard lock(g_listViewMutex);
            if (std::find(g_listViewWindows.begin(), g_listViewWindows.end(), hWnd)
                == g_listViewWindows.end())
            {
                g_listViewWindows.push_back(hWnd);
            }
        }

        if (GlassShared::GlassIsBackdropAllowedForHwnd(hWnd))
            ApplyListViewGlassVisuals(hWnd, kind);

        WndCompat::TryRegisterTextGlowClientWithGlassCore(hWnd);

        DebugLog(L"[WndCompat][ListView] ListView_Attach: hwnd=0x%p", hWnd);
    }

    void ListView_Detach(HWND hWnd)
    {
        WndCompat::UnregisterTextGlowClientWithGlassCore(hWnd);
        GlassShared::GlassUnmarkWindow(hWnd);

        {
            std::lock_guard lock(g_listViewMutex);
            auto it = std::find(g_listViewWindows.begin(), g_listViewWindows.end(), hWnd);
            if (it != g_listViewWindows.end())
                g_listViewWindows.erase(it);
        }

        DebugLog(L"[WndCompat][ListView] ListView_Detach: hwnd=0x%p", hWnd);
    }

    bool ListView_IsTracked(HWND hWnd) noexcept
    {
        std::lock_guard lock(g_listViewMutex);
        return std::find(g_listViewWindows.begin(), g_listViewWindows.end(), hWnd)
            != g_listViewWindows.end();
    }

    void ListView_DetachAll()
    {
        std::vector<HWND> windows;
        {
            std::lock_guard lock(g_listViewMutex);
            windows.swap(g_listViewWindows);
        }

        for (HWND hWnd : windows)
        {
            if (!IsWindow(hWnd))
                continue;

            const auto kind = GlassShared::GlassGetWindowKind(hWnd);
            RestoreListViewGlassVisuals(hWnd, kind);
            GlassShared::GlassUnmarkWindow(hWnd);
            DebugLog(L"[WndCompat][ListView] ListView_DetachAll: restored 0x%p", hWnd);
        }
    }

    void ListView_ApplyBackdropPolicyToTracked() noexcept
    {
        const bool allowed = WndCompat::IsGlassVisualsAllowed();

        std::vector<HWND> windows;
        {
            std::lock_guard lock(g_listViewMutex);
            windows = g_listViewWindows;
        }

        for (HWND hWnd : windows)
        {
            if (!hWnd || !IsWindow(hWnd))
                continue;

            const auto kind = GlassShared::GlassGetWindowKind(hWnd);
            if (kind != GlassShared::GlassControlKind::ListView
                && kind != GlassShared::GlassControlKind::WinForms_ListView)
                continue;

            if (allowed)
                ApplyListViewGlassVisuals(hWnd, kind);
            else
                RestoreListViewGlassVisuals(hWnd, kind);

            RedrawListViewClient(hWnd);
        }
    }
}
