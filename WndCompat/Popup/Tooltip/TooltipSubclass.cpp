#include "WndPch.h"
#include "WndCompat.h"

#include "TooltipSubclass.h"
#include <GlassMessages.h>
#include <TooltipEventProvider.h>
#include <RegHelper.h>
#include <CommCtrl.h>

#include <algorithm>
#include <mutex>
#include <vector>

namespace
{
    std::mutex g_tooltipMutex;
    std::vector<HWND> g_tooltipWindows;

    [[nodiscard]] bool Tooltip_IsStandardAttachable(HWND hWnd) noexcept
    {
        WCHAR cn[256]{};
        const int nc = GetClassNameW(hWnd, cn, static_cast<int>(std::size(cn)));
        if (nc <= 0)
            return false;

        // MicrosoftWindowsTooltip (WinUI/оболочка) часто без классической visual theme на HWND.
        const bool isMsChrome = (_wcsicmp(cn, L"MicrosoftWindowsTooltip") == 0);
        if (!isMsChrome && !GetWindowTheme(hWnd))
            return false;

        const LONG st = static_cast<LONG>(GetWindowLongPtrW(hWnd, GWL_STYLE));
        if ((st & TTS_BALLOON) == TTS_BALLOON)
            return false;
        return true;
    }

    void Tooltip_RemoveFromList(HWND hWnd)
    {
        std::lock_guard lock(g_tooltipMutex);
        g_tooltipWindows.erase(
            std::remove(g_tooltipWindows.begin(), g_tooltipWindows.end(), hWnd),
            g_tooltipWindows.end());
    }

    [[nodiscard]] bool Tooltip_IsTracked(HWND hWnd)
    {
        std::lock_guard lock(g_tooltipMutex);
        return std::find(g_tooltipWindows.begin(), g_tooltipWindows.end(), hWnd) != g_tooltipWindows.end();
    }

    LRESULT CALLBACK TooltipSubclassProc(
        HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
        UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
    {
        // WM_GLASS_DETACH — до IsShuttingDown (как WM_GLASS_DETACH_* у Root/ComboLBox):
        // ExecuteShutdownCore выставляет g_shuttingDown=true до Tooltip_DetachAll.
        // WM_NCDESTROY — после IsShuttingDown (как RootSubclassProc): при обычном hide
        // тултипа субкласс снимается здесь; повторный аттач — по EVENT_OBJECT_SHOW в WinEvent.
        const WndCompat::Tooltip::TooltipEventProvider* const p = WndCompat::Tooltip::GetTooltipEventProvider();

        if (uMsg == WndCompat::WM_GLASS_DETACH_TOOLTIP)
        {
            WndCompat::UnregisterTextGlowClientWithGlassCore(hWnd);
            if (p && p->OnTooltipSubclassDetached)
                p->OnTooltipSubclassDetached(hWnd, static_cast<BOOL>(wParam != 0));
            Tooltip_RemoveFromList(hWnd);
            RemoveWindowSubclass(hWnd, TooltipSubclassProc, WndCompat::kTooltipSubclassId);
            return 0;
        }

        if (WndCompat::IsShuttingDown())
            return DefSubclassProc(hWnd, uMsg, wParam, lParam);

        if (uMsg == WM_NCDESTROY)
        {
            WndCompat::UnregisterTextGlowClientWithGlassCore(hWnd);
            if (p && p->OnTooltipSubclassDetached)
                p->OnTooltipSubclassDetached(hWnd, FALSE);
            Tooltip_RemoveFromList(hWnd);
            RemoveWindowSubclass(hWnd, TooltipSubclassProc, WndCompat::kTooltipSubclassId);
            return DefSubclassProc(hWnd, uMsg, wParam, lParam);
        }

        if (uMsg == WM_WINDOWPOSCHANGED)
        {
            const WINDOWPOS& wp = *reinterpret_cast<WINDOWPOS*>(lParam);
            if (wp.flags & SWP_SHOWWINDOW)
            {
                LRESULT const r = DefSubclassProc(hWnd, uMsg, wParam, lParam);

                const bool disabled = p && p->IsTooltipCustomizationDisabled && p->IsTooltipCustomizationDisabled();
                if (!disabled && Shared::RegHelper::Get<DWORD>({ L"Tooltip" }, L"NoSystemDropShadow", 0) != 0)
                {
                    HWND shadow = GetWindow(hWnd, GW_HWNDNEXT);
                    WCHAR cn[32]{};
                    if (shadow && GetClassNameW(shadow, cn, static_cast<int>(std::size(cn)))
                        && _wcsicmp(cn, L"SysShadow") == 0)
                    {
                        ShowWindow(shadow, SW_HIDE);
                    }
                }

                return r;
            }
        }

        if (uMsg == WM_PAINT || uMsg == WM_PRINTCLIENT)
        {
            bool broadcast = false;
            if (p && p->OnTooltipPaintSessionBegin && p->OnTooltipPaintSessionEnd)
            {
                broadcast = p->OnTooltipPaintSessionBegin(hWnd);
                LRESULT const r = DefSubclassProc(hWnd, uMsg, wParam, lParam);
                p->OnTooltipPaintSessionEnd(hWnd);
                if (broadcast)
                    WndCompat::Tooltip_BroadcastThemeChanged(hWnd);
                return r;
            }

            return DefSubclassProc(hWnd, uMsg, wParam, lParam);
        }

        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }
}

namespace WndCompat
{
    void Tooltip_BroadcastThemeChanged(HWND exceptHwnd)
    {
        std::vector<HWND> copy;
        {
            std::lock_guard lock(g_tooltipMutex);
            copy = g_tooltipWindows;
        }

        for (HWND h : copy)
        {
            if (!h || h == exceptHwnd || !IsWindow(h))
                continue;
            PostMessageW(h, WM_THEMECHANGED, 0, 0);
            PostMessageW(h, TTM_UPDATE, 0, 0);
        }
    }

    void Tooltip_AttachIfNeeded(HWND hWnd)
    {
        if (!IsWindow(hWnd) || WndCompat::IsShuttingDown())
            return;

        const WndCompat::Tooltip::TooltipEventProvider* const p = WndCompat::Tooltip::GetTooltipEventProvider();
        if (!p || !p->IsTooltipIatReady || !p->IsTooltipIatReady())
            return;
        if (p->IsTooltipCustomizationDisabled && p->IsTooltipCustomizationDisabled())
            return;
        if (!Tooltip_IsStandardAttachable(hWnd))
            return;

        {
            std::lock_guard lock(g_tooltipMutex);
            if (std::find(g_tooltipWindows.begin(), g_tooltipWindows.end(), hWnd) != g_tooltipWindows.end())
                return;
        }

        if (!SetWindowSubclass(hWnd, TooltipSubclassProc, kTooltipSubclassId, 0))
            return;

        {
            std::lock_guard lock(g_tooltipMutex);
            g_tooltipWindows.push_back(hWnd);
        }

        WndCompat::RegisterTextGlowPopupClientWithGlassCore(hWnd, 0);

        if (p->OnTooltipSubclassAttached)
            p->OnTooltipSubclassAttached(hWnd);
    }

    void Tooltip_AttachHintWindow(HWND hWnd)
    {
        if (!IsWindow(hWnd) || WndCompat::IsShuttingDown())
            return;

        const WndCompat::Tooltip::TooltipEventProvider* const p = WndCompat::Tooltip::GetTooltipEventProvider();
        if (!p || !p->IsTooltipIatReady || !p->IsTooltipIatReady())
            return;
        if (p->IsTooltipCustomizationDisabled && p->IsTooltipCustomizationDisabled())
            return;

        {
            std::lock_guard lock(g_tooltipMutex);
            if (std::find(g_tooltipWindows.begin(), g_tooltipWindows.end(), hWnd) != g_tooltipWindows.end())
                return;
        }

        if (!SetWindowSubclass(hWnd, TooltipSubclassProc, kTooltipSubclassId, 0))
            return;

        {
            std::lock_guard lock(g_tooltipMutex);
            g_tooltipWindows.push_back(hWnd);
        }

        WndCompat::RegisterTextGlowPopupClientWithGlassCore(hWnd, 0);

        if (p->OnTooltipSubclassAttached)
            p->OnTooltipSubclassAttached(hWnd);
    }

    void Tooltip_Detach(HWND hWnd)
    {
        if (!IsWindow(hWnd) || !Tooltip_IsTracked(hWnd))
            return;

        DWORD_PTR result = 0;
        const DWORD wndTid = GetWindowThreadProcessId(hWnd, nullptr);
        if (wndTid == GetCurrentThreadId())
        {
            SendMessageW(hWnd, WM_GLASS_DETACH_TOOLTIP, 0, 0);
        }
        else
        {
            SendMessageTimeoutW(
                hWnd, WM_GLASS_DETACH_TOOLTIP, 0, 0,
                SMTO_ABORTIFHUNG, 1000, &result);
        }
    }

    void Tooltip_DetachAll()
    {
        std::vector<HWND> copy;
        {
            std::lock_guard lock(g_tooltipMutex);
            copy = g_tooltipWindows;
        }

        const DWORD selfTid = GetCurrentThreadId();
        DebugLog(DBG_IMPORTANT,
            L"[WndCompat][Tooltip] Tooltip_DetachAll: отслеживалось %zu hwnd, поток=%u",
            copy.size(), static_cast<unsigned>(selfTid));
        for (HWND h : copy)
        {
            if (!IsWindow(h))
                continue;

            DWORD_PTR result = 0;
            if (GetWindowThreadProcessId(h, nullptr) == selfTid)
                SendMessageW(h, WM_GLASS_DETACH_TOOLTIP, 1, 0);
            else
                SendMessageTimeoutW(
                    h, WM_GLASS_DETACH_TOOLTIP, 1, 0,
                    SMTO_ABORTIFHUNG, 1000, &result);
        }

        {
            std::lock_guard lock(g_tooltipMutex);
            g_tooltipWindows.clear();
        }
    }
}
