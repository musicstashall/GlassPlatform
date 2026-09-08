#include "WndPch.h"
#include "Explorer/ExplorerIslandContainerSubclass.h"
#include <WndCompat.h>
#include <GlassMessages.h>
#include <GlassBackdropMark.h>
#include <RootPolicyQuery.h>
#include <vector>
#include <mutex>
#include <algorithm>
#include "Tracker/WinEventHandler.h"

namespace
{
    static std::vector<HWND> g_islandWindows;
    static std::mutex        g_islandWindowsMutex;

    void RestoreIslandDefaultAfterLiveOff(HWND hWnd) noexcept
    {
        if (!IsWindow(hWnd))
            return;
        // Live OFF: без FillRect(COLOR_WINDOW) — «хвост» белой кисти на шапке/кнопках; полный GDI — на shutdown.
        InvalidateRect(hWnd, nullptr, FALSE);
    }

    void RestoreIslandDefaultAfterDetach(HWND hWnd) noexcept
    {
        if (!IsWindow(hWnd))
            return;

        RECT rc{};
        if (GetClientRect(hWnd, &rc) && rc.right > rc.left && rc.bottom > rc.top)
        {
            if (HDC hdc = GetDC(hWnd))
            {
                HBRUSH br = GetSysColorBrush(COLOR_WINDOW);
                if (!br)
                    br = reinterpret_cast<HBRUSH>(static_cast<ULONG_PTR>(COLOR_WINDOW) + 1);
                FillRect(hdc, &rc, br);
                ReleaseDC(hWnd, hdc);
            }
        }

        InvalidateRect(hWnd, nullptr, TRUE);
        UpdateWindow(hWnd);
    }

    LRESULT CALLBACK IslandContainerSubclassProc(
        HWND hWnd,
        UINT uMsg,
        WPARAM wParam,
        LPARAM lParam,
        UINT_PTR uIdSubclass,
        DWORD_PTR dwRefData)
    {
        // Снятие субкласса по запросу из чужого потока (ExplorerIslandContainer_DetachAll -> SendMessageTimeout).
        if (uMsg == WndCompat::WM_GLASS_DETACH_EXPLORER_ISLAND)
        {
            RemoveWindowSubclass(hWnd, IslandContainerSubclassProc, uIdSubclass);
            RestoreIslandDefaultAfterDetach(hWnd);
            DebugLog(L"[WndCompat][Explorer] WM_GLASS_DETACH_EXPLORER_ISLAND: субкласс снят hwnd=0x%p", hWnd);
            WndCompat::WinEventHook_SignalDetachComplete();
            return 0;
        }

        if (WndCompat::IsShuttingDown())
            return DefSubclassProc(hWnd, uMsg, wParam, lParam);

        if (!GlassShared::GlassIsBackdropAllowedForHwnd(hWnd))
            return DefSubclassProc(hWnd, uMsg, wParam, lParam);

        switch (uMsg)
        {
        case WM_ERASEBKGND:
        {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            if (!hdc)
                return 0;

            RECT rcClip{};
            if (GetClipBox(hdc, &rcClip) <= 0 || rcClip.right <= rcClip.left || rcClip.bottom <= rcClip.top)
                GetClientRect(hWnd, &rcClip);

            PatBlt(
                hdc,
                rcClip.left,
                rcClip.top,
                rcClip.right - rcClip.left,
                rcClip.bottom - rcClip.top,
                BLACKNESS);
            return 1;
        }

        case WM_PAINT:
        {
            // Эталон ExplorerBlurMica: BeginPaint -> FillRect(BLACK_BRUSH) -> EndPaint.
            // Это подготавливает фон под WinUI-островом.
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            if (hdc)
            {
                RECT rcFill = ps.rcPaint;
                if (rcFill.right <= rcFill.left || rcFill.bottom <= rcFill.top)
                    GetClientRect(hWnd, &rcFill);

                FillRect(hdc, &rcFill, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
                EndPaint(hWnd, &ps);
            }
            return 0;
        }

        case WM_PRINT:
        case WM_PRINTCLIENT:
        {
            // Если передан hDC (wParam), выполняем заливку в нём.
            HDC hdc = (HDC)wParam;
            if (hdc)
            {
                RECT rc;
                GetClientRect(hWnd, &rc);
                FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
            }
            return 0;
        }

        case WM_NCDESTROY:
        {
            {
                std::lock_guard lock(g_islandWindowsMutex);
                auto it = std::find(g_islandWindows.begin(), g_islandWindows.end(), hWnd);
                if (it != g_islandWindows.end())
                    g_islandWindows.erase(it);
            }
            RemoveWindowSubclass(hWnd, IslandContainerSubclassProc, uIdSubclass);
            break;
        }
        }

        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }
}

namespace WndCompat
{
    void ExplorerIslandContainer_Attach(HWND hWnd)
    {
        if (!IsWindow(hWnd))
            return;

        if (WndCompat::IsShuttingDown())
            return;

        {
            std::lock_guard lock(g_islandWindowsMutex);
            if (std::find(g_islandWindows.begin(), g_islandWindows.end(), hWnd) != g_islandWindows.end())
                return;
            g_islandWindows.push_back(hWnd);
        }

        if (SetWindowSubclass(hWnd, IslandContainerSubclassProc, 0, 0))
        {
            DebugLog(L"[WndCompat][Explorer] ExplorerIslandContainer_Attach: субкласс установлен для hwnd=0x%p", hWnd);
            InvalidateRect(hWnd, nullptr, TRUE);
        }
        else
        {
            std::lock_guard lock(g_islandWindowsMutex);
            auto it = std::find(g_islandWindows.begin(), g_islandWindows.end(), hWnd);
            if (it != g_islandWindows.end())
                g_islandWindows.erase(it);
        }
    }

    void ExplorerIslandContainer_DetachAll()
    {
        std::vector<HWND> windows;
        {
            std::lock_guard lock(g_islandWindowsMutex);
            windows = std::move(g_islandWindows);
        }

        const DWORD curTid = GetCurrentThreadId();

        for (HWND hWnd : windows)
        {
            if (!IsWindow(hWnd))
                continue;

            const DWORD wndTid = GetWindowThreadProcessId(hWnd, nullptr);

            if (wndTid != curTid)
            {
                WinEventHook_RegisterPendingDetach();
                BOOL posted = PostMessageW(hWnd, WM_GLASS_DETACH_EXPLORER_ISLAND, 0, 0);
                if (!posted)
                {
                    WinEventHook_SignalDetachComplete();
                    DebugLog(L"[WndCompat][Explorer] ExplorerIslandContainer_DetachAll: PostMessageW FAILED for 0x%p", hWnd);
                }
                continue;
            }

            if (!RemoveWindowSubclass(hWnd, IslandContainerSubclassProc, 0))
            {
                DebugLog(L"[WndCompat][Explorer] ExplorerIslandContainer_DetachAll: RemoveWindowSubclass FAILED for 0x%p", hWnd);
            }
            else
            {
                DebugLog(L"[WndCompat][Explorer] ExplorerIslandContainer_DetachAll: detached 0x%p", hWnd);
                RestoreIslandDefaultAfterDetach(hWnd);
            }
        }
    }

    void ExplorerIslandContainer_ApplyBackdropPolicyToTracked() noexcept
    {
        const bool allowed = WndCompat::IsGlassVisualsAllowed();

        std::vector<HWND> windows;
        {
            std::lock_guard lock(g_islandWindowsMutex);
            windows = g_islandWindows;
        }

        for (HWND hWnd : windows)
        {
            if (!hWnd || !IsWindow(hWnd))
                continue;

            if (allowed)
            {
                InvalidateRect(hWnd, nullptr, TRUE);
            }
            else
            {
                RestoreIslandDefaultAfterLiveOff(hWnd);
            }
        }

        DebugLog(DBG_IMPORTANT,
            L"[WndCompat][Explorer] ExplorerIslandContainer_ApplyBackdropPolicy: allowed=%d count=%zu",
            allowed ? 1 : 0,
            windows.size());
    }
}
