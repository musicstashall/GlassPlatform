// NC surfaces: CoreWindow (SEH/SMEH/Search) + ControlCenterWindow (ShellHost QS).
// Без субкласса HWND — уведомление UICompat (IXDE interest).
#include "WndPch.h"
#include "WinUiBridge/CoreWindowHost.h"

#include <SystemHelper.h>
#include <RootBackdropPolicy.h>
#include <DebugLog.h>
#include <algorithm>
#include <mutex>
#include <vector>

namespace
{
    using UICompatNotifyCoreWindowFn = BOOL(WINAPI*)(HWND);
    using UICompatReleaseInterestFn  = void(WINAPI*)();

    constexpr unsigned kMaxPendingCoreWindows = 16;

    std::mutex                      g_cwMutex;
    std::vector<HWND>               g_coreWindows;
    std::vector<HWND>               g_pendingCoreHwnds;
    UICompatNotifyCoreWindowFn      g_pfnNotifyCoreWindow = nullptr;
    UICompatNotifyCoreWindowFn      g_pfnNotifyShellSurface = nullptr;
    UICompatReleaseInterestFn       g_pfnReleaseInterest  = nullptr;
    bool                            g_loggedMissingUICompat = false;

    [[nodiscard]] bool IsNotificationSurfaceClass(const wchar_t* cls) noexcept
    {
        if (!cls || !cls[0])
            return false;
        if (_wcsicmp(cls, L"Windows.UI.Core.CoreWindow") == 0)
            return Shared::RootBackdropPolicy::IsCoreWindowHostProcess();
        // ShellHost Quick Settings (24H2+): Windhawk NC Styler parity.
        if (_wcsicmp(cls, L"ControlCenterWindow") == 0)
            return Shared::RootBackdropPolicy::IsShellNotificationHostProcess();
        return false;
    }

    [[nodiscard]] bool IsCoreWindowHwnd(HWND hWnd) noexcept
    {
        if (!hWnd || !IsWindow(hWnd))
            return false;
        if (Shared::SystemHelper::g_buildNumber < 22000)
            return false;
        wchar_t cls[96]{};
        if (GetClassNameW(hWnd, cls, static_cast<int>(std::size(cls))) <= 0)
            return false;
        return IsNotificationSurfaceClass(cls);
    }

    void EnsureUICompatLoaded() noexcept
    {
        if (Shared::SystemHelper::g_buildNumber < 22000)
            return;
        if (const HMODULE hGlassCompat = GetModuleHandleW(L"GlassCompat.dll"))
        {
            using EnsureFn = void(WINAPI*)() noexcept;
            if (const auto pfnEnsure = reinterpret_cast<EnsureFn>(
                    GetProcAddress(hGlassCompat, "GlassCompat_EnsureUICompatLoaded")))
            {
                pfnEnsure();
            }
        }
    }

    void RefreshUICompatExportsUnlocked(bool forceReload) noexcept
    {
        if (!forceReload && g_pfnNotifyCoreWindow)
            return;

        const HMODULE hUi = GetModuleHandleW(L"UICompat.dll");
        if (!hUi)
        {
            if (!g_loggedMissingUICompat)
            {
                g_loggedMissingUICompat = true;
                DebugLog(
                    DBG_NORMAL,
                    L"[WndCompat][CoreWindow] UICompat.dll ещё не в процессе — Notify отложен");
            }
            g_pfnNotifyCoreWindow = nullptr;
            g_pfnNotifyShellSurface = nullptr;
            g_pfnReleaseInterest  = nullptr;
            return;
        }

        g_pfnNotifyCoreWindow = reinterpret_cast<UICompatNotifyCoreWindowFn>(
            GetProcAddress(hUi, "UICompat_NotifyCoreWindow"));
        g_pfnNotifyShellSurface = reinterpret_cast<UICompatNotifyCoreWindowFn>(
            GetProcAddress(hUi, "UICompat_NotifyShellSurfaceWindow"));
        g_pfnReleaseInterest = reinterpret_cast<UICompatReleaseInterestFn>(
            GetProcAddress(hUi, "UICompat_ReleaseIxdeSurfaceInterest"));

        if (!g_pfnNotifyCoreWindow)
        {
            if (!g_loggedMissingUICompat)
            {
                g_loggedMissingUICompat = true;
                DebugLog(
                    DBG_WARNING,
                    L"[WndCompat][CoreWindow] GetProcAddress(UICompat_NotifyCoreWindow) failed");
            }
            return;
        }

        g_loggedMissingUICompat = false;
    }

    // Только выбор функции под g_cwMutex. Сам вызов — вне мьютекса: для Start/Search/NC
    // Notify маршалит на UI-поток окна и может блокироваться.
    [[nodiscard]] UICompatNotifyCoreWindowFn SelectNotifyUnlocked() noexcept
    {
        // Start/Search/NC: IXDE на UI-потоке окна (Windhawk).
        if ((Shared::RootBackdropPolicy::IsStartMenuHostProcess()
             || Shared::RootBackdropPolicy::IsSearchHostProcess()
             || Shared::RootBackdropPolicy::IsShellNotificationHostProcess())
            && g_pfnNotifyShellSurface)
        {
            return g_pfnNotifyShellSurface;
        }
        return g_pfnNotifyCoreWindow;
    }

    void CallReleaseInterestUnlocked() noexcept
    {
        if (!g_pfnReleaseInterest)
        {
            const HMODULE hUi = GetModuleHandleW(L"UICompat.dll");
            if (!hUi)
                return;
            g_pfnReleaseInterest = reinterpret_cast<UICompatReleaseInterestFn>(
                GetProcAddress(hUi, "UICompat_ReleaseIxdeSurfaceInterest"));
        }
        if (g_pfnReleaseInterest)
            g_pfnReleaseInterest();
    }

    void QueuePendingUnlocked(HWND hWnd) noexcept
    {
        if (std::find(g_coreWindows.begin(), g_coreWindows.end(), hWnd) != g_coreWindows.end())
            return;
        if (std::find(g_pendingCoreHwnds.begin(), g_pendingCoreHwnds.end(), hWnd)
            != g_pendingCoreHwnds.end())
            return;
        if (g_pendingCoreHwnds.size() >= kMaxPendingCoreWindows)
        {
            DebugLog(
                DBG_WARNING,
                L"[WndCompat][CoreWindow] очередь отложенных CoreWindow полна — hwnd=0x%p",
                hWnd);
            return;
        }
        g_pendingCoreHwnds.push_back(hWnd);
        DebugLog(
            DBG_NORMAL,
            L"[WndCompat][CoreWindow] CoreWindow в очереди до UICompat: hwnd=0x%p",
            hWnd);
    }
}

namespace WndCompat
{
    void CoreWindowHost_Attach(HWND hWnd) noexcept
    {
        if (!IsCoreWindowHwnd(hWnd))
            return;

        EnsureUICompatLoaded();

        UICompatNotifyCoreWindowFn pfnNotify = nullptr;
        {
            std::lock_guard lock(g_cwMutex);
            RefreshUICompatExportsUnlocked(/*forceReload=*/false);
            if (!g_pfnNotifyCoreWindow)
            {
                QueuePendingUnlocked(hWnd);
                return;
            }

            if (std::find(g_coreWindows.begin(), g_coreWindows.end(), hWnd) == g_coreWindows.end())
                g_coreWindows.push_back(hWnd);

            pfnNotify = SelectNotifyUnlocked();
        }

        // Вне g_cwMutex: иначе SendMessage на UI-поток вешал GlassGuiThread и блокировал
        // Detach/DetachAll на стопе (модули оставались загруженными).
        const bool delivered = pfnNotify && pfnNotify(hWnd) != FALSE;

        DebugLog(
            DBG_IMPORTANT,
            L"[WndCompat][CoreWindow] Attach: hwnd=0x%p delivered=%u",
            hWnd,
            delivered ? 1u : 0u);
    }

    void CoreWindowHost_Detach(HWND hWnd) noexcept
    {
        if (!hWnd)
            return;

        bool hadInterest = false;
        {
            std::lock_guard lock(g_cwMutex);
            auto eraseFrom = [](std::vector<HWND>& v, HWND hwnd) noexcept -> bool {
                const auto it = std::find(v.begin(), v.end(), hwnd);
                if (it == v.end())
                    return false;
                v.erase(it);
                return true;
            };
            const bool wasTracked = eraseFrom(g_coreWindows, hWnd);
            (void)eraseFrom(g_pendingCoreHwnds, hWnd);
            hadInterest = wasTracked;
            if (wasTracked)
                CallReleaseInterestUnlocked();
        }

        if (hadInterest)
        {
            DebugLog(
                DBG_NORMAL,
                L"[WndCompat][CoreWindow] Detach: hwnd=0x%p",
                hWnd);
        }
    }

    void CoreWindowHost_DetachAll() noexcept
    {
        std::vector<HWND> copy;
        {
            std::lock_guard lock(g_cwMutex);
            copy.swap(g_coreWindows);
            g_pendingCoreHwnds.clear();
            for (std::size_t i = 0; i < copy.size(); ++i)
                CallReleaseInterestUnlocked();
            g_pfnNotifyCoreWindow = nullptr;
            g_pfnNotifyShellSurface = nullptr;
            g_pfnReleaseInterest  = nullptr;
            g_loggedMissingUICompat = false;
        }
        DebugLog(
            DBG_IMPORTANT,
            L"[WndCompat][CoreWindow] DetachAll: count=%zu",
            copy.size());
    }

    void CoreWindowHost_FlushPendingAfterUICompatReady() noexcept
    {
        std::vector<HWND> pending;
        {
            std::lock_guard lock(g_cwMutex);
            RefreshUICompatExportsUnlocked(/*forceReload=*/true);
            if (!g_pfnNotifyCoreWindow)
                return;
            pending.swap(g_pendingCoreHwnds);
        }

        for (HWND hwnd : pending)
        {
            if (hwnd && IsWindow(hwnd))
                CoreWindowHost_Attach(hwnd);
        }

        if (!pending.empty())
        {
            DebugLog(
                DBG_IMPORTANT,
                L"[WndCompat][CoreWindow] FlushPending: tried=%zu",
                pending.size());
        }
    }

    // Опасно на cold start: Attach+TAP по dormant HWND может вызвать SEH/VEH AV.
    // Экспорт оставлен; вызовы из DoInitialize/UICompat_Initialize сняты.
    void CoreWindowHost_AttachExistingInProcess() noexcept
    {
        if (!Shared::RootBackdropPolicy::IsCoreWindowHostProcess())
            return;

        struct EnumCtx
        {
            DWORD pid;
            HWND hwnds[16];
            int heights[16];
            unsigned count;
        } ctx{ GetCurrentProcessId(), {}, {}, 0 };

        EnumWindows(
            [](HWND hwnd, LPARAM lp) noexcept -> BOOL {
                auto* c = reinterpret_cast<EnumCtx*>(lp);
                if (c->count >= 16u)
                    return FALSE;
                DWORD pid = 0;
                GetWindowThreadProcessId(hwnd, &pid);
                if (pid != c->pid)
                    return TRUE;
                wchar_t cls[96]{};
                if (GetClassNameW(hwnd, cls, 96) <= 0)
                    return TRUE;
                // CoreWindow (SEH) или ControlCenterWindow (ShellHost QS).
                if (_wcsicmp(cls, L"Windows.UI.Core.CoreWindow") != 0
                    && _wcsicmp(cls, L"ControlCenterWindow") != 0)
                    return TRUE;
                RECT rc{};
                const int cy = GetClientRect(hwnd, &rc) ? (rc.bottom - rc.top) : 0;
                c->hwnds[c->count] = hwnd;
                c->heights[c->count] = cy;
                ++c->count;
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&ctx));

        // SEH: высокая NC-панель (cy≥600) раньше toast/1×1 — иначе IXDE уходит в Defer
        // и pre-show CatchUp не стартует (бэкапы HR16 / план nc-preshow).
        if (Shared::RootBackdropPolicy::IsShellExperienceHostProcess() && ctx.count > 1u)
        {
            for (unsigned i = 0; i + 1u < ctx.count; ++i)
            {
                unsigned best = i;
                for (unsigned j = i + 1u; j < ctx.count; ++j)
                {
                    if (ctx.heights[j] > ctx.heights[best])
                        best = j;
                }
                if (best != i)
                {
                    const HWND th = ctx.hwnds[i];
                    const int ty = ctx.heights[i];
                    ctx.hwnds[i] = ctx.hwnds[best];
                    ctx.heights[i] = ctx.heights[best];
                    ctx.hwnds[best] = th;
                    ctx.heights[best] = ty;
                }
            }
        }

        unsigned tall = 0;
        unsigned shortH = 0;
        for (unsigned i = 0; i < ctx.count; ++i)
        {
            if (ctx.heights[i] >= 600)
                ++tall;
            else
                ++shortH;
        }

        unsigned attached = 0;
        for (unsigned i = 0; i < ctx.count; ++i)
        {
            if (ctx.hwnds[i] && IsWindow(ctx.hwnds[i]))
            {
                CoreWindowHost_Attach(ctx.hwnds[i]);
                ++attached;
            }
        }

        if (attached != 0)
        {
            DebugLog(
                DBG_IMPORTANT,
                L"[WndCompat][CoreWindow] AttachExisting: found=%u attached=%u "
                L"tall=%u short=%u firstCy=%d",
                ctx.count,
                attached,
                tall,
                shortH,
                ctx.count ? ctx.heights[0] : -1);
        }
    }

    void CoreWindowHost_OnIxdeReady() noexcept
    {
        std::vector<HWND> tracked;
        {
            std::lock_guard lock(g_cwMutex);
            RefreshUICompatExportsUnlocked(/*forceReload=*/true);
            if (!g_pfnNotifyCoreWindow)
                return;
            tracked = g_coreWindows;
            for (HWND hwnd : g_pendingCoreHwnds)
            {
                if (std::find(tracked.begin(), tracked.end(), hwnd) == tracked.end())
                    tracked.push_back(hwnd);
            }
            g_pendingCoreHwnds.clear();
        }

        unsigned notified = 0;
        for (HWND hwnd : tracked)
        {
            if (!hwnd || !IsWindow(hwnd))
                continue;
            CoreWindowHost_Attach(hwnd);
            ++notified;
        }

        DebugLog(
            DBG_IMPORTANT,
            L"[WndCompat][CoreWindow] OnIxdeReady: renotify=%u",
            notified);
    }

    void CoreWindowHost_ResetUICompatNotifyProcCache() noexcept
    {
        std::lock_guard lock(g_cwMutex);
        g_pfnNotifyCoreWindow = nullptr;
        g_pfnNotifyShellSurface = nullptr;
        g_pfnReleaseInterest  = nullptr;
        g_loggedMissingUICompat = false;
    }
}
