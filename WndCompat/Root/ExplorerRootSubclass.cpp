// WndCompat/Root/ExplorerRootSubclass.cpp — только shell-root проводника (CabinetWClass / ExploreWClass, GA_ROOT).
// Без общего RootSubclassProcShared: нет WM_CTLCOLOR*, классического menu bar, ComboLBox CBN_DROPDOWN, WM_ERASEBKGND и т.д.
#include "WndPch.h"
#include "Root/ExplorerRootSubclass.h"
#include "Root/RootSubclass.h"
#include "Root/RootSubclass.h"
#include <Helpers/RootNcActiveCache.h>
#include "Root/RootExplorerSubclassBridge.h"

#include <GlassComctlScrollHost.h>
#include <GlassControlKind.h>
#include <GlassHostRegistry.h>
#include <GlassMessages.h>
#include <InjectionMode.h>
#include <RootBackdropPolicy.h>
#include <GlassBackdropMark.h>
#include <RootPolicyQuery.h>
#include <SystemHelper.h>
#include <RootEventProvider.h>
#include "Tracker/WinEventHandler.h"
#include "Tracker/BackdropPolicyRescan.h"
#include <WndCompat.h>
#include "Root/RootBackdropRefreshThrottle.h"

#include <dwmapi.h>

#include <mutex>
#include <string>
#include <unordered_map>

#pragma comment(lib, "dwmapi.lib")

namespace
{
    constexpr wchar_t kExplorerShowBackdropRefreshProp[] = L"GlassPlatform.ExplorerShowBackdropRefresh";

    using GlassCoreRemoveRootSwcaCacheFn = void(WINAPI*)(HWND) noexcept;

    [[nodiscard]] GlassCoreRemoveRootSwcaCacheFn GetRemoveRootSwcaApplyCacheEntryFn() noexcept
    {
        static const GlassCoreRemoveRootSwcaCacheFn pfn = []() noexcept -> GlassCoreRemoveRootSwcaCacheFn {
            const HMODULE hCore = GetModuleHandleW(L"GlassCore.dll");
            if (!hCore)
                return nullptr;
            return reinterpret_cast<GlassCoreRemoveRootSwcaCacheFn>(
                GetProcAddress(hCore, "GlassCore_RemoveRootSwcaApplyCacheEntry"));
        }();
        return pfn;
    }

    void GlassCore_TabChroma_RefreshFromTheme(HWND themeAnchorOrNull) noexcept
    {
        if (auto* hp = WndCompat::Root::GetRootEventProvider())
        {
            if (hp->OnRootWmThemeChangedForTabChroma)
                hp->OnRootWmThemeChangedForTabChroma(themeAnchorOrNull);
        }
    }

    [[nodiscard]] bool CanRefreshExplorerShellBackdrop(HWND hWnd) noexcept
    {
        if (!hWnd || !IsWindow(hWnd))
            return false;
        if (WndCompat::IsShuttingDown())
            return false;
        if (GetAncestor(hWnd, GA_ROOT) != hWnd)
            return false;
        if (WndCompat::WinEventHook_GetMode() != Shared::InjectionMode::Full)
            return false;
        if (!Shared::RootPolicyQuery::UsesSwcaBackdrop())
            return false;
        return Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(hWnd);
    }

    void ApplyExplorerShellBackdropRefresh(HWND hWnd, const wchar_t* reason) noexcept
    {
        if (const auto pfnCache = GetRemoveRootSwcaApplyCacheEntryFn())
            pfnCache(hWnd);

        const bool isActive = Shared::RootNcActiveCache::Get(hWnd);

        if (auto* provider = WndCompat::Root::GetRootEventProvider())
        {
            WndCompat::RootBackdropRefreshThrottle::SetRootSwcaApplyReasonForNextCall(reason);
            if (isActive && provider->OnRootSwcaBackdropActiveApply)
                provider->OnRootSwcaBackdropActiveApply(hWnd);
            else if (!isActive && provider->OnRootSwcaBackdropInactiveApply)
                provider->OnRootSwcaBackdropInactiveApply(hWnd);
            else if (provider->OnRootSwcaBackdropActiveApply)
                provider->OnRootSwcaBackdropActiveApply(hWnd);
        }

        LRESULT chromeLr{};
        (void)WndCompat::ExplorerRoot::TryHandleChromeRedraw(hWnd, chromeLr);
        WndCompat::ExplorerRoot::PostExplorerChromeRedrawIfShellRoot(hWnd);
    }

    /// WM_SHOWWINDOW: один раз (prop). Не вызывать синхронно из Attach при IsWindowVisible — WinUI ещё не готов.
    void TryRefreshExplorerShellBackdropOnFirstShow(HWND hWnd, const wchar_t* reason) noexcept
    {
        if (!CanRefreshExplorerShellBackdrop(hWnd))
            return;
        if (GetPropW(hWnd, kExplorerShowBackdropRefreshProp) != nullptr)
            return;

        SetPropW(hWnd, kExplorerShowBackdropRefreshProp, reinterpret_cast<HANDLE>(1));
        ApplyExplorerShellBackdropRefresh(hWnd, reason);
    }

    /// Единственный SWCA на Attach (без второго прохода — иначе мерцание SetWindowBackdrop).
    void ApplyExplorerShellBackdropAtAttach(HWND hWnd) noexcept
    {
        if (!CanRefreshExplorerShellBackdrop(hWnd))
            return;

        ApplyExplorerShellBackdropRefresh(hWnd, L"ATTACH");
        SetPropW(hWnd, kExplorerShowBackdropRefreshProp, reinterpret_cast<HANDLE>(1));
    }

    /// Отложенный путь (PostMessage с других мест); на Attach не используем.
    void ForceRefreshExplorerShellBackdropPostAttach(HWND hWnd) noexcept
    {
        if (!CanRefreshExplorerShellBackdrop(hWnd))
            return;

        ApplyExplorerShellBackdropRefresh(hWnd, L"POST_ATTACH");
        SetPropW(hWnd, kExplorerShowBackdropRefreshProp, reinterpret_cast<HANDLE>(1));
    }
} // namespace

// 1 — MessageBox после успешного SetWindowSubclass (только отладка).
#ifndef GLASS_DIAG_EXPLORER_ROOT_ATTACH_MSGBOX
#define GLASS_DIAG_EXPLORER_ROOT_ATTACH_MSGBOX 0
#endif

#ifndef WM_DWMCOLORIZATIONCHANGED
#define WM_DWMCOLORIZATIONCHANGED 0x0320
#endif

#ifndef DWMWA_EXTENDED_FRAME_BOUNDS
#define DWMWA_EXTENDED_FRAME_BOUNDS 9
#endif

namespace
{
    [[nodiscard]] bool ExplorerRootIsVisuallyActive(HWND hRoot) noexcept
    {
        const HWND fg = GetForegroundWindow();
        if (!fg)
            return false;
        const HWND rWnd = GetAncestor(hRoot, GA_ROOT);
        const HWND rFg = GetAncestor(fg, GA_ROOT);
        return rWnd && rWnd == rFg;
    }

    void RefreshExplorerRootBackdropThrottled(HWND hWnd) noexcept
    {
        DebugLog(
            L"[WndCompat][ExplorerRoot] RefreshExplorerRootBackdropThrottled: enter hwnd=0x%p",
            hWnd);

        if (!Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(hWnd))
        {
            DebugLog(
                L"[WndCompat][ExplorerRoot] RefreshExplorerRootBackdropThrottled: skip — not FileExplorerShellRoot hwnd=0x%p",
                hWnd);
            return;
        }
        if (!Shared::RootPolicyQuery::UsesSwcaBackdrop())
        {
            DebugLog(
                L"[WndCompat][ExplorerRoot] RefreshExplorerRootBackdropThrottled: skip — DWM policy (SWCA only) hwnd=0x%p",
                hWnd);
            return;
        }
        if (WndCompat::RootBackdropRefreshThrottle::ShouldSkip(hWnd))
        {
            DebugLog(
                L"[WndCompat][ExplorerRoot] RefreshExplorerRootBackdropThrottled: skip — throttled (%ums) hwnd=0x%p",
                static_cast<unsigned>(WndCompat::RootBackdropRefreshThrottle::kDefaultThrottleMs),
                hWnd);
            return;
        }

        auto* p = WndCompat::Root::GetRootEventProvider();
        if (!p)
        {
            DebugLog(
                L"[WndCompat][ExplorerRoot] RefreshExplorerRootBackdropThrottled: skip — GetRootEventProvider() null hwnd=0x%p",
                hWnd);
            return;
        }

        const bool hasSwcaAct = (p->OnRootSwcaBackdropActiveApply != nullptr);
        const bool hasSwcaInact = (p->OnRootSwcaBackdropInactiveApply != nullptr);
        const bool visuallyActive = ExplorerRootIsVisuallyActive(hWnd);

        DebugLog(
            L"[WndCompat][ExplorerRoot] RefreshExplorerRootBackdropThrottled: apply hwnd=0x%p "
            L"visuallyActive=%d SwcaActive=%d SwcaInactive=%d WinEventMode=%u",
            hWnd,
            visuallyActive ? 1 : 0,
            hasSwcaAct ? 1 : 0,
            hasSwcaInact ? 1 : 0,
            static_cast<unsigned>(WndCompat::WinEventHook_GetMode()));

        WndCompat::RootBackdropRefreshThrottle::SetRootSwcaApplyReasonForNextCall(L"EXPLORER_REFRESH");

        if (visuallyActive)
        {
            if (p->OnRootSwcaBackdropActiveApply)
                p->OnRootSwcaBackdropActiveApply(hWnd);
            else
                DebugLog(
                    L"[WndCompat][ExplorerRoot] RefreshExplorerRootBackdropThrottled: "
                    L"WARN OnRootSwcaBackdropActiveApply null (active) hwnd=0x%p",
                    hWnd);
        }
        else
        {
            if (p->OnRootSwcaBackdropInactiveApply)
                p->OnRootSwcaBackdropInactiveApply(hWnd);
            else
                DebugLog(
                    L"[WndCompat][ExplorerRoot] RefreshExplorerRootBackdropThrottled: "
                    L"WARN OnRootSwcaBackdropInactiveApply null (inactive) hwnd=0x%p",
                    hWnd);
        }

        WndCompat::RootBackdropRefreshThrottle::Stamp(hWnd);

        DebugLog(
            L"[WndCompat][ExplorerRoot] RefreshExplorerRootBackdropThrottled: done hwnd=0x%p",
            hWnd);
    }

    void RefreshExplorerRootBackdropImmediate(HWND hWnd) noexcept
    {
        if (!Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(hWnd))
            return;
        if (!Shared::RootPolicyQuery::UsesSwcaBackdrop())
            return;
        auto* p = WndCompat::Root::GetRootEventProvider();
        if (!p)
            return;

        const bool visuallyActive = ExplorerRootIsVisuallyActive(hWnd);
        WndCompat::RootBackdropRefreshThrottle::SetRootSwcaApplyReasonForNextCall(L"EXPLORER_REFRESH_IMMEDIATE");
        if (visuallyActive)
        {
            if (p->OnRootSwcaBackdropActiveApply)
                p->OnRootSwcaBackdropActiveApply(hWnd);
        }
        else
        {
            if (p->OnRootSwcaBackdropInactiveApply)
                p->OnRootSwcaBackdropInactiveApply(hWnd);
        }
    }

    using DwmUpdateAccentBlurRectFn = HRESULT(WINAPI*)(HWND, RECT*);

    [[nodiscard]] DwmUpdateAccentBlurRectFn GetDwmUpdateAccentBlurRect() noexcept
    {
        static const DwmUpdateAccentBlurRectFn fn =
            reinterpret_cast<DwmUpdateAccentBlurRectFn>(
                GetProcAddress(GetModuleHandleW(L"dwmapi.dll"), MAKEINTRESOURCEA(159)));
        return fn;
    }
}

namespace WndCompat
{
    LRESULT CALLBACK ExplorerRootSubclassProc(
        HWND hWnd,
        UINT uMsg,
        WPARAM wParam,
        LPARAM lParam,
        UINT_PTR uIdSubclass,
        DWORD_PTR /*dwRefData*/)
    {
        if (uMsg == WndCompat::WM_GLASS_DETACH_ROOT)
        {
            Root::ExplorerBridge::OnDetachRoot(hWnd, ExplorerRootSubclassProc, uIdSubclass);
            WndCompat::WinEventHook_SignalDetachComplete();
            return 0;
        }

        if (uMsg == WndCompat::WM_GLASS_REAPPLY_ROOT_BACKDROP)
        {
            WndCompat::ApplyRootBackdropReapplyOnOwnerThread(hWnd);
            return 0;
        }

        // До guard IsShuttingDown:
        // а после установки shuttingDown шапка не получает Redraw/SWCA (остаются «битые» цвета).
        if (uMsg == ExplorerCompatMsg::WM_GLASS_EXPLORER_CHROME_REDRAW)
        {
            LRESULT chromeLr{};
            if (ExplorerRoot::TryHandleChromeRedraw(hWnd, chromeLr))
                return chromeLr;
        }

        if (uMsg == ExplorerCompatMsg::WM_GLASS_EXPLORER_ROOT_FULL_REDRAW)
        {
            ExplorerRoot::RedrawExplorerShellRootFull(hWnd);
            return 0;
        }

        if (uMsg == ExplorerCompatMsg::WM_GLASS_EXPLORER_SHELL_BACKDROP_REFRESH)
        {
            ForceRefreshExplorerShellBackdropPostAttach(hWnd);
            return 0;
        }

        if (uMsg == WndCompat::WM_GLASS_DEFER_THEMED_CHROME_INVALIDATE)
        {
            WndCompat::RunDeferredThemedChromeSoftInvalidateWithGlassCore(hWnd);
            return 0;
        }

        if (uMsg == WndCompat::WM_GLASS_DEFER_TEXT_GLOW_INVALIDATE)
        {
            WndCompat::RunDeferredTextGlowSoftInvalidateWithGlassCore(hWnd);
            return 0;
        }

        if (uMsg == WndCompat::WM_GLASS_DEFER_TEXT_INK_INVALIDATE)
        {
            WndCompat::RunDeferredTextInkSoftInvalidateWithGlassCore(hWnd);
            return 0;
        }

        if (IsShuttingDown())
            return DefSubclassProc(hWnd, uMsg, wParam, lParam);

        switch (uMsg)
        {
        case WM_THEMECHANGED:
            GlassCore_TabChroma_RefreshFromTheme(nullptr);
            break;

        case WM_SHOWWINDOW:
            if (wParam)
            {
                TryRefreshExplorerShellBackdropOnFirstShow(hWnd, L"SHOWWINDOW");
                // Seed актуального nc-состояния при показе (симметрия RootSubclassProc).
                const bool showActive = ExplorerRootIsVisuallyActive(hWnd);
                Shared::RootNcActiveCache::Set(hWnd, showActive);
                WndCompat::FinalizeGlassCoreRootActivationVisualUpdate(hWnd, showActive ? TRUE : FALSE);
            }
            break;

        case WM_NCACTIVATE:
        {
            const BOOL ncActive = (wParam != FALSE);
            const bool wasNcActive = Shared::RootNcActiveCache::Get(hWnd);
            const bool nowNcActive = (ncActive != FALSE);
            const bool ncStateChanged = (wasNcActive != nowNcActive);

            const LRESULT lr = DefSubclassProc(hWnd, uMsg, wParam, lParam);

            if (ncStateChanged)
                Shared::RootNcActiveCache::Set(hWnd, nowNcActive);

            // Win11: backdrop transition — IAT CAPTION_COLOR / hooks, не NCACTIVATE reapply.
            WndCompat::FinalizeGlassCoreRootActivationVisualUpdate(hWnd, ncActive);
            return lr;
        }

        case WM_DWMCOLORIZATIONCHANGED:
        {
            if (WinEventHook_GetMode() != Shared::InjectionMode::Full)
                break;
            if (auto* hp = Root::GetRootEventProvider())
            {
                if (hp->OnRootDwmColorizationChanged)
                    hp->OnRootDwmColorizationChanged(hWnd, wParam, lParam);
            }
            WndCompat::PostDwmColorizationDispatch(wParam);
            break;
        }

        //// Как в доноре (FileBrowsWindowPatch): отдельного re-apply здесь нет.
        //// Эффект проводника переустанавливается в WM_NCPAINT.
        //case WM_NCACTIVATE:
        //{
        //    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
        //}

        // Как FileBrowsWindowPatch HookManager::SetWindowBlur на WM_NCPAINT: сначала backdrop (DWM/SWCA),
        // затем DefSubclassProc. Если SWCA после отрисовки NC — на Win11 часто пропадают системные кнопки.
        case WM_NCPAINT:
        {
            // SWCA для shell-root: IAT DwmExtendFrameIntoClientArea (ExplorerFrameHooks) — apply до orig; здесь refresh не нужен.
#if 0
            if (WinEventHook_GetMode() == Shared::InjectionMode::Full
                && Shared::RootPolicyQuery::UsesSwcaBackdrop())
            {
                // В стиле донора (FileBrowsWindowPatch): в NC-проходе применяем эффект без троттлинга.
                RefreshExplorerRootBackdropImmediate(hWnd);
            }
            else if (WinEventHook_GetMode() == Shared::InjectionMode::Full)
            {
                DebugLog(
                    L"[WndCompat][ExplorerRoot] WM_NCPAINT: skip backdrop refresh — DWM policy hwnd=0x%p",
                    hWnd);
            }
            else
            {
                DebugLog(
                    L"[WndCompat][ExplorerRoot] WM_NCPAINT: skip refresh — not Full mode (mode=%u) hwnd=0x%p",
                    static_cast<unsigned>(WinEventHook_GetMode()),
                    hWnd);
            }
#endif
            return DefSubclassProc(hWnd, uMsg, wParam, lParam);
        }

        // FileBrows: DwmUpdateAccentBlurRect (dwmapi ord 159) при смене позиции/размера.
        /*case WM_WINDOWPOSCHANGED:
        {
            const LRESULT lr = DefSubclassProc(hWnd, uMsg, wParam, lParam);
            if (WinEventHook_GetMode() == Shared::InjectionMode::Full
                && Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(hWnd))
            {
                if (const auto fn = GetDwmUpdateAccentBlurRect())
                {
                    RECT rc{-1, 0, 0, 0};
                    if (!IsZoomed(hWnd) && lParam)
                    {
                        if (SUCCEEDED(DwmGetWindowAttribute(
                                hWnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rc, sizeof(rc))))
                        {
                            OffsetRect(&rc, -rc.left, -rc.top);
                            const auto* pos = reinterpret_cast<const WINDOWPOS*>(lParam);
                            const int width = (pos->cx - (rc.right - rc.left)) / 2;
                            rc.left += width;
                            rc.right += width;
                        }
                    }
                    (void)fn(hWnd, &rc);
                }
            }
            return lr;
        }*/

        case WM_NCDESTROY:
            Shared::RootNcActiveCache::Remove(hWnd);
            WndCompat::RemoveGlassCoreThemePaintRootNcState(hWnd);
            WndCompat::RemoveTextGlowClientsForRootWithGlassCore(hWnd);
            WndCompat::RemoveTextInkClientsForRootWithGlassCore(hWnd);
            WndCompat::RootBackdropRefreshThrottle::Erase(hWnd);
            Root::ExplorerBridge::OnRootAttachedWndNcDestroy(hWnd);
            break;

        default:
            break;
        }

        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }
}

namespace WndCompat::ExplorerRoot
{
    bool ExplorerRoot_ForceDetachSubclass(HWND hWnd) noexcept
    {
        return RemoveWindowSubclass(hWnd, ExplorerRootSubclassProc, ExplorerRootSubclassId) != FALSE;
    }

    // #region agent log
    // GLASS-ULTRACE (H91): RVA прокa для сверки с адресом из дампа.
    std::size_t ExplorerRoot_SubclassProcRva() noexcept
    {
        HMODULE self = nullptr;
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&ExplorerRoot_ForceDetachSubclass), &self) || !self)
            return 0;
        const auto base = reinterpret_cast<std::uintptr_t>(self);
        const auto proc = reinterpret_cast<std::uintptr_t>(&ExplorerRootSubclassProc);
        return proc >= base ? static_cast<std::size_t>(proc - base) : 0;
    }
    // #endregion

    void Attach(HWND hRoot)
    {
        if (!IsWindow(hRoot))
            return;
        if (IsShuttingDown())
            return;
        if (!Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(hRoot))
            return;

        wchar_t rootClassName[256]{};
        if (GetClassNameW(hRoot, rootClassName, static_cast<int>(_countof(rootClassName))) <= 0)
            rootClassName[0] = L'\0';

        if (wcscmp(rootClassName, ExplorerCompatMsg::kExplorerXamlShutdownWndClass) == 0)
            return;
        if (wcscmp(rootClassName, TaskbarCompatMsg::kTaskbarXamlShutdownWndClass) == 0)
            return;

        // ExplorerCompat только Win11 + процесс с CabinetWClass/ExploreWClass.
        if (Shared::SystemHelper::g_buildNumber >= 22000)
        {
            if (const HMODULE hGlassCompat = GetModuleHandleW(L"GlassCompat.dll"))
            {
                using EnsureFn = void(WINAPI*)() noexcept;
                if (const auto pfnEnsure = reinterpret_cast<EnsureFn>(
                        GetProcAddress(hGlassCompat, "GlassCompat_EnsureExplorerCompatLoaded")))
                {
                    pfnEnsure();
                }
            }
        }

        if (!Root::ExplorerBridge::TryRegisterRootWindowForAttach(hRoot))
            return;

        GlassShared::GlassMarkRootBackdropAllowed(
            hRoot, Shared::RootPolicyQuery::IsRootGlassRenderingEnabled());

        if (!SetWindowSubclass(hRoot, ExplorerRootSubclassProc, ExplorerRootSubclassId, 0))
        {
            Root::ExplorerBridge::UnregisterRootWindowAfterSubclassFailed(hRoot);
            DebugLog(std::format(
                L"[WndCompat][ExplorerRoot] Attach: FAILED to subclass 0x{:X} class=\"{}\"",
                reinterpret_cast<std::uintptr_t>(hRoot),
                rootClassName
            ).c_str());
            return;
        }

        if (WinEventHook_GetMode() == Shared::InjectionMode::Full
            && Root::ExplorerBridge::ShouldSetComctlScrollHostForRootClassName(rootClassName))
        {
            GlassShared::GlassMarkComctlScrollHost(hRoot);
        }

        if (Shared::RootPolicyQuery::IsRootGlassRenderingEnabled())
            Root::ExplorerBridge::ApplyRootClassBlackBrush(hRoot);

        if (auto* hp = Root::GetRootEventProvider())
        {
            if (hp->OnRootHooksAttach)
                hp->OnRootHooksAttach(hRoot);
        }

        if (GetAncestor(hRoot, GA_ROOT) == hRoot
            && GlassShared::GlassIsBackdropAllowedForHwnd(hRoot))
        {
            auto provider = Root::GetRootEventProvider();
            if (provider)
            {
                if (Shared::RootPolicyQuery::UsesSwcaBackdrop())
                    ApplyExplorerShellBackdropAtAttach(hRoot);
                else if (provider->OnRootDwmBackdropApply)
                    provider->OnRootDwmBackdropApply(hRoot);
            }

            // Seed стартового nc-состояния для DTB-хрома (см. Root_Attach в RootSubclass.cpp).
            const bool attachActive = ExplorerRootIsVisuallyActive(hRoot);
            Shared::RootNcActiveCache::Set(hRoot, attachActive);
            WndCompat::FinalizeGlassCoreRootActivationVisualUpdate(hRoot, attachActive ? TRUE : FALSE);
        }

        DebugLog(std::format(
            L"[WndCompat][ExplorerRoot] Attach: subclassed 0x{:X} class=\"{}\"",
            reinterpret_cast<std::uintptr_t>(hRoot),
            rootClassName
        ).c_str());

        if (Shared::RootBackdropPolicy::IsHwndBackdropExempt(hRoot))
            Shared::GlassHostRegistry::MarkSubtreeGlassDisabled(hRoot);

        WndCompat::BackdropPolicyRescan::ScheduleSubtreeRescanAfterRootAttach(hRoot);

        if (Shared::RootPolicyQuery::IsRootGlassRenderingEnabled()
            && GlassShared::GlassIsBackdropAllowedForHwnd(hRoot))
        {
            RedrawExplorerShellRootFull(hRoot);
        }
    }

    void Detach(HWND hRoot)
    {
        if (!IsWindow(hRoot))
            return;
        if (!Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(hRoot))
            return;

        if (!Root::ExplorerBridge::RemoveTrackedRootAndDecrementBrush(hRoot))
            return;

        if (auto* hp = Root::GetRootEventProvider())
        {
            if (hp->OnRootHooksDetach)
                hp->OnRootHooksDetach(hRoot);
        }

        const DWORD curTid = GetCurrentThreadId();
        const DWORD wndTid = GetWindowThreadProcessId(hRoot, nullptr);

        if (wndTid != curTid)
        {
            DebugLog(DBG_WARNING,
                L"[WndCompat][ExplorerRoot] Detach: cross-thread hwnd=0x%p — skip RemoveWindowSubclass",
                hRoot);
            return;
        }

        if (!RemoveWindowSubclass(hRoot, ExplorerRootSubclassProc, ExplorerRootSubclassId))
        {
            DebugLog(std::format(
                L"[WndCompat][ExplorerRoot] Detach: RemoveWindowSubclass FAILED for 0x{:X}",
                reinterpret_cast<std::uintptr_t>(hRoot)
            ).c_str());
        }
        else
        {
            RemovePropW(hRoot, kExplorerShowBackdropRefreshProp);
            GlassShared::GlassUnmarkWindow(hRoot);
            GlassShared::GlassClearComctlScrollHost(hRoot);
            DebugLog(std::format(
                L"[WndCompat][ExplorerRoot] Detach: 0x{:X}",
                reinterpret_cast<std::uintptr_t>(hRoot)
            ).c_str());
        }
    }

    void DetachAll(
        const std::vector<HWND>& windows,
        const std::unordered_map<ATOM, WndCompat::Root::ExplorerBridge::ClassBrushEntry>& classBrushes,
        DWORD curTid)
    {
        for (HWND hWnd : windows)
        {
            if (!IsWindow(hWnd))
                continue;
            if (!Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(hWnd))
                continue;

#if GLASS_DIAG_EXPLORER_USE_ROOT_SUBCLASS
            // Проводник на RootSubclassProc — снимает Root_DetachAll, не дублируем hooks/detach.
            continue;
#endif

            const DWORD wndTid = GetWindowThreadProcessId(hWnd, nullptr);
            const bool isRoot = (GetAncestor(hWnd, GA_ROOT) == hWnd);

            if (wndTid != curTid)
            {
                WinEventHook_RegisterPendingDetach();
                BOOL posted = PostMessageW(hWnd, WndCompat::WM_GLASS_DETACH_ROOT, 0, 0);
                if (!posted)
                {
                    // Окно уже уничтожено — откатываем счётчик, чтобы не зависнуть на WaitForSingleObject.
                    WinEventHook_SignalDetachComplete();
                    DebugLog(std::format(
                        L"[WndCompat][ExplorerRoot] DetachAll: PostMessageW FAILED for 0x{:X} (window destroyed?)",
                        reinterpret_cast<std::uintptr_t>(hWnd)
                    ).c_str());
                }

                Root::ExplorerBridge::RestoreOriginalClassBrush(hWnd, classBrushes);

                if (isRoot)
                {
                    const bool hadGlassBackdrop =
                        Shared::RootBackdropPolicy::DidGlassApplyRootBackdrop(hWnd);
                    if (hadGlassBackdrop)
                        Root::ExplorerBridge::RestoreRootNcAfterDetach(hWnd);
                }
                continue;
            }

            if (auto* hp = Root::GetRootEventProvider())
            {
                if (hp->OnRootHooksDetach)
                    hp->OnRootHooksDetach(hWnd);
            }

            if (!RemoveWindowSubclass(hWnd, ExplorerRootSubclassProc, ExplorerRootSubclassId))
            {
                DebugLog(std::format(
                    L"[WndCompat][ExplorerRoot] DetachAll: RemoveWindowSubclass FAILED for 0x{:X}",
                    reinterpret_cast<std::uintptr_t>(hWnd)
                ).c_str());
            }
            else
            {
                DebugLog(std::format(
                    L"[WndCompat][ExplorerRoot] DetachAll: detached 0x{:X}",
                    reinterpret_cast<std::uintptr_t>(hWnd)
                ).c_str());
            }

            Root::ExplorerBridge::RestoreOriginalClassBrush(hWnd, classBrushes);

            if (isRoot)
            {
                const bool hadGlassBackdrop =
                    Shared::RootBackdropPolicy::DidGlassApplyRootBackdrop(hWnd);
                if (hadGlassBackdrop)
                    Root::ExplorerBridge::RestoreRootNcAfterDetach(hWnd);
            }
        }
    }

    bool TryHandleChromeRedraw(HWND hWnd, LRESULT& outLr) noexcept
    {
        if (!Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(hWnd))
            return false;

        if (WndCompat::WinEventHook_GetMode() != Shared::InjectionMode::Full)
        {
            outLr = 0;
            return true;
        }

        // Repaint-only: рамку и SBT восстанавливает RestoreWin11ExplorerStockFrame.
        RedrawWindow(hWnd, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_UPDATENOW);
        SendMessageW(hWnd, WM_NCPAINT, 1, 0);

        outLr = 0;
        return true;
    }

    void PostExplorerChromeRedrawIfShellRoot(HWND hRoot) noexcept
    {
        if (!hRoot || !IsWindow(hRoot))
            return;
        if (!Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(hRoot))
            return;
        if (WndCompat::WinEventHook_GetMode() != Shared::InjectionMode::Full)
            return;

        (void)PostMessageW(hRoot, ExplorerCompatMsg::WM_GLASS_EXPLORER_CHROME_REDRAW, 0, 0);
    }

    void RedrawExplorerShellRootFull(HWND hRoot) noexcept
    {
        if (!hRoot || !IsWindow(hRoot))
            return;
        if (WndCompat::IsShuttingDown())
            return;
        if (!Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(hRoot))
            return;
        if (GetAncestor(hRoot, GA_ROOT) != hRoot)
            return;
        if (WndCompat::WinEventHook_GetMode() != Shared::InjectionMode::Full)
            return;

        RedrawWindow(hRoot, nullptr, nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_ERASENOW | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_FRAME);
    }

    namespace
    {
        BOOL CALLBACK EnumSyncChromeRedraw(HWND top, LPARAM lParam) noexcept
        {
            auto* sent = reinterpret_cast<unsigned*>(lParam);
            DWORD pid = 0;
            if (!GetWindowThreadProcessId(top, &pid) || pid != GetCurrentProcessId())
                return TRUE;
            wchar_t cls[96]{};
            if (GetClassNameW(top, cls, static_cast<int>(std::size(cls))) <= 0)
                return TRUE;
            if (wcscmp(cls, L"CabinetWClass") != 0 && wcscmp(cls, L"ExploreWClass") != 0)
                return TRUE;

            constexpr UINT kTimeoutMs = 5000;
            DWORD_PTR smResult = 0;
            if (SendMessageTimeoutW(
                    top,
                    ExplorerCompatMsg::WM_GLASS_EXPLORER_CHROME_REDRAW,
                    0,
                    0,
                    SMTO_ABORTIFHUNG,
                    kTimeoutMs,
                    &smResult) != 0)
            {
                ++*sent;
            }
            return TRUE;
        }

        BOOL CALLBACK EnumSyncRootFullRedraw(HWND top, LPARAM lParam) noexcept
        {
            auto* sent = reinterpret_cast<unsigned*>(lParam);
            DWORD pid = 0;
            if (!GetWindowThreadProcessId(top, &pid) || pid != GetCurrentProcessId())
                return TRUE;
            wchar_t cls[96]{};
            if (GetClassNameW(top, cls, static_cast<int>(std::size(cls))) <= 0)
                return TRUE;
            if (wcscmp(cls, L"CabinetWClass") != 0 && wcscmp(cls, L"ExploreWClass") != 0)
                return TRUE;

            constexpr UINT kTimeoutMs = 5000;
            DWORD_PTR smResult = 0;
            if (SendMessageTimeoutW(
                    top,
                    ExplorerCompatMsg::WM_GLASS_EXPLORER_ROOT_FULL_REDRAW,
                    0,
                    0,
                    SMTO_ABORTIFHUNG,
                    kTimeoutMs,
                    &smResult) != 0)
            {
                ++*sent;
            }
            return TRUE;
        }
    } // namespace

    void SyncExplorerShellChromeRedrawAll() noexcept
    {
        if (WndCompat::WinEventHook_GetMode() != Shared::InjectionMode::Full)
            return;

        unsigned sent = 0;
        EnumWindows(EnumSyncChromeRedraw, reinterpret_cast<LPARAM>(&sent));
        DebugLog(
            DBG_IMPORTANT,
            L"[WndCompat][Explorer] Sync CHROME_REDRAW count=%u",
            sent);
    }

    void SyncExplorerShellRootRedrawAll() noexcept
    {
        if (WndCompat::WinEventHook_GetMode() != Shared::InjectionMode::Full)
            return;

        unsigned sent = 0;
        EnumWindows(EnumSyncRootFullRedraw, reinterpret_cast<LPARAM>(&sent));
        DebugLog(
            DBG_IMPORTANT,
            L"[WndCompat][Explorer] Sync ROOT_FULL_REDRAW count=%u",
            sent);
    }

    void ApplyShellBackdropOnBackdropEnabled(HWND hRoot) noexcept
    {
        ApplyExplorerShellBackdropAtAttach(hRoot);
    }
}

// ── Раннее подклассирование из IAT CreateWindowExW (GlassCore) ───────────────
// Вызывается сразу после CreateWindowExW для CabinetWClass / ExploreWClass.
// Win11: ExplorerRootSubclassProc; Win10: общий RootSubclassProc.
extern "C" __declspec(dllexport)
void WINAPI WndCompat_EarlyExplorerRootAttach(HWND hWnd) noexcept
{
    if (!hWnd || !IsWindow(hWnd))
        return;
    if (WndCompat::IsShuttingDown())
        return;

    if (Shared::SystemHelper::g_buildNumber >= 22000)
        WndCompat::ExplorerRoot::Attach(hWnd);
    else
        WndCompat::Root_Attach(hWnd);
}
