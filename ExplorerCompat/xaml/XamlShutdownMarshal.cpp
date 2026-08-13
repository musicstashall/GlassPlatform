#include "ExplorerPch.h"
#include "xaml/Patcher/XamlHostBarPatcher.h"
#include "xaml/Patcher/XamlHostBarPatcherState.h"
#include "xaml/XamlShutdownMarshal.hpp"
#include "xaml/XamlTapSite.hpp"
#include "xaml/XamlComLifetime.hpp"

#include <GlassMessages.h>
#include <atomic>

namespace
{
    HWND s_hwndMsg{};
    std::atomic<ULONGLONG> g_lastRootBackdropEnableTick{};
    std::atomic<ULONGLONG> g_lastRootBackdropDisableTick{};

    LRESULT CALLBACK ShutdownWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) noexcept
    {
        if (msg == ExplorerCompatMsg::WM_GLASS_EXPLORER_DESTROY_SHUTDOWN_WND)
        {
            KillTimer(hwnd, XamlShutdownMarshal::kTrayRedrawAfterAdviseTimerId);
            KillTimer(hwnd, XamlShutdownMarshal::kRepatchTimerId);
            // DestroyWindow только на потоке-владельце (см. DestroyMessageWindow).
            DestroyWindow(hwnd);
            return 0;
        }
        if (msg == ExplorerCompatMsg::WM_GLASS_EXPLORER_XAML_SHUTDOWN)
        {
            XamlTapSite::ShutdownVisualTreeWatcher();
            if (GlassExplorerCom::g_shutdownDone.load(std::memory_order_acquire))
                CoFreeUnusedLibraries();
            return 0;
        }
        if (msg == ExplorerCompatMsg::WM_GLASS_EXPLORER_ROOT_BACKDROP_POLICY)
        {
            if (wParam)
                XamlTapSite::RequestApplyHostBarChromeOnVtcThread();
            else
                XamlTapSite::OnRootBackdropDisabled();
            return 0;
        }
        if (msg == ExplorerCompatMsg::WM_GLASS_EXPLORER_POST_LIVE_DISABLE_REDRAW)
        {
            XamlTapSite::RedrawExplorerShellCaptionAfterLiveOff();
            return 0;
        }
        if (msg == ExplorerCompatMsg::WM_GLASS_EXPLORER_XAML_ADVISE)
        {
            XamlTapSite::CompleteDeferredAdvise();
            return 0;
        }
        if (msg == WM_TIMER && wParam == XamlShutdownMarshal::kTrayRedrawAfterAdviseTimerId)
        {
            KillTimer(hwnd, XamlShutdownMarshal::kTrayRedrawAfterAdviseTimerId);
            XamlTapSite::OnTrayRedrawAfterAdviseTimer();
            return 0;
        }
        if (msg == WM_TIMER && wParam == XamlShutdownMarshal::kRepatchTimerId)
        {
            XamlTapSite::OnRepatchTimer();
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

HWND XamlShutdownMarshal::GetMessageWindowHwnd() noexcept
{
    return (s_hwndMsg && IsWindow(s_hwndMsg)) ? s_hwndMsg : nullptr;
}

DWORD XamlShutdownMarshal::GetMessageWindowThreadId() noexcept
{
    const HWND h = GetMessageWindowHwnd();
    if (!h)
        return 0;
    DWORD tid = 0;
    (void)GetWindowThreadProcessId(h, &tid);
    return tid;
}

void XamlShutdownMarshal::EnsureMessageWindow() noexcept
{
    if (s_hwndMsg && IsWindow(s_hwndMsg))
        return;

    HMODULE hMod{};
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&XamlShutdownMarshal::EnsureMessageWindow),
            &hMod))
    {
        hMod = GetModuleHandleW(nullptr);
    }

    // После FreeLibrary ExplorerCompat класс может остаться со stale lpfnWndProc
    // (как UICompat/TaskbarCompat marshal) → CreateWindowExW крашит процесс.
    WNDCLASSEXW existing{};
    existing.cbSize = sizeof(existing);
    const bool classExists =
        GetClassInfoExW(hMod, ExplorerCompatMsg::kExplorerXamlShutdownWndClass, &existing)
        != 0;
    const bool procStale = classExists && existing.lpfnWndProc != ShutdownWndProc;
    if (procStale)
    {
        DebugLog(
            DBG_WARNING,
            L"[ExplorerCompat][XamlShutdownMarshal] marshal-класс от прошлой загрузки — re-register");
        (void)UnregisterClassW(ExplorerCompatMsg::kExplorerXamlShutdownWndClass, hMod);

        WNDCLASSEXW recheck{};
        recheck.cbSize = sizeof(recheck);
        if (GetClassInfoExW(
                hMod, ExplorerCompatMsg::kExplorerXamlShutdownWndClass, &recheck)
            && recheck.lpfnWndProc != ShutdownWndProc)
        {
            DebugLog(
                DBG_ERROR,
                L"[ExplorerCompat][XamlShutdownMarshal] stale marshal-класс не снят — окно не создаём");
            s_hwndMsg = nullptr;
            return;
        }
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ShutdownWndProc;
    wc.hInstance = hMod;
    wc.lpszClassName = ExplorerCompatMsg::kExplorerXamlShutdownWndClass;
    if (RegisterClassExW(&wc) == 0)
    {
        const DWORD gle = GetLastError();
        if (gle != ERROR_CLASS_ALREADY_EXISTS)
        {
            DebugLog(
                DBG_WARNING,
                L"[ExplorerCompat][XamlShutdownMarshal] RegisterClassEx failed GLE=%u",
                static_cast<unsigned>(gle));
            return;
        }
    }

    const HWND created = CreateWindowExW(
        0,
        ExplorerCompatMsg::kExplorerXamlShutdownWndClass,
        L"",
        0,
        0,
        0,
        0,
        0,
        HWND_MESSAGE,
        nullptr,
        hMod,
        nullptr);
    if (!created)
    {
        DebugLog(
            DBG_WARNING,
            L"[ExplorerCompat][XamlShutdownMarshal] CreateWindowEx failed GLE=%u",
            static_cast<unsigned>(GetLastError()));
        return;
    }

    s_hwndMsg = created;
    DebugLog(
        DBG_IMPORTANT,
        L"[ExplorerCompat][XamlShutdownMarshal] HWND_MESSAGE hwnd=0x%p tid=%u (Advise thread)",
        created,
        GetCurrentThreadId());
}

void XamlShutdownMarshal::RunShutdownVisualTreeWatcherOnXamlThread() noexcept
{
    const HWND h = s_hwndMsg;
    if (h && IsWindow(h))
    {
        const DWORD wndTid = GetWindowThreadProcessId(h, nullptr);
        if (wndTid != 0 && GetCurrentThreadId() != wndTid)
        {
            DWORD_PTR result = 0;
            constexpr UINT kTimeoutMs = 15'000;
            if (SendMessageTimeoutW(
                    h,
                    ExplorerCompatMsg::WM_GLASS_EXPLORER_XAML_SHUTDOWN,
                    0,
                    0,
                    SMTO_ABORTIFHUNG,
                    kTimeoutMs,
                    &result) != 0)
            {
                DebugLog(DBG_IMPORTANT,
                    L"[ExplorerCompat][XamlShutdownMarshal] ShutdownVisualTreeWatcher on xaml tid=%u",
                    wndTid);
                return;
            }

            DebugLog(DBG_WARNING,
                L"[ExplorerCompat][XamlShutdownMarshal] SendMessageTimeout failed GLE=%u — повтор 30s",
                static_cast<unsigned>(GetLastError()));
            if (SendMessageTimeoutW(
                    h,
                    ExplorerCompatMsg::WM_GLASS_EXPLORER_XAML_SHUTDOWN,
                    0,
                    0,
                    SMTO_ABORTIFHUNG,
                    30'000,
                    &result) != 0)
            {
                DebugLog(DBG_IMPORTANT,
                    L"[ExplorerCompat][XamlShutdownMarshal] ShutdownVisualTreeWatcher (повтор) ok tid=%u",
                    wndTid);
                return;
            }
            DebugLog(
                DBG_CRITICAL,
                L"[ExplorerCompat][XamlShutdownMarshal] повтор shutdown failed GLE=%u — Unadvise с чужого потока; "
                L"возможна гонка с HWND_MESSAGE",
                static_cast<unsigned>(GetLastError()));
        }
    }

    XamlTapSite::ShutdownVisualTreeWatcher();
}

void XamlShutdownMarshal::RunRootBackdropPolicyChangeOnXamlThread(BOOL enabled) noexcept
{
    if (enabled)
    {
        if (g_pendingApplyHostBarChrome.load(std::memory_order_acquire))
        {
            DebugLog(
                DBG_IMPORTANT,
                L"[ExplorerCompat][XamlShutdownMarshal] OnRootBackdropPolicyChanged enabled=1: coalesced "
                L"(apply already pending)");
            return;
        }

        if (XamlTapSite::ShouldCoalesceLiveHostBarApply())
        {
            DebugLog(
                DBG_IMPORTANT,
                L"[ExplorerCompat][XamlShutdownMarshal] OnRootBackdropPolicyChanged enabled=1: coalesced "
                L"(recent apply success)");
            return;
        }

        const ULONGLONG now = GetTickCount64();
        const ULONGLONG prev = g_lastRootBackdropEnableTick.exchange(now, std::memory_order_acq_rel);
        constexpr ULONGLONG kCoalesceMs = 4000;
        if (prev != 0 && now >= prev && (now - prev) < kCoalesceMs)
        {
            DebugLog(
                DBG_IMPORTANT,
                L"[ExplorerCompat][XamlShutdownMarshal] OnRootBackdropPolicyChanged enabled=1: coalesced skip "
                L"(Δ=%llums)",
                now - prev);
            return;
        }
    }
    else
    {
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG prev = g_lastRootBackdropDisableTick.exchange(now, std::memory_order_acq_rel);
        constexpr ULONGLONG kCoalesceMs = 2500;
        if (prev != 0 && now >= prev && (now - prev) < kCoalesceMs)
        {
            DebugLog(
                DBG_IMPORTANT,
                L"[ExplorerCompat][XamlShutdownMarshal] OnRootBackdropPolicyChanged enabled=0: coalesced skip "
                L"(Δ=%llums)",
                now - prev);
            return;
        }
    }

    EnsureMessageWindow();

    const auto runDirect = [enabled]() noexcept
    {
        if (enabled)
            XamlTapSite::RequestApplyHostBarChromeOnVtcThread();
        else
            XamlTapSite::OnRootBackdropDisabled();
    };

    const HWND h = s_hwndMsg;
    if (h && IsWindow(h))
    {
        const DWORD wndTid = GetWindowThreadProcessId(h, nullptr);
        if (wndTid != 0 && GetCurrentThreadId() != wndTid)
        {
            DWORD_PTR result = 0;
            constexpr UINT kTimeoutMs = 15'000;
            const WPARAM wParam = enabled ? 1u : 0u;
            if (SendMessageTimeoutW(
                    h,
                    ExplorerCompatMsg::WM_GLASS_EXPLORER_ROOT_BACKDROP_POLICY,
                    wParam,
                    0,
                    SMTO_ABORTIFHUNG,
                    kTimeoutMs,
                    &result) != 0)
            {
                DebugLog(
                    DBG_IMPORTANT,
                    L"[ExplorerCompat][XamlShutdownMarshal] RootBackdropPolicy on xaml tid=%u enabled=%d",
                    wndTid,
                    enabled ? 1 : 0);
                return;
            }

            DebugLog(
                DBG_WARNING,
                L"[ExplorerCompat][XamlShutdownMarshal] RootBackdropPolicy SendMessageTimeout failed GLE=%u — повтор 30s",
                static_cast<unsigned>(GetLastError()));
            if (SendMessageTimeoutW(
                    h,
                    ExplorerCompatMsg::WM_GLASS_EXPLORER_ROOT_BACKDROP_POLICY,
                    wParam,
                    0,
                    SMTO_ABORTIFHUNG,
                    30'000,
                    &result) != 0)
            {
                DebugLog(
                    DBG_IMPORTANT,
                    L"[ExplorerCompat][XamlShutdownMarshal] RootBackdropPolicy (повтор) ok tid=%u enabled=%d",
                    wndTid,
                    enabled ? 1 : 0);
                return;
            }

            DebugLog(
                DBG_WARNING,
                L"[ExplorerCompat][XamlShutdownMarshal] RootBackdropPolicy SendMessageTimeout failed GLE=%u — runDirect",
                static_cast<unsigned>(GetLastError()));
        }
    }

    runDirect();
}

void XamlShutdownMarshal::RunPostLiveDisableChromeRedrawOnXamlThread() noexcept
{
    const auto runDirect = []() noexcept
    {
        XamlTapSite::RedrawExplorerShellCaptionAfterLiveOff();
    };

    const HWND h = s_hwndMsg;
    if (h && IsWindow(h))
    {
        const DWORD wndTid = GetWindowThreadProcessId(h, nullptr);
        if (wndTid != 0 && GetCurrentThreadId() != wndTid)
        {
            DWORD_PTR result = 0;
            constexpr UINT kTimeoutMs = 15'000;
            if (SendMessageTimeoutW(
                    h,
                    ExplorerCompatMsg::WM_GLASS_EXPLORER_POST_LIVE_DISABLE_REDRAW,
                    0,
                    0,
                    SMTO_ABORTIFHUNG,
                    kTimeoutMs,
                    &result) != 0)
            {
                DebugLog(
                    DBG_IMPORTANT,
                    L"[ExplorerCompat][XamlShutdownMarshal] PostLiveDisableChromeRedraw on xaml tid=%u",
                    wndTid);
                return;
            }

            DebugLog(
                DBG_WARNING,
                L"[ExplorerCompat][XamlShutdownMarshal] PostLiveDisableChromeRedraw SendMessageTimeout failed "
                L"GLE=%u — прямой вызов",
                static_cast<unsigned>(GetLastError()));
        }
    }

    runDirect();
}

void XamlShutdownMarshal::DestroyMessageWindow() noexcept
{
    const HWND h = s_hwndMsg;
    s_hwndMsg = nullptr;
    if (!h || !IsWindow(h))
        return;

    const DWORD wndTid = GetWindowThreadProcessId(h, nullptr);
    const DWORD curTid = GetCurrentThreadId();
    BOOL destroyed = FALSE;

    if (wndTid != 0 && curTid != wndTid)
    {
        DWORD_PTR result = 0;
        constexpr UINT kTimeoutMs = 5'000;
        if (SendMessageTimeoutW(
                h,
                ExplorerCompatMsg::WM_GLASS_EXPLORER_DESTROY_SHUTDOWN_WND,
                0,
                0,
                SMTO_ABORTIFHUNG,
                kTimeoutMs,
                &result) != 0)
        {
            destroyed = TRUE;
        }
        else
        {
            DebugLog(
                DBG_WARNING,
                L"[ExplorerCompat][XamlShutdownMarshal] DestroyMessageWindow marshal GLE=%u — fallback DestroyWindow",
                static_cast<unsigned>(GetLastError()));
            destroyed = DestroyWindow(h);
        }
    }
    else
    {
        KillTimer(h, XamlShutdownMarshal::kTrayRedrawAfterAdviseTimerId);
        KillTimer(h, XamlShutdownMarshal::kRepatchTimerId);
        destroyed = DestroyWindow(h);
    }

    // Класс держит lpfnWndProc=ShutdownWndProc в USER — снять до FreeLibrary ExplorerCompat.
    HMODULE hMod{};
    if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&XamlShutdownMarshal::DestroyMessageWindow),
            &hMod))
    {
        (void)UnregisterClassW(ExplorerCompatMsg::kExplorerXamlShutdownWndClass, hMod);
    }

    DebugLog(
        DBG_IMPORTANT,
        L"[ExplorerCompat][XamlShutdownMarshal] DestroyMessageWindow: destroyed=%d stillAlive=%d wndTid=%u curTid=%u",
        destroyed ? 1 : 0,
        IsWindow(h) ? 1 : 0,
        static_cast<unsigned>(wndTid),
        static_cast<unsigned>(curTid));
}
