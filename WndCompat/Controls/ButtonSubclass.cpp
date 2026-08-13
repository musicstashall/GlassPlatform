#include "WndPch.h"
#include "ButtonSubclass.h"
#include <WndCompat.h>
#include <GlassControlKind.h>
#include <GlassBackdropMark.h>
#include "Tracker/SubclassDiagPublish.h"
#include "Tracker/WinEventHandler.h"
#include <RootPolicyQuery.h>

#include <vector>
#include <mutex>
#include <algorithm>

namespace
{
    std::vector<HWND> g_buttonWindows;
    std::mutex        g_buttonMutex;
}

namespace WndCompat
{
    void Button_Attach(HWND hWnd, GlassShared::GlassControlKind kind)
    {
        if (!IsWindow(hWnd))
            return;

        using namespace GlassShared;
        if (kind != GlassControlKind::Button && kind != GlassControlKind::WinForms_Button)
        {
            DebugLog(DBG_WARNING, L"[WndCompat][Button] Button_Attach: неверный kind=%u hwnd=0x%p",
                static_cast<unsigned>(kind), hWnd);
            return;
        }

        if (GlassGetWindowKind(hWnd) == GlassControlKind::Button
         || GlassGetWindowKind(hWnd) == GlassControlKind::WinForms_Button)
        {
            std::lock_guard lock(g_buttonMutex);
            if (std::find(g_buttonWindows.begin(), g_buttonWindows.end(), hWnd) != g_buttonWindows.end())
                return;
        }

        GlassMarkWindow(hWnd, kind);
        std::size_t n = 0;
        {
            std::lock_guard lock(g_buttonMutex);
            g_buttonWindows.push_back(hWnd);
            n = g_buttonWindows.size();
        }
        SubclassDiag::PublishKindCount(kind, n);

        if (GlassShared::GlassIsBackdropAllowedForHwnd(hWnd))
            WndCompat::RegisterThemedChromeClientWithGlassCore(hWnd);

        WndCompat::TryRegisterTextGlowClientWithGlassCore(hWnd);

        const wchar_t* const kindName =
            (kind == GlassControlKind::WinForms_Button) ? L"WinForms_Button" : L"Button";
        DebugLog(L"[WndCompat][Button] Button_Attach: kind=%s hwnd=0x%p", kindName, hWnd);
    }

    void Button_Detach(HWND hWnd)
    {
        WndCompat::UnregisterTextGlowClientWithGlassCore(hWnd);
        WndCompat::UnregisterThemedChromeClientWithGlassCore(hWnd);
        if (IsWindow(hWnd))
            GlassShared::GlassUnmarkWindow(hWnd);

        {
            std::lock_guard lock(g_buttonMutex);
            auto it = std::find(g_buttonWindows.begin(), g_buttonWindows.end(), hWnd);
            if (it != g_buttonWindows.end())
                g_buttonWindows.erase(it);
            const auto n = g_buttonWindows.size();
            SubclassDiag::PublishKindCount(GlassShared::GlassControlKind::Button, n);
        }

        DebugLog(L"[WndCompat][Button] Button_Detach: hwnd=0x%p", hWnd);
    }

    bool Button_IsTracked(HWND hWnd) noexcept
    {
        std::lock_guard lock(g_buttonMutex);
        return std::find(g_buttonWindows.begin(), g_buttonWindows.end(), hWnd)
            != g_buttonWindows.end();
    }

    void Button_DetachAll()
    {
        std::vector<HWND> windows;
        {
            std::lock_guard lock(g_buttonMutex);
            windows.swap(g_buttonWindows);
        }
        SubclassDiag::PublishKindCount(GlassShared::GlassControlKind::Button, 0);
        SubclassDiag::PublishKindCount(GlassShared::GlassControlKind::WinForms_Button, 0);

        for (HWND hWnd : windows)
        {
            if (!IsWindow(hWnd))
                continue;

            WndCompat::UnregisterThemedChromeClientWithGlassCore(hWnd);
            GlassShared::GlassUnmarkWindow(hWnd);
            DebugLog(L"[WndCompat][Button] Button_DetachAll: unmarked 0x%p", hWnd);
        }
    }

    void Button_ApplyBackdropPolicyToTracked() noexcept
    {
        const bool allowed = WndCompat::IsGlassVisualsAllowed();

        std::vector<HWND> windows;
        {
            std::lock_guard lock(g_buttonMutex);
            windows = g_buttonWindows;
        }

        for (HWND hWnd : windows)
        {
            if (!hWnd || !IsWindow(hWnd))
                continue;

            using namespace GlassShared;
            const auto kind = GlassGetWindowKind(hWnd);
            if (kind != GlassControlKind::Button && kind != GlassControlKind::WinForms_Button)
                continue;

            if (allowed)
            {
                WndCompat::RegisterThemedChromeClientWithGlassCore(hWnd);
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            else
            {
                WndCompat::UnregisterThemedChromeClientWithGlassCore(hWnd);
            }
        }
    }
}
