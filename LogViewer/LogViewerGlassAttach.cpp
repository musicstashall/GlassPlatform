#include "SharedPch.h"

#include "LogViewerGlassAttach.h"
#include <GlassControlKind.h>
#include <Helpers/WinApiUtils.h>

#include <algorithm>
#include <mutex>
#include <vector>

#include <commctrl.h>
#include <uxtheme.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")

namespace
{
    const wchar_t kGlassTabUseThemeProp[] = L"Glass.TabUseTheme";

    std::vector<HWND> g_tabControlWindows;

    std::vector<HWND> g_buttonWindows;
    std::mutex        g_buttonMutex;
}

namespace LogViewerGlassAttach
{
    void AttachTabControl(HWND hWnd) noexcept
    {
        if (!hWnd || !::IsWindow(hWnd))
            return;

        using namespace GlassShared;
        if (GlassGetWindowKind(hWnd) == GlassControlKind::TabControl)
            return;

        GlassMarkWindow(hWnd, GlassControlKind::TabControl);
        g_tabControlWindows.push_back(hWnd);

        // Glass.TabUseTheme → TabHooks: при 0 сессия без DrawThemeBackground (только ExtTextOut + чёрная база).
        // IsThemed(hwnd) на свежем дочернем HWND иногда ложно false; дублируем проверкой OpenThemeData.
        bool useTheme = Shared::WinApiUtils::IsThemed(hWnd, L"TAB");
        if (!useTheme)
        {
            HTHEME th = ::OpenThemeData(hWnd, L"TAB");
            if (!th)
                th = ::OpenThemeData(nullptr, L"TAB");
            if (th)
            {
                ::CloseThemeData(th);
                useTheme = true;
            }
        }
        ::SetPropW(
            hWnd,
            kGlassTabUseThemeProp,
            reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(useTheme ? 1u : 0u)));
    }

    void RequestTabControlGlassRepaint(HWND hTabCtrl) noexcept
    {
        if (!hTabCtrl || !::IsWindow(hTabCtrl))
            return;
        ::RedrawWindow(hTabCtrl, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME);
    }

    void AttachRebar(HWND hWnd) noexcept
    {
        if (hWnd && ::IsWindow(hWnd))
            GlassShared::GlassMarkWindow(hWnd, GlassShared::GlassControlKind::Rebar);
    }

    void AttachToolbar(HWND hWnd) noexcept
    {
        if (hWnd && ::IsWindow(hWnd))
            GlassShared::GlassMarkWindow(hWnd, GlassShared::GlassControlKind::Toolbar);
    }

    void AttachEdit(HWND hWnd) noexcept
    {
        if (hWnd && ::IsWindow(hWnd))
            GlassShared::GlassMarkWindow(hWnd, GlassShared::GlassControlKind::Edit);
    }

    void AttachHeader(HWND hWnd) noexcept
    {
        if (hWnd && ::IsWindow(hWnd))
            GlassShared::GlassMarkWindow(hWnd, GlassShared::GlassControlKind::Header);
    }

    void AttachListView(HWND hWnd) noexcept
    {
        if (!hWnd || !::IsWindow(hWnd))
            return;
        GlassShared::GlassMarkWindow(hWnd, GlassShared::GlassControlKind::ListView);
        const HWND hHdr = ListView_GetHeader(hWnd);
        if (hHdr && ::IsWindow(hHdr))
            AttachHeader(hHdr);
    }

    void AttachStatic(HWND hWnd) noexcept
    {
        if (hWnd && ::IsWindow(hWnd))
            GlassShared::GlassMarkWindow(hWnd, GlassShared::GlassControlKind::Static);
    }

    void AttachStatusBar(HWND hWnd) noexcept
    {
        if (hWnd && ::IsWindow(hWnd))
            GlassShared::GlassMarkWindow(hWnd, GlassShared::GlassControlKind::StatusBar);
    }

    void AttachComboBox(HWND hWnd) noexcept
    {
        if (hWnd && ::IsWindow(hWnd))
            GlassShared::GlassMarkWindow(hWnd, GlassShared::GlassControlKind::ComboBox);
    }

    void AttachPreviewEdit(HWND hWnd) noexcept
    {
        if (hWnd && ::IsWindow(hWnd))
            GlassShared::GlassMarkWindow(hWnd, GlassShared::GlassControlKind::Edit);
    }

    void DetachTabControl(HWND hWnd) noexcept
    {
        if (!hWnd)
            return;
        ::RemovePropW(hWnd, kGlassTabUseThemeProp);
        GlassShared::GlassUnmarkWindow(hWnd);

        auto it = std::find(g_tabControlWindows.begin(), g_tabControlWindows.end(), hWnd);
        if (it != g_tabControlWindows.end())
            g_tabControlWindows.erase(it);
    }

    void AttachButton(HWND hWnd, GlassShared::GlassControlKind kind) noexcept
    {
        if (!hWnd || !::IsWindow(hWnd))
            return;

        using namespace GlassShared;
        if (kind != GlassControlKind::Button && kind != GlassControlKind::WinForms_Button)
            return;

        if (GlassGetWindowKind(hWnd) == GlassControlKind::Button
            || GlassGetWindowKind(hWnd) == GlassControlKind::WinForms_Button)
            return;

        GlassMarkWindow(hWnd, kind);

        {
            std::lock_guard lock(g_buttonMutex);
            g_buttonWindows.push_back(hWnd);
        }
    }

    void DetachButton(HWND hWnd) noexcept
    {
        if (::IsWindow(hWnd))
            GlassShared::GlassUnmarkWindow(hWnd);

        {
            std::lock_guard lock(g_buttonMutex);
            auto it = std::find(g_buttonWindows.begin(), g_buttonWindows.end(), hWnd);
            if (it != g_buttonWindows.end())
                g_buttonWindows.erase(it);
        }
    }

    void DetachChildrenInDialog(HWND hwndParent, HWND hPreview) noexcept
    {
        if (!hwndParent || !::IsWindow(hwndParent))
            return;
        for (HWND w = ::GetWindow(hwndParent, GW_CHILD); w; w = ::GetWindow(w, GW_HWNDNEXT))
        {
            wchar_t cls[64]{};
            if (::GetClassNameW(w, cls, static_cast<int>(std::size(cls))) == 0)
                continue;
            if (_wcsicmp(cls, WC_TABCONTROLW) == 0)
                DetachTabControl(w);
            else if (_wcsicmp(cls, L"Button") == 0)
                DetachButton(w);
        }
        if (hPreview && ::IsWindow(hPreview))
            GlassShared::GlassUnmarkWindow(hPreview);
    }

    void ShutdownCleanup() noexcept
    {
        {
            std::lock_guard lock(g_buttonMutex);
            for (HWND hWnd : g_buttonWindows)
            {
                if (hWnd && ::IsWindow(hWnd))
                    GlassShared::GlassUnmarkWindow(hWnd);
            }
            g_buttonWindows.clear();
        }

        auto windows = g_tabControlWindows;
        g_tabControlWindows.clear();

        for (HWND hWnd : windows)
        {
            if (!hWnd || !::IsWindow(hWnd))
                continue;
            ::RemovePropW(hWnd, kGlassTabUseThemeProp);
            GlassShared::GlassUnmarkWindow(hWnd);
            ::InvalidateRect(hWnd, nullptr, TRUE);
        }
    }
}
