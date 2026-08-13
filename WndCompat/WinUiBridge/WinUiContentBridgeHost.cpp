// Windows.UI.Composition.DesktopWindowContentBridge под Shell_TrayWnd: без субкласса HWND.
// Уведомление TaskbarCompat (IXDE по событию), зеркало WinUiDesktopBridge → ExplorerCompat.
#include "WndPch.h"
#include "WinUiBridge/WinUiContentBridgeHost.h"

#include <DebugLog.h>

#include <algorithm>
#include <mutex>
#include <vector>

namespace
{
    using TaskbarCompatNotifyContentBridgeFn       = BOOL(WINAPI*)(HWND);
    using TaskbarCompatNotifyTrayTrackBarFn        = BOOL(WINAPI*)(HWND);
    using TaskbarCompatReleaseTrayInterestFn       = void(WINAPI*)();
    using TaskbarCompatResetIxdeSchedulingFailuresFn = void(WINAPI*)();

    constexpr unsigned kMaxPendingBridges = 32;

    std::mutex                          g_bridgeMutex;
    std::vector<HWND>                   g_bridgeWindows;
    std::vector<HWND>                   g_pendingBridgeHwnds;
    TaskbarCompatNotifyContentBridgeFn  g_pfnNotifyTaskbarBridge = nullptr;
    TaskbarCompatNotifyTrayTrackBarFn   g_pfnNotifyTrayTrackBar  = nullptr;
    TaskbarCompatReleaseTrayInterestFn  g_pfnReleaseInterest     = nullptr;
    TaskbarCompatResetIxdeSchedulingFailuresFn g_pfnResetScheduling = nullptr;
    bool                                g_loggedMissingTaskbarCompat = false;

    [[nodiscard]] bool IsUnderShellTrayRoot(HWND hWnd) noexcept
    {
        if (!hWnd || !IsWindow(hWnd))
            return false;
        const HWND root = GetAncestor(hWnd, GA_ROOT);
        if (!root || !IsWindow(root))
            return false;
        wchar_t cls[64]{};
        if (GetClassNameW(root, cls, static_cast<int>(std::size(cls))) <= 0)
            return false;
        return wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0;
    }

    void RefreshTaskbarExportsUnlocked(bool forceReload) noexcept
    {
        if (!forceReload && g_pfnNotifyTaskbarBridge)
            return;

        const HMODULE hTaskbar = GetModuleHandleW(L"TaskbarCompat.dll");
        if (!hTaskbar)
        {
            if (!g_loggedMissingTaskbarCompat)
            {
                g_loggedMissingTaskbarCompat = true;
                DebugLog(
                    DBG_NORMAL,
                    L"[WndCompat][WinUiContentBridge] TaskbarCompat.dll ещё не в процессе — Notify отложен "
                    L"(очередь до Flush)");
            }
            g_pfnNotifyTaskbarBridge = nullptr;
            g_pfnNotifyTrayTrackBar  = nullptr;
            g_pfnReleaseInterest     = nullptr;
            g_pfnResetScheduling     = nullptr;
            return;
        }

        g_pfnNotifyTaskbarBridge = reinterpret_cast<TaskbarCompatNotifyContentBridgeFn>(
            GetProcAddress(hTaskbar, "TaskbarCompat_NotifyContentBridgeWindow"));
        g_pfnNotifyTrayTrackBar = reinterpret_cast<TaskbarCompatNotifyTrayTrackBarFn>(
            GetProcAddress(hTaskbar, "TaskbarCompat_NotifyTrayTrackBarHwnd"));
        g_pfnReleaseInterest = reinterpret_cast<TaskbarCompatReleaseTrayInterestFn>(
            GetProcAddress(hTaskbar, "TaskbarCompat_ReleaseTrayIxdeSurfaceInterest"));
        g_pfnResetScheduling = reinterpret_cast<TaskbarCompatResetIxdeSchedulingFailuresFn>(
            GetProcAddress(hTaskbar, "TaskbarCompat_ResetIxdeSchedulingAfterFailure"));

        if (!g_pfnNotifyTaskbarBridge)
        {
            if (!g_loggedMissingTaskbarCompat)
            {
                g_loggedMissingTaskbarCompat = true;
                DebugLog(
                    DBG_WARNING,
                    L"[WndCompat][WinUiContentBridge] GetProcAddress(TaskbarCompat_NotifyContentBridgeWindow) failed");
            }
            return;
        }

        g_loggedMissingTaskbarCompat = false;
    }

    void CallReleaseTaskbarInterestUnlocked() noexcept
    {
        const HMODULE hTaskbar = GetModuleHandleW(L"TaskbarCompat.dll");
        if (!hTaskbar)
            return;
        if (!g_pfnReleaseInterest)
            g_pfnReleaseInterest = reinterpret_cast<TaskbarCompatReleaseTrayInterestFn>(
                GetProcAddress(hTaskbar, "TaskbarCompat_ReleaseTrayIxdeSurfaceInterest"));
        if (g_pfnReleaseInterest)
            g_pfnReleaseInterest();
    }
}

namespace WndCompat
{
    [[nodiscard]] bool WinUi_IsUnderShellTrayRoot(HWND hWnd) noexcept
    {
        return IsUnderShellTrayRoot(hWnd);
    }

    void WinUi_ResetTaskbarCompatNotifyProcCache() noexcept
    {
        std::lock_guard lock(g_bridgeMutex);
        g_pfnNotifyTaskbarBridge     = nullptr;
        g_pfnNotifyTrayTrackBar      = nullptr;
        g_pfnReleaseInterest         = nullptr;
        g_pfnResetScheduling         = nullptr;
        g_loggedMissingTaskbarCompat = false;
    }

    void WinUi_TrayTrackBar_ReportIxdeIntent(HWND hWnd) noexcept
    {
        if (!IsUnderShellTrayRoot(hWnd))
            return;

        decltype(g_pfnNotifyTrayTrackBar) pfn = nullptr;
        {
            std::lock_guard lock(g_bridgeMutex);
            RefreshTaskbarExportsUnlocked(/*forceReload=*/true);
            pfn = g_pfnNotifyTrayTrackBar;
        }

        if (!pfn)
        {
            DebugLog(DBG_NORMAL, L"[WndCompat][WinUiContentBridge] NotifyTrayTrackBar export недоступен");
            return;
        }

        (void)pfn(hWnd);
    }

    void WinUi_TrayTrackBar_ReleaseIxdeIntent(HWND hWnd) noexcept
    {
        if (!IsUnderShellTrayRoot(hWnd))
            return;
        std::lock_guard lock(g_bridgeMutex);
        RefreshTaskbarExportsUnlocked(/*forceReload=*/false);
        CallReleaseTaskbarInterestUnlocked();
    }

    void WinUi_OnTaskbarIxdeOutcome(BOOL succeeded) noexcept
    {
        if (succeeded)
            return;

        WinUi_ResetTaskbarCompatNotifyProcCache();
        if (const HMODULE hTb = GetModuleHandleW(L"TaskbarCompat.dll"))
        {
            auto* const resetFn = reinterpret_cast<TaskbarCompatResetIxdeSchedulingFailuresFn>(
                GetProcAddress(hTb, "TaskbarCompat_ResetIxdeSchedulingAfterFailure"));
            if (resetFn)
                resetFn();
        }

        std::vector<HWND> toReattach;
        {
            std::lock_guard lock(g_bridgeMutex);
            RefreshTaskbarExportsUnlocked(/*forceReload=*/true);
            g_pendingBridgeHwnds.clear();
            g_bridgeWindows.swap(toReattach);
        }

        for (HWND bridge : toReattach)
        {
            if (bridge && IsWindow(bridge) && IsUnderShellTrayRoot(bridge))
                WinUiContentBridge_Attach(bridge);
        }
    }

    void WinUiContentBridge_Attach(HWND hWnd) noexcept
    {
        if (!hWnd || !IsWindow(hWnd))
            return;
        if (!IsUnderShellTrayRoot(hWnd))
            return;

        TaskbarCompatNotifyContentBridgeFn pfn = nullptr;
        {
            std::lock_guard lock(g_bridgeMutex);
            RefreshTaskbarExportsUnlocked(/*forceReload=*/false);

            if (!g_pfnNotifyTaskbarBridge)
            {
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
                            L"[WndCompat][WinUiContentBridge] очередь отложенных мостов полна (%u) — hwnd отброшен",
                            kMaxPendingBridges);
                    }
                    return;
                }
                g_pendingBridgeHwnds.push_back(hWnd);
                DebugLog(
                    DBG_NORMAL,
                    L"[WndCompat][WinUiContentBridge] мост в очереди до TaskbarCompat: hwnd=0x%p",
                    hWnd);
                return;
            }

            if (std::find(g_bridgeWindows.begin(), g_bridgeWindows.end(), hWnd) != g_bridgeWindows.end())
                return;
            pfn = g_pfnNotifyTaskbarBridge;
        }

        const BOOL notifyAccepted = pfn(hWnd);

        if (!notifyAccepted)
        {
            std::lock_guard lock(g_bridgeMutex);
            RefreshTaskbarExportsUnlocked(/*forceReload=*/false);
            if (!g_pfnNotifyTaskbarBridge)
                return;
            if (std::find(g_bridgeWindows.begin(), g_bridgeWindows.end(), hWnd) != g_bridgeWindows.end())
                return;
            if (std::find(g_pendingBridgeHwnds.begin(), g_pendingBridgeHwnds.end(), hWnd) !=
                g_pendingBridgeHwnds.end())
                return;
            if (g_pendingBridgeHwnds.size() >= kMaxPendingBridges)
            {
                DebugLog(
                    DBG_WARNING,
                    L"[WndCompat][WinUiContentBridge] notify от TaskbarCompat отвергнут, очередь полна hwnd=0x%p",
                    hWnd);
                return;
            }
            g_pendingBridgeHwnds.push_back(hWnd);
            DebugLog(
                DBG_NORMAL,
                L"[WndCompat][WinUiContentBridge] Notify отложен после отказа TAP hwnd=0x%p — ждём Prepare/Flush",
                hWnd);
            return;
        }

        {
            std::lock_guard lock(g_bridgeMutex);
            if (std::find(g_bridgeWindows.begin(), g_bridgeWindows.end(), hWnd) == g_bridgeWindows.end())
                g_bridgeWindows.push_back(hWnd);
        }

        DebugLog(
            DBG_NORMAL,
            L"[WndCompat][WinUiContentBridge] Attach: hwnd=0x%p → TaskbarCompat_NotifyContentBridgeWindow",
            hWnd);
    }

    namespace
    {
        void WalkTraySubtreeForContentBridges(HWND root, unsigned& bridgeTries) noexcept
        {
            for (HWND ch = GetWindow(root, GW_CHILD); ch; ch = GetWindow(ch, GW_HWNDNEXT))
            {
                wchar_t cls[256]{};
                if (GetClassNameW(ch, cls, static_cast<int>(std::size(cls))) > 0 &&
                    wcscmp(cls, L"Windows.UI.Composition.DesktopWindowContentBridge") == 0)
                {
                    WinUiContentBridge_Attach(ch);
                    ++bridgeTries;
                }
                WalkTraySubtreeForContentBridges(ch, bridgeTries);
            }
        }

        void RescanShellTraysForContentBridges() noexcept
        {
            unsigned tries = 0;
            if (const HWND r1 = FindWindowW(L"Shell_TrayWnd", nullptr))
                WalkTraySubtreeForContentBridges(r1, tries);
            if (const HWND r2 = FindWindowW(L"Shell_SecondaryTrayWnd", nullptr))
                WalkTraySubtreeForContentBridges(r2, tries);
            if (tries != 0u)
            {
                DebugLog(
                    DBG_IMPORTANT,
                    L"[WndCompat][WinUiContentBridge] RescanTray: в дереве трея DesktopWindowContentBridge=%u",
                    tries);
            }
        }
    }

    void WinUiContentBridge_Detach(HWND hWnd) noexcept
    {
        bool hadActiveBridge = false;
        {
            std::lock_guard lock(g_bridgeMutex);
            const auto it = std::find(g_bridgeWindows.begin(), g_bridgeWindows.end(), hWnd);
            if (it != g_bridgeWindows.end())
            {
                hadActiveBridge = true;
                g_bridgeWindows.erase(it);
            }
            const auto pit = std::find(g_pendingBridgeHwnds.begin(), g_pendingBridgeHwnds.end(), hWnd);
            if (pit != g_pendingBridgeHwnds.end())
                g_pendingBridgeHwnds.erase(pit);
        }
        if (hadActiveBridge && IsUnderShellTrayRoot(hWnd))
        {
            std::lock_guard lock(g_bridgeMutex);
            CallReleaseTaskbarInterestUnlocked();
        }
    }

    void WinUiContentBridge_DetachAll() noexcept
    {
        std::vector<HWND> windows;
        {
            std::lock_guard lock(g_bridgeMutex);
            windows.swap(g_bridgeWindows);
            g_pendingBridgeHwnds.clear();
        }

        WinUi_ResetTaskbarCompatNotifyProcCache();

        for (HWND w : windows)
        {
            if (w && IsUnderShellTrayRoot(w))
            {
                std::lock_guard lock(g_bridgeMutex);
                CallReleaseTaskbarInterestUnlocked();
            }
        }

        DebugLog(
            DBG_IMPORTANT,
            L"[WndCompat][WinUiContentBridge] DetachAll: очищено %u записей с release интереса",
            static_cast<unsigned>(windows.size()));
    }

    void WinUiContentBridge_FlushPendingAfterTaskbarCompatReady() noexcept
    {
        WinUi_ResetTaskbarCompatNotifyProcCache();

        std::vector<HWND> pending;
        TaskbarCompatNotifyContentBridgeFn pfn = nullptr;

        {
            std::lock_guard lock(g_bridgeMutex);
            RefreshTaskbarExportsUnlocked(/*forceReload=*/true);
            pfn = g_pfnNotifyTaskbarBridge;
            pending.swap(g_pendingBridgeHwnds);
        }

        if (!pfn)
        {
            DebugLog(DBG_WARNING, L"[WndCompat][WinUiContentBridge] FlushPending: экспорт Notify недоступен");
            std::lock_guard lock(g_bridgeMutex);
            for (HWND h : pending)
            {
                if (std::find(g_pendingBridgeHwnds.begin(), g_pendingBridgeHwnds.end(), h) ==
                    g_pendingBridgeHwnds.end())
                    g_pendingBridgeHwnds.push_back(h);
            }
            return;
        }

        unsigned delivered = 0;
        for (HWND h : pending)
        {
            if (!h || !IsWindow(h))
                continue;

            {
                std::lock_guard lock(g_bridgeMutex);
                if (std::find(g_bridgeWindows.begin(), g_bridgeWindows.end(), h) != g_bridgeWindows.end())
                    continue;
            }

            const BOOL notifyAccepted = pfn(h);
            if (!notifyAccepted)
            {
                std::lock_guard lock(g_bridgeMutex);
                if (std::find(g_bridgeWindows.begin(), g_bridgeWindows.end(), h) == g_bridgeWindows.end() &&
                    std::find(g_pendingBridgeHwnds.begin(), g_pendingBridgeHwnds.end(), h) ==
                        g_pendingBridgeHwnds.end() &&
                    g_pendingBridgeHwnds.size() < kMaxPendingBridges)
                {
                    g_pendingBridgeHwnds.push_back(h);
                }
                continue;
            }

            {
                std::lock_guard lock(g_bridgeMutex);
                if (std::find(g_bridgeWindows.begin(), g_bridgeWindows.end(), h) == g_bridgeWindows.end())
                    g_bridgeWindows.push_back(h);
            }
            ++delivered;
        }

        if (delivered != 0u)
        {
            DebugLog(
                DBG_IMPORTANT,
                L"[WndCompat][WinUiContentBridge] FlushPending: доставлено %u отложенных мостов в TaskbarCompat",
                delivered);
        }

        RescanShellTraysForContentBridges();
    }

    void WinUiContentBridge_RescanShellTraysDesktopBridges() noexcept
    {
        RescanShellTraysForContentBridges();
    }
}

extern "C" __declspec(dllexport) void WINAPI WndCompat_OnTaskbarIxdeInstallOutcome(BOOL succeeded) noexcept
{
    WndCompat::WinUi_OnTaskbarIxdeOutcome(succeeded);
}
