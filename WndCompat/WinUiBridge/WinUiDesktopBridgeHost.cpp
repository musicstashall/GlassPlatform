// Microsoft.UI.Content.DesktopChildSiteBridge: учёт HWND + ранний триггер XAML TAP (ExplorerCompat).
// Субкласс не используется.
#include "WndPch.h"
#include "WinUiBridge/WinUiDesktopBridgeHost.h"

#include <DebugLog.h>
#include <ExplorerCompatHostPolicy.h>

#include <algorithm>
#include <mutex>
#include <vector>

namespace
{
    using ExplorerCompatNotifyDesktopBridgeFn = void(WINAPI*)(HWND);

    constexpr unsigned kMaxPendingBridges = 32;

    std::mutex                        g_bridgeMutex;
    std::vector<HWND>                 g_bridgeWindows;
    std::vector<HWND>                 g_pendingBridgeHwnds;
    ExplorerCompatNotifyDesktopBridgeFn g_pfnNotifyBridge = nullptr;
    bool                              g_loggedMissingExplorerCompat = false;

    [[nodiscard]] ExplorerCompatNotifyDesktopBridgeFn ResolveNotifyProc() noexcept
    {
        if (g_pfnNotifyBridge)
            return g_pfnNotifyBridge;

        HMODULE hExplorerCompat = GetModuleHandleW(L"ExplorerCompat.dll");
        if (!hExplorerCompat)
        {
            if (!g_loggedMissingExplorerCompat)
            {
                g_loggedMissingExplorerCompat = true;
                DebugLog(
                    DBG_NORMAL,
                    L"[WndCompat][WinUiBridge] ExplorerCompat.dll ещё не в процессе — Notify отложен (очередь до Flush)");
            }
            return nullptr;
        }

        auto* p = reinterpret_cast<ExplorerCompatNotifyDesktopBridgeFn>(
            GetProcAddress(hExplorerCompat, "ExplorerCompat_NotifyDesktopBridgeWindow"));
        if (!p)
        {
            if (!g_loggedMissingExplorerCompat)
            {
                g_loggedMissingExplorerCompat = true;
                DebugLog(
                    DBG_WARNING,
                    L"[WndCompat][WinUiBridge] GetProcAddress(ExplorerCompat_NotifyDesktopBridgeWindow) failed");
            }
            return nullptr;
        }

        g_pfnNotifyBridge = p;
        return g_pfnNotifyBridge;
    }
}

namespace WndCompat
{
    void WinUiDesktopBridge_Attach(HWND hWnd) noexcept
    {
        if (!hWnd || !IsWindow(hWnd))
            return;

        if (!Shared::ExplorerCompatHostPolicy::IsExplorerProcess())
            return;

        auto* pfn = ResolveNotifyProc();
        if (!pfn)
        {
            std::lock_guard lock(g_bridgeMutex);
            if (std::find(g_bridgeWindows.begin(), g_bridgeWindows.end(), hWnd) != g_bridgeWindows.end())
                return;
            if (std::find(g_pendingBridgeHwnds.begin(), g_pendingBridgeHwnds.end(), hWnd) !=
                g_pendingBridgeHwnds.end())
                return;
            if (g_pendingBridgeHwnds.size() >= kMaxPendingBridges)
            {
                static bool s_pendingFullLogged = false;
                if (!s_pendingFullLogged)
                {
                    s_pendingFullLogged = true;
                    DebugLog(
                        DBG_WARNING,
                        L"[WndCompat][WinUiBridge] очередь отложенных мостов полна (%u) — hwnd отброшен",
                        kMaxPendingBridges);
                }
                return;
            }
            g_pendingBridgeHwnds.push_back(hWnd);
            DebugLog(DBG_NORMAL, L"[WndCompat][WinUiBridge] мост в очереди до ExplorerCompat: hwnd=0x%p", hWnd);
            return;
        }

        {
            std::lock_guard lock(g_bridgeMutex);
            if (std::find(g_bridgeWindows.begin(), g_bridgeWindows.end(), hWnd) != g_bridgeWindows.end())
                return;
            g_bridgeWindows.push_back(hWnd);
        }

        pfn(hWnd);
        DebugLog(DBG_NORMAL, L"[WndCompat][WinUiBridge] Attach: hwnd=0x%p → ExplorerCompat_NotifyDesktopBridgeWindow", hWnd);
    }

    void WinUiDesktopBridge_Detach(HWND hWnd) noexcept
    {
        {
            std::lock_guard lock(g_bridgeMutex);
            const auto it = std::find(g_bridgeWindows.begin(), g_bridgeWindows.end(), hWnd);
            if (it != g_bridgeWindows.end())
                g_bridgeWindows.erase(it);
            const auto pit = std::find(g_pendingBridgeHwnds.begin(), g_pendingBridgeHwnds.end(), hWnd);
            if (pit != g_pendingBridgeHwnds.end())
                g_pendingBridgeHwnds.erase(pit);
        }
        (void)hWnd;
    }

    void WinUiDesktopBridge_DetachAll() noexcept
    {
        std::vector<HWND> windows;
        {
            std::lock_guard lock(g_bridgeMutex);
            windows.swap(g_bridgeWindows);
            g_pendingBridgeHwnds.clear();
        }
        DebugLog(
            DBG_IMPORTANT,
            L"[WndCompat][WinUiBridge] DetachAll: очищено %u записей",
            static_cast<unsigned>(windows.size()));
    }

    void WinUiDesktopBridge_FlushPendingAfterExplorerCompatReady() noexcept
    {
        auto* pfn = ResolveNotifyProc();
        if (!pfn)
            return;

        std::vector<HWND> pending;
        {
            std::lock_guard lock(g_bridgeMutex);
            pending.swap(g_pendingBridgeHwnds);
        }

        if (pending.empty())
            return;

        unsigned delivered = 0;
        for (HWND h : pending)
        {
            if (!h || !IsWindow(h))
                continue;

            bool shouldNotify = false;
            {
                std::lock_guard lock(g_bridgeMutex);
                if (std::find(g_bridgeWindows.begin(), g_bridgeWindows.end(), h) != g_bridgeWindows.end())
                    continue;
                g_bridgeWindows.push_back(h);
                shouldNotify = true;
            }
            if (shouldNotify)
            {
                pfn(h);
                ++delivered;
            }
        }

        if (delivered != 0u)
        {
            DebugLog(
                DBG_IMPORTANT,
                L"[WndCompat][WinUiBridge] FlushPending: доставлено %u отложенных мостов в ExplorerCompat",
                delivered);
        }
    }
}
