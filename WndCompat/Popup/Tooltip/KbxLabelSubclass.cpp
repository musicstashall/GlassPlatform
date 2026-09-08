#include "WndPch.h"
#include "WndCompat.h"

#include "KbxLabelSubclass.h"
#include <GlassMessages.h>
#include <TooltipEventProvider.h>
#include <Tracker/WinEventHandler.h>

#include <algorithm>
#include <mutex>
#include <vector>

namespace
{
    std::mutex g_kbxMutex;
    std::vector<HWND> g_kbxWindows;

    void KbxLabel_RemoveFromList(HWND hWnd)
    {
        std::lock_guard lock(g_kbxMutex);
        g_kbxWindows.erase(
            std::remove(g_kbxWindows.begin(), g_kbxWindows.end(), hWnd),
            g_kbxWindows.end());
    }

    [[nodiscard]] bool KbxLabel_IsTracked(HWND hWnd)
    {
        std::lock_guard lock(g_kbxMutex);
        return std::find(g_kbxWindows.begin(), g_kbxWindows.end(), hWnd) != g_kbxWindows.end();
    }

    LRESULT CALLBACK KbxLabelSubclassProc(
        HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
        UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
    {
        const WndCompat::Tooltip::TooltipEventProvider* const p = WndCompat::Tooltip::GetTooltipEventProvider();

        if (uMsg == WndCompat::WM_GLASS_DETACH_KBXLABEL)
        {
            WndCompat::UnregisterTextGlowClientWithGlassCore(hWnd);
            if (p && p->OnKbxLabelSubclassDetached)
                p->OnKbxLabelSubclassDetached(hWnd, static_cast<BOOL>(wParam != 0));
            KbxLabel_RemoveFromList(hWnd);
            RemoveWindowSubclass(hWnd, KbxLabelSubclassProc, WndCompat::kKbxLabelSubclassId);
            WndCompat::WinEventHook_SignalDetachComplete();
            return 0;
        }

        if (WndCompat::IsShuttingDown())
            return DefSubclassProc(hWnd, uMsg, wParam, lParam);

        if (uMsg == WM_NCDESTROY)
        {
            WndCompat::UnregisterTextGlowClientWithGlassCore(hWnd);
            if (p && p->OnKbxLabelSubclassDetached)
                p->OnKbxLabelSubclassDetached(hWnd, FALSE);
            KbxLabel_RemoveFromList(hWnd);
            RemoveWindowSubclass(hWnd, KbxLabelSubclassProc, WndCompat::kKbxLabelSubclassId);
            return DefSubclassProc(hWnd, uMsg, wParam, lParam);
        }

        if (uMsg == WM_PAINT)
        {
            if (p && p->OnKbxLabelPaintSessionBegin && p->OnKbxLabelPaintSessionEnd)
            {
                p->OnKbxLabelPaintSessionBegin(hWnd);
                LRESULT const r = DefSubclassProc(hWnd, uMsg, wParam, lParam);
                p->OnKbxLabelPaintSessionEnd(hWnd);
                return r;
            }
            return DefSubclassProc(hWnd, uMsg, wParam, lParam);
        }

        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }
}

namespace WndCompat
{
    void KbxLabel_Attach(HWND hWnd)
    {
        if (!IsWindow(hWnd) || WndCompat::IsShuttingDown())
            return;

        const WndCompat::Tooltip::TooltipEventProvider* const p = WndCompat::Tooltip::GetTooltipEventProvider();
        if (!p || !p->IsTooltipIatReady || !p->IsTooltipIatReady())
            return;
        if (p->IsTooltipCustomizationDisabled && p->IsTooltipCustomizationDisabled())
            return;

        {
            std::lock_guard lock(g_kbxMutex);
            if (std::find(g_kbxWindows.begin(), g_kbxWindows.end(), hWnd) != g_kbxWindows.end())
                return;
        }

        if (!SetWindowSubclass(hWnd, KbxLabelSubclassProc, kKbxLabelSubclassId, 0))
            return;

        {
            std::lock_guard lock(g_kbxMutex);
            g_kbxWindows.push_back(hWnd);
        }

        WndCompat::RegisterTextGlowPopupClientWithGlassCore(hWnd, 0);

        if (p->OnKbxLabelSubclassAttached)
            p->OnKbxLabelSubclassAttached(hWnd);
    }

    void KbxLabel_Detach(HWND hWnd)
    {
        if (!IsWindow(hWnd) || !KbxLabel_IsTracked(hWnd))
            return;

        DWORD_PTR result = 0;
        if (GetWindowThreadProcessId(hWnd, nullptr) == GetCurrentThreadId())
            SendMessageW(hWnd, WM_GLASS_DETACH_KBXLABEL, 0, 0);
        else
            SendMessageTimeoutW(
                hWnd, WM_GLASS_DETACH_KBXLABEL, 0, 0,
                SMTO_ABORTIFHUNG, 1000, &result);
    }

    void KbxLabel_DetachAll()
    {
        std::vector<HWND> copy;
        {
            std::lock_guard lock(g_kbxMutex);
            copy = g_kbxWindows;
        }

        const DWORD selfTid = GetCurrentThreadId();
        DebugLog(DBG_IMPORTANT,
            L"[WndCompat][KbxLabel] KbxLabel_DetachAll: отслеживалось %zu hwnd, поток=%u",
            copy.size(), static_cast<unsigned>(selfTid));
        for (HWND h : copy)
        {
            if (!IsWindow(h))
                continue;

            if (GetWindowThreadProcessId(h, nullptr) == selfTid)
                RemoveWindowSubclass(h, KbxLabelSubclassProc, kKbxLabelSubclassId);
            else
            {
                WinEventHook_RegisterPendingDetach();
                BOOL posted = PostMessageW(h, WM_GLASS_DETACH_KBXLABEL, 0, 0);
                if (!posted)
                {
                    WinEventHook_SignalDetachComplete();
                    DebugLog(L"[WndCompat][KbxLabel] KbxLabel_DetachAll: PostMessageW FAILED for 0x%p", h);
                }
            }
        }

        {
            std::lock_guard lock(g_kbxMutex);
            g_kbxWindows.clear();
        }
    }
}
