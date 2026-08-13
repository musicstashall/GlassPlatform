#include "WndPch.h"
#include "Tracker/BackdropPolicyRescan.h"

#include "Tracker/WindowClassifier.h"
#include "Tracker/WindowClassUtils.h"
#include "Tracker/GlassAttachGate.h"
#include "Tracker/WinEventHandler.h"
#include "Root/RootSubclass.h"
#include "Root/ExplorerRootSubclass.h"
#include <Controls/TreeViewSubclass.h>
#include <Controls/ButtonSubclass.h>
#include <Controls/ListViewSubclass.h>
#include <Explorer/ExplorerIslandContainerSubclass.h>
#include <WinUiBridge/WinUiDesktopBridgeHost.h>
#include <GlassControlKind.h>
#include <GlassMessages.h>
#include <RootPolicyQuery.h>
#include <SystemHelper.h>
#include <ExplorerCompatHostPolicy.h>
#include <WndCompat.h>
#include <atomic>
#include <cstdio>

extern "C" void WINAPI WndCompat_HandleWinEvent(
    DWORD event,
    HWND hWnd,
    LONG idObject,
    LONG idChild,
    DWORD dwEventThread,
    DWORD dwmsEventTime);

namespace
{
    using GK = GlassShared::GlassControlKind;

    std::atomic<ULONGLONG> g_lastBackdropEnableRescanTick{};

    void DispatchSyntheticCreateSameThread(HWND hWnd) noexcept
    {
        WndCompat_HandleWinEvent(
            EVENT_OBJECT_CREATE,
            hWnd,
            OBJID_WINDOW,
            CHILDID_SELF,
            GetCurrentThreadId(),
            0);
    }

    void PostSyntheticCreateCrossThread(HWND hWnd, UINT msg) noexcept
    {
        WndCompat::EnsureGetMsgHookForHwnd(hWnd);
        if (!PostMessageW(hWnd, msg, 0, 0))
        {
            DebugLog(DBG_WARNING,
                L"[WndCompat][Rescan] PostMessage synthetic create failed hwnd=0x%p msg=0x%X GLE=%u",
                hWnd,
                msg,
                GetLastError());
        }
    }

    void SendSyntheticCreateCrossThread(HWND hWnd, UINT msg) noexcept
    {
        WndCompat::EnsureGetMsgHookForHwnd(hWnd);
        DWORD_PTR sendResult = 0;
        const BOOL sent = SendMessageTimeoutW(
            hWnd,
            msg,
            0,
            0,
            SMTO_ABORTIFHUNG,
            1000,
            &sendResult);
        if (!sent)
        {
            DebugLog(DBG_WARNING,
                L"[WndCompat][Rescan] SendMessage synthetic create timeout/fail hwnd=0x%p msg=0x%X",
                hWnd,
                msg);
        }
    }

    [[nodiscard]] UINT SyntheticCreateMessageForHwnd(HWND hWnd) noexcept
    {
        const auto [role, kind] = WindowClassifier::Classify(hWnd);
        (void)kind;
        if (role == WindowRole::Root || role == WindowRole::Tray)
            return WndCompat::WM_GLASS_ATTACH_ROOT;
        return WndCompat::WM_GLASS_SYNTHETIC_CREATE;
    }

    [[nodiscard]] bool IsTreeViewTracked(HWND hWnd) noexcept
    {
        return WndCompat::TreeView_IsTracked(hWnd);
    }

    [[nodiscard]] bool IsButtonTracked(HWND hWnd) noexcept
    {
        return WndCompat::Button_IsTracked(hWnd);
    }

    [[nodiscard]] bool IsListViewTracked(HWND hWnd) noexcept
    {
        return WndCompat::ListView_IsTracked(hWnd);
    }

    void TryCatchUpMarkedControlAttach(HWND hWnd) noexcept
    {
        if (!hWnd || !IsWindow(hWnd))
            return;
        if (!WndCompat::GlassAttachGate::ShouldAttachGlassToHwnd(hWnd))
            return;

        const GK marked = GlassShared::GlassGetWindowKind(hWnd);
        if (marked == GK::None)
            return;

        switch (marked)
        {
        case GK::TreeView:
            if (!IsTreeViewTracked(hWnd))
                WndCompat::TreeView_Attach(hWnd);
            break;
        case GK::Button:
        case GK::WinForms_Button:
            if (!IsButtonTracked(hWnd))
                WndCompat::Button_Attach(hWnd, marked);
            break;
        case GK::ListView:
        case GK::WinForms_ListView:
            if (!IsListViewTracked(hWnd))
                WndCompat::ListView_Attach(hWnd);
            break;
        default:
            break;
        }
    }

    void TryRescanControlHwnd(HWND hChild) noexcept
    {
        if (!hChild || !IsWindow(hChild))
            return;

        if (WndCompat::IsGlassSettingsBitmapSwitchHwnd(hChild))
            return;

        if (GlassShared::GlassGetWindowKind(hChild) != GK::None)
        {
            TryCatchUpMarkedControlAttach(hChild);
            return;
        }

        if (!WndCompat::GlassAttachGate::ShouldAttachGlassToHwnd(hChild))
            return;

        WndCompat::BackdropPolicyRescan::MarshalSyntheticCreateAttach(hChild);
    }

    void VisitRecursiveGwChild(HWND hWnd) noexcept
    {
        for (HWND c = GetWindow(hWnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
        {
            TryRescanControlHwnd(c);
            VisitRecursiveGwChild(c);
        }
    }

    [[nodiscard]] bool IsWinUiHostClass(const wchar_t* cls) noexcept
    {
        if (!cls)
            return false;
        return wcscmp(cls, L"Microsoft.UI.Content.DesktopChildSiteBridge") == 0
            || wcscmp(cls, L"FileExplorer_FolderViewIsland") == 0
            || wcscmp(cls, L"XamlExplorerHostIslandWindow") == 0
            || wcscmp(cls, L"SHELLDLL_DefView") == 0
            || wcscmp(cls, L"ShellTabWindowClass") == 0;
    }

    void TryRescanWinUiHostHwnd(HWND hChild) noexcept
    {
        if (!hChild || !IsWindow(hChild))
            return;

        wchar_t cls[128]{};
        if (GetClassNameW(hChild, cls, static_cast<int>(std::size(cls))) <= 0)
            return;
        if (!IsWinUiHostClass(cls))
            return;
        if (!WndCompat::GlassAttachGate::ShouldAttachGlassToHwnd(hChild))
            return;

        if (wcscmp(cls, L"Microsoft.UI.Content.DesktopChildSiteBridge") == 0)
            WndCompat::WinUiDesktopBridge_Attach(hChild);
        else if (wcscmp(cls, L"SHELLDLL_DefView") == 0
            || wcscmp(cls, L"ShellTabWindowClass") == 0
            || wcscmp(cls, L"FileExplorer_FolderViewIsland") == 0
            || wcscmp(cls, L"XamlExplorerHostIslandWindow") == 0)
        {
            WndCompat::ExplorerIslandContainer_Attach(hChild);
        }
    }

    BOOL CALLBACK RescanWinUiHostsProc(HWND hChild, LPARAM) noexcept
    {
        TryRescanWinUiHostHwnd(hChild);
        EnumChildWindows(hChild, RescanWinUiHostsProc, 0);
        return TRUE;
    }

    void RescanMissingRootsForProcess() noexcept
    {
        if (WndCompat::IsShuttingDown())
            return;
        if (WndCompat::WinEventHook_GetMode() != Shared::InjectionMode::Full)
            return;

        const DWORD pid = GetCurrentProcessId();
        EnumWindows(
            [](HWND hTop, LPARAM lPid) -> BOOL {
                DWORD wndPid = 0;
                GetWindowThreadProcessId(hTop, &wndPid);
                if (wndPid != static_cast<DWORD>(lPid))
                    return TRUE;
                if (GetAncestor(hTop, GA_ROOT) != hTop)
                    return TRUE;
                if (WndCompat::Root_IsAttached(hTop))
                    return TRUE;
                if (!WndCompat::GlassAttachGate::ShouldAttachGlassToHwnd(hTop))
                    return TRUE;

                const auto [role, kind] = WindowClassifier::Classify(hTop);
                if (role != WindowRole::Root)
                    return TRUE;

                if (kind == ControlKind::FileExplorerShellRoot
                    && Shared::SystemHelper::g_buildNumber >= 22000)
                {
                    WndCompat::DispatchExplorerRootAttachToOwnerThread(hTop);
                }
                else
                {
                    WndCompat::DispatchRootAttachToOwnerThread(hTop);
                }
                return TRUE;
            },
            static_cast<LPARAM>(pid));
    }

    void RescanMissingWinUiHostsUnderRoots(const std::vector<HWND>& roots) noexcept
    {
        for (HWND hRoot : roots)
        {
            if (!hRoot || !IsWindow(hRoot))
                continue;
            EnumChildWindows(hRoot, RescanWinUiHostsProc, 0);
        }
    }

    void RescanMissingControlsUnderTrackedRoots(const std::vector<HWND>& roots) noexcept
    {
        if (WndCompat::IsShuttingDown())
            return;
        if (!Shared::RootPolicyQuery::IsRootGlassRenderingEnabled())
            return;

        for (HWND hRoot : roots)
        {
            if (!hRoot || !IsWindow(hRoot))
                continue;
            VisitRecursiveGwChild(hRoot);
        }

        DebugLog(DBG_IMPORTANT,
            L"[WndCompat][Rescan] RescanMissingControlsUnderTrackedRoots: roots=%zu",
            roots.size());
    }

    void CopyTrackedRoots(std::vector<HWND>& out) noexcept
    {
        WndCompat::Root_CopyTrackedRoots(out);
    }
}

namespace WndCompat::BackdropPolicyRescan
{
    void MarshalSyntheticCreateAttach(HWND hWnd) noexcept
    {
        if (!hWnd || !IsWindow(hWnd))
            return;
        if (WndCompat::IsShuttingDown())
            return;

        const DWORD curTid = GetCurrentThreadId();
        const DWORD wndTid = GetWindowThreadProcessId(hWnd, nullptr);
        const UINT msg = SyntheticCreateMessageForHwnd(hWnd);

        if (wndTid == curTid)
        {
            DispatchSyntheticCreateSameThread(hWnd);
            return;
        }

        if (msg == WM_GLASS_ATTACH_ROOT)
            PostSyntheticCreateCrossThread(hWnd, msg);
        else
            SendSyntheticCreateCrossThread(hWnd, msg);
    }

    void ScanSubtreeRecursive(HWND hSubtreeRoot, WindowScanContext& ctx) noexcept
    {
        if (!hSubtreeRoot || !IsWindow(hSubtreeRoot))
            return;

        const DWORD wndTid = GetWindowThreadProcessId(hSubtreeRoot, nullptr);
        if (wndTid == ctx.scanTid)
        {
            DispatchSyntheticCreateSameThread(hSubtreeRoot);
            ++ctx.stats.childDirect;
        }
        else if (wndTid != 0)
        {
            const UINT msg = SyntheticCreateMessageForHwnd(hSubtreeRoot);
            WndCompat::EnsureGetMsgHookForHwnd(hSubtreeRoot);
            if (msg == WM_GLASS_SYNTHETIC_CREATE)
                SendSyntheticCreateCrossThread(hSubtreeRoot, msg);
            else if (PostMessageW(hSubtreeRoot, msg, 0, 0))
                ++ctx.stats.childPosted;
            else
            {
                DebugLog(DBG_WARNING,
                    L"[WndCompat][WinEvent] %s child: PostMessage failed hwnd=0x%p GLE=%u",
                    ctx.phase,
                    hSubtreeRoot,
                    GetLastError());
            }
        }

        for (HWND c = GetWindow(hSubtreeRoot, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
            ScanSubtreeRecursive(c, ctx);
    }

    void RunProcessWindowScan(WindowScanContext& ctx) noexcept
    {
        EnumWindows(
            [](HWND hWnd, LPARAM lParam) -> BOOL {
                auto* scan = reinterpret_cast<WindowScanContext*>(lParam);
                if (!scan)
                    return TRUE;

                DWORD wndPid = 0;
                const DWORD wndTid = GetWindowThreadProcessId(hWnd, &wndPid);
                if (wndPid != scan->pid)
                    return TRUE;

                if (wndTid == scan->scanTid)
                {
                    DispatchSyntheticCreateSameThread(hWnd);
                    ++scan->stats.topDirect;
                }
                else
                {
                    const auto [role, kind] = WindowClassifier::Classify(hWnd);
                    // H32: CoreWindow Special не постить на scan — только SHOW (иначе poke dormant).
                    const bool coreWindowSpecial =
                        kind == ControlKind::ShellNotificationCore
                        || kind == ControlKind::ShellStartMenuCore;
                    const bool shouldPost =
                        ((role == WindowRole::Root || role == WindowRole::Tray) &&
                            scan->mode == Shared::InjectionMode::Full) ||
                        role == WindowRole::Popup ||
                        (role == WindowRole::Special && !coreWindowSpecial);

                    if (shouldPost)
                    {
                        WndCompat::EnsureGetMsgHookForHwnd(hWnd);
                        if (PostMessageW(hWnd, WndCompat::WM_GLASS_ATTACH_ROOT, 0, 0))
                        {
                            ++scan->stats.topPosted;
                            if (scan->verbose)
                            {
                                DebugLog(
                                    L"[WndCompat][WinEvent] %s: posted WM_GLASS_ATTACH_ROOT hwnd=0x%p role=%d",
                                    scan->phase,
                                    hWnd,
                                    static_cast<int>(role));
                            }
                        }
                        else
                        {
                            DebugLog(DBG_WARNING,
                                L"[WndCompat][WinEvent] %s: PostMessage WM_GLASS_ATTACH_ROOT failed hwnd=0x%p GLE=%u",
                                scan->phase,
                                hWnd,
                                GetLastError());
                        }
                    }
                }

                // H12: initial scan не обходит всё дерево SendMessageTimeout — иначе GUI-поток
                // зависает в DoInitialize, g_initDone не ставится, Tray/TaskbarCompat не поднимаются.
                if (!scan->topLevelOnly)
                    ScanSubtreeRecursive(hWnd, *scan);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&ctx));

    }

    void DispatchSyntheticCreateAttachRecursive(HWND hRoot) noexcept
    {
        if (!hRoot || !IsWindow(hRoot))
            return;
        VisitRecursiveGwChild(hRoot);
    }

    void RescanAllMissingGlassHostsForBackdropEnable() noexcept
    {
        if (WndCompat::IsShuttingDown())
            return;
        if (!Shared::RootPolicyQuery::IsRootGlassRenderingEnabled())
            return;

        constexpr ULONGLONG kCoalesceMs = 800;
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG prev = g_lastBackdropEnableRescanTick.load(std::memory_order_relaxed);
        if (prev != 0 && now >= prev && (now - prev) < kCoalesceMs)
        {
            DebugLog(DBG_IMPORTANT,
                L"[WndCompat][Rescan] RescanAllMissingGlassHostsForBackdropEnable: coalesced skip "
                L"(delta=%llu ms)",
                static_cast<unsigned long long>(now - prev));
            return;
        }
        g_lastBackdropEnableRescanTick.store(now, std::memory_order_relaxed);

        RescanMissingRootsForProcess();

        std::vector<HWND> roots;
        CopyTrackedRoots(roots);

        for (HWND hRoot : roots)
        {
            if (!hRoot || !IsWindow(hRoot))
                continue;
            VisitRecursiveGwChild(hRoot);
        }
        RescanMissingWinUiHostsUnderRoots(roots);

        DebugLog(DBG_IMPORTANT,
            L"[WndCompat][Rescan] RescanAllMissingGlassHostsForBackdropEnable: roots=%zu",
            roots.size());
    }

    void ScheduleSubtreeRescanAfterRootAttach(HWND hRoot) noexcept
    {
        DispatchSyntheticCreateAttachRecursive(hRoot);
    }

    void RescanMissingControlsUnderTrackedRoots(const std::vector<HWND>& roots) noexcept
    {
        if (WndCompat::IsShuttingDown())
            return;
        if (!Shared::RootPolicyQuery::IsRootGlassRenderingEnabled())
            return;

        for (HWND hRoot : roots)
        {
            if (!hRoot || !IsWindow(hRoot))
                continue;
            VisitRecursiveGwChild(hRoot);
        }

        DebugLog(DBG_IMPORTANT,
            L"[WndCompat][Rescan] RescanMissingControlsUnderTrackedRoots: roots=%zu",
            roots.size());
    }
}
