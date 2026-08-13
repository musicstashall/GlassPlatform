// WndCompat/Root/RootSubclass.cpp
#include "WndPch.h"
#include "Root/RootSubclass.h"
#include "Root/Win32ChromeRestore.h"
#include "Tracker/BackdropPolicyRescan.h"
#include "Tracker/SubclassDiagPublish.h"
#include <wil.h>

#include <Popup/Menu/MenuOwnerSubclass.h>
#include <MenuEventProvider.h>
#include <MenuContext.h>

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <WndCompat.h>
#include <RootEventProvider.h>
#include <DropDownEventProvider.h>
#include "uah.h"
#include <Popup/ComboLBox/ComboLBoxSubclass.h>
#include <Controls/TreeViewSubclass.h>
#include <Controls/ListViewSubclass.h>
#include <Controls/StaticSubclass.h>
#include <Controls/ButtonSubclass.h>
#include <Controls/ScrollBarHost.h>
#include <Controls/TrackBarHost.h>
#include <Controls/DirectUISubclass.h>
#include <Controls/RebarSubclass.h>
#include <Controls/ToolbarSubclass.h>
#include <Explorer/ExplorerIslandContainerSubclass.h>
#include <Root/ExplorerRootSubclass.h>
#include <Tracker/WindowClassUtils.h>
#include <Tracker/WinEventHandler.h>
#include <GlassControlKind.h>
#include <GlassComctlScrollHost.h>
#include <InjectionMode.h>
#include <Helpers/ThemeHelper.h>
#include <RootBackdropPolicy.h>
#include <RootPolicyQuery.h>
#include <GlassBackdropMark.h>
#include <SystemHelper.h>
#include <GlassHostRegistry.h>
#include <Helpers/RootNcActiveCache.h>
#include <GlassMessages.h>
#include <ProcessBlockList.h>
#include <uxtheme.h>
#include "Root/RootExplorerSubclassBridge.h"
#include "Root/ExplorerRootSubclass.h"
#include "Root/RootSubclassShared.h"
#include "Root/RootBackdropRefreshThrottle.h"
#include "Root/TaskbarHost.h"
#include <string>

#pragma comment(lib, "uxtheme.lib")

#ifndef WM_DWMCOLORIZATIONCHANGED
#define WM_DWMCOLORIZATIONCHANGED 0x0320
#endif

namespace
{
    using GlassCoreIsExplorerAuxiliaryRootFn = BOOL(WINAPI*)(HWND) noexcept;
    using GlassCoreShouldApplyRootSwcaOnShowWindowFn = BOOL(WINAPI*)(HWND) noexcept;

    [[nodiscard]] GlassCoreIsExplorerAuxiliaryRootFn GetIsExplorerAuxiliaryRootFn() noexcept
    {
        static const GlassCoreIsExplorerAuxiliaryRootFn pfn = []() noexcept -> GlassCoreIsExplorerAuxiliaryRootFn {
            const HMODULE hCore = GetModuleHandleW(L"GlassCore.dll");
            if (!hCore)
                return nullptr;
            return reinterpret_cast<GlassCoreIsExplorerAuxiliaryRootFn>(
                GetProcAddress(hCore, "GlassCore_IsExplorerAuxiliaryRootHwnd"));
        }();
        return pfn;
    }

    [[nodiscard]] GlassCoreShouldApplyRootSwcaOnShowWindowFn GetShouldApplyRootSwcaOnShowWindowFn() noexcept
    {
        static const GlassCoreShouldApplyRootSwcaOnShowWindowFn pfn =
            []() noexcept -> GlassCoreShouldApplyRootSwcaOnShowWindowFn {
                const HMODULE hCore = GetModuleHandleW(L"GlassCore.dll");
                if (!hCore)
                    return nullptr;
                return reinterpret_cast<GlassCoreShouldApplyRootSwcaOnShowWindowFn>(
                    GetProcAddress(hCore, "GlassCore_ShouldApplyRootSwcaOnShowWindow"));
            }();
        return pfn;
    }

    /// Shell-root и auxiliary: SWCA на Root_Attach не вызываем —
    /// shell Win11 обслуживает ExplorerRootSubclassProc + DwmSetWindowAttribute IAT.
    [[nodiscard]] bool ShouldSkipRootAttachSwca(HWND hRoot) noexcept
    {
        if (Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(hRoot))
            return true;
        if (const auto pfn = GetIsExplorerAuxiliaryRootFn())
        {
            if (pfn(hRoot) != FALSE)
                return true;
        }
        return false;
    }

    // Wizard «Настроить визуально» (GlassSettings host): footer bar как ClearType wizard.
    constexpr int    kTextVisualTuningFooterBarHeight = 45;
    constexpr int    kExplorerControlPanelFooterPart  = 12;
    constexpr wchar_t kTextVisualTuningDlgClass[]       = L"GlassSettingsTextVisualTuningDlg";

    [[nodiscard]] bool IsTextVisualTuningWizardRoot(HWND hWnd) noexcept
    {
        return ProcessBlockList::IsCurrentProcessGlassToolHost()
            && WndCompat::IsWindowClass(hWnd, kTextVisualTuningDlgClass);
    }

    void PaintTextVisualTuningFooterBar(HWND hWnd, HDC hdc) noexcept
    {
        if (!hWnd || !hdc)
            return;

        RECT barRect{};
        GetClientRect(hWnd, &barRect);
        barRect.top = barRect.bottom - kTextVisualTuningFooterBarHeight;

        static HTHEME s_theme = nullptr;
        static bool     s_themeOpenAttempted = false;
        if (!s_themeOpenAttempted)
        {
            s_themeOpenAttempted = true;
            s_theme = OpenThemeData(nullptr, L"Explorer::ControlPanel");
        }
        if (!s_theme)
            return;

        DrawThemeBackground(
            s_theme,
            hdc,
            kExplorerControlPanelFooterPart,
            0,
            &barRect,
            nullptr);
    }

    // Кэш TabChroma в GlassCore; Refresh сюда через RootEventProvider (GlassShared без mutable state).
    void GlassCore_TabChroma_RefreshFromTheme(HWND themeAnchorOrNull) noexcept
    {
        if (auto* hp = WndCompat::Root::GetRootEventProvider())
        {
            if (hp->OnRootWmThemeChangedForTabChroma)
                hp->OnRootWmThemeChangedForTabChroma(themeAnchorOrNull);
        }
    }
} // namespace

// Состояние Root_Attach: общее для обычного root и Explorer shell-root (отдельный subclass proc).
static std::vector<HWND> g_rootWindows;
static std::mutex        g_rootWindowsMutex;

[[nodiscard]] std::size_t CountLiveRootWindowsUnlocked() noexcept
{
    std::size_t n = 0;
    for (HWND h : g_rootWindows)
    {
        if (h && IsWindow(h))
            ++n;
    }
    return n;
}

[[nodiscard]] std::size_t PublishLiveRootCount() noexcept
{
    std::size_t live = 0;
    std::size_t raw = 0;
    {
        std::lock_guard lock(g_rootWindowsMutex);
        raw = g_rootWindows.size();
        g_rootWindows.erase(
            std::remove_if(
                g_rootWindows.begin(),
                g_rootWindows.end(),
                [](HWND h) noexcept { return !h || !IsWindow(h); }),
            g_rootWindows.end());
        live = g_rootWindows.size();
    }
    WndCompat::SubclassDiag::PublishKindCount(GlassShared::GlassControlKind::Root, live);
    return live;
}
static std::unordered_map<ATOM, WndCompat::Root::ExplorerBridge::ClassBrushEntry> g_classBrushes;

static bool IsRootAttachedInternal(HWND hWnd)
{
    return std::find(g_rootWindows.begin(), g_rootWindows.end(), hWnd) != g_rootWindows.end();
}

namespace
{
    std::atomic<bool> g_startupOpenRootsFullRedrawPending{ false };

    void RedrawRootFullAfterStartupBackdropSweep(HWND hRoot) noexcept
    {
        if (!hRoot || !IsWindow(hRoot))
            return;
        if (GetAncestor(hRoot, GA_ROOT) != hRoot)
            return;
        if (WndCompat::IsShuttingDown())
            return;

        RedrawWindow(hRoot, nullptr, nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_ERASENOW | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_FRAME);

        if (Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(hRoot))
            WndCompat::ExplorerRoot::PostExplorerChromeRedrawIfShellRoot(hRoot);
    }

    void RedrawStartupOpenRootsAfterBackdropSweepIfPending() noexcept
    {
        if (!g_startupOpenRootsFullRedrawPending.exchange(false, std::memory_order_acq_rel))
            return;

        std::vector<HWND> windows;
        {
            std::lock_guard lock(g_rootWindowsMutex);
            windows = g_rootWindows;
        }

        DebugLog(DBG_IMPORTANT,
            L"[WndCompat][Root] StartupOpenRootsFullRedraw: roots=%zu",
            windows.size());

        for (HWND hRoot : windows)
            RedrawRootFullAfterStartupBackdropSweep(hRoot);
    }
} // namespace

static void RestoreRootNcAfterDetachImpl(HWND hWnd)
{
    if (Shared::SystemHelper::g_buildNumber >= 22000
        && Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(hWnd))
    {
        return;
    }

    RedrawWindow(hWnd, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_ERASENOW | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_FRAME);

    SetWindowPos(hWnd, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
        SWP_NOOWNERZORDER | SWP_DRAWFRAME | SWP_NOACTIVATE);

    // WinForms может кэшировать фоновые кисти (.EDIT). После снятия Glass
    // принудительно уведомляем корень о смене системных цветов/параметров,
    // чтобы дочерние контролы пересобрали brush-кэш.
    PostMessageW(hWnd, WM_SYSCOLORCHANGE, 0, 0);
    PostMessageW(hWnd, WM_SETTINGCHANGE, 0, 0);
    PostMessageW(hWnd, WM_THEMECHANGED, 0, 0);

    const BOOL isActive = (GetForegroundWindow() == hWnd);
    PostMessageW(hWnd, WM_NCACTIVATE, !isActive, 0);
    PostMessageW(hWnd, WM_NCACTIVATE, isActive, 0);
}

static void ApplyRootBackdropReapplyOnOwnerThreadImpl(HWND hRoot) noexcept
{
    if (!hRoot || !IsWindow(hRoot))
        return;
    if (GetAncestor(hRoot, GA_ROOT) != hRoot)
        return;
    if (WndCompat::IsShuttingDown())
        return;

    const bool allowed = WndCompat::IsGlassVisualsAllowed();
    GlassShared::GlassMarkRootBackdropAllowed(hRoot, allowed);

    if (!allowed)
    {
        WndCompat::RestoreRootWin32Chrome(hRoot, WndCompat::Win32ChromeRestoreScope::LiveNoBackdrop);
        return;
    }

    WndCompat::ApplyRootGlassVisualsOnBackdropEnabled(hRoot);
}

static LRESULT RootHandleReapplyRootBackdropMessage(HWND hWnd) noexcept
{
    ApplyRootBackdropReapplyOnOwnerThreadImpl(hWnd);
    return 0;
}

namespace WndCompat
{
void MarshalApplyRootBackdropReapplyForHwnd(HWND hRoot) noexcept
{
    if (!hRoot || !IsWindow(hRoot))
        return;

    wchar_t cls[256]{};
    if (::GetClassNameW(hRoot, cls, static_cast<int>(_countof(cls))) > 0)
    {
        if (_wcsicmp(cls, L"Shell_TrayWnd") == 0
            || _wcsicmp(cls, L"Shell_SecondaryTrayWnd") == 0)
        {
            WndCompat::DispatchTrayReapplyBackdropToOwnerThread(hRoot);
            return;
        }
    }

    const DWORD curTid = GetCurrentThreadId();
    const DWORD wndTid = GetWindowThreadProcessId(hRoot, nullptr);
    if (wndTid == curTid)
    {
        ApplyRootBackdropReapplyOnOwnerThreadImpl(hRoot);
        return;
    }

    DWORD_PTR sendResult = 0;
    const BOOL sent = SendMessageTimeoutW(
        hRoot,
        WndCompat::WM_GLASS_REAPPLY_ROOT_BACKDROP,
        0,
        0,
        SMTO_ABORTIFHUNG,
        1000,
        &sendResult);
    if (!sent)
    {
        DebugLog(DBG_WARNING,
            L"[WndCompat][Root] Marshal REAPPLY: SendMessage timeout/fail hwnd=0x%p",
            hRoot);
    }
}
} // namespace WndCompat

static LRESULT RootHandleDetachRootMessage(
    HWND hWnd,
    SUBCLASSPROC selfProc,
    UINT_PTR uIdSubclass)
{
    if (auto* hp = WndCompat::Root::GetRootEventProvider())
    {
        if (hp->OnRootHooksDetach)
            hp->OnRootHooksDetach(hWnd);
    }
    RemoveWindowSubclass(hWnd, selfProc, uIdSubclass);
    GlassShared::GlassUnmarkWindow(hWnd);
    GlassShared::GlassClearComctlScrollHost(hWnd);
    DebugLog(L"[WndCompat][Root] WM_GLASS_DETACH_ROOT: субкласс снят hwnd=0x%p", hWnd);

    if (GetAncestor(hWnd, GA_ROOT) == hWnd)
        RedrawWindow(hWnd, nullptr, nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_ERASENOW | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_FRAME);

    return 0;
}

static void RootHandleRootAttachedWndNcDestroy(HWND hWnd)
{
    bool found = false;
    {
        std::lock_guard lock(g_rootWindowsMutex);
        auto it = std::find(g_rootWindows.begin(), g_rootWindows.end(), hWnd);
        if (it != g_rootWindows.end())
        {
            g_rootWindows.erase(it);
            found = true;
        }
        ATOM classAtom = static_cast<ATOM>(GetClassLongPtr(hWnd, GCW_ATOM));
        if (classAtom)
        {
            auto brushIt = g_classBrushes.find(classAtom);
            if (brushIt != g_classBrushes.end() && --brushIt->second.refCount <= 0)
                g_classBrushes.erase(brushIt);
        }
    }
    if (found)
    {
        Shared::RootNcActiveCache::Remove(hWnd);
        WndCompat::RootBackdropRefreshThrottle::Erase(hWnd);
        WndCompat::RemoveGlassCoreThemePaintRootNcState(hWnd);
        WndCompat::RemoveTextGlowClientsForRootWithGlassCore(hWnd);
        (void)PublishLiveRootCount();
        WndCompat::RemoveTextInkClientsForRootWithGlassCore(hWnd);
        Shared::GlassHostRegistry::Remove(hWnd);
        GlassShared::GlassUnmarkWindow(hWnd);
        GlassShared::GlassClearComctlScrollHost(hWnd);
        WndCompat::MenuOwner_ReleaseClassicMenuBarHold(hWnd);

        if (auto* hp = WndCompat::Root::GetRootEventProvider())
        {
            if (hp->OnRootHooksDetach)
                hp->OnRootHooksDetach(hWnd);
        }

        DebugLog(std::format(
            L"[WndCompat][Root] Root WM_NCDESTROY: removed 0x{:X} from registry",
            reinterpret_cast<std::uintptr_t>(hWnd)
        ).c_str());
    }
}

namespace WndCompat::Root::ExplorerBridge
{
    LRESULT OnDetachRoot(HWND hWnd, SUBCLASSPROC selfProc, UINT_PTR uIdSubclass)
    {
        return RootHandleDetachRootMessage(hWnd, selfProc, uIdSubclass);
    }

    void OnRootAttachedWndNcDestroy(HWND hWnd)
    {
        RootHandleRootAttachedWndNcDestroy(hWnd);
    }

    bool TryRegisterRootWindowForAttach(HWND hWnd)
    {
        {
            std::lock_guard lock(g_rootWindowsMutex);
            if (IsRootAttachedInternal(hWnd))
                return false;
            g_rootWindows.push_back(hWnd);
        }
        (void)PublishLiveRootCount();
        return true;
    }

    void UnregisterRootWindowAfterSubclassFailed(HWND hWnd)
    {
        {
            std::lock_guard lock(g_rootWindowsMutex);
            auto it = std::find(g_rootWindows.begin(), g_rootWindows.end(), hWnd);
            if (it != g_rootWindows.end())
                g_rootWindows.erase(it);
        }
        (void)PublishLiveRootCount();
    }

    void ApplyRootClassBlackBrush(HWND hWnd)
    {
        const ATOM classAtom = static_cast<ATOM>(GetClassLongPtr(hWnd, GCW_ATOM));
        const LONG_PTR prevClassStyle = GetClassLongPtr(hWnd, GCL_STYLE);
        auto hPrevBrush = reinterpret_cast<HBRUSH>(
            SetClassLongPtr(hWnd, GCLP_HBRBACKGROUND, (LONG_PTR)GetStockObject(BLACK_BRUSH)));
        // Паттерн Root GA_ROOT: усиливаем перерисовку класса, чтобы фон стабильно
        // пересчитывался при изменениях размеров/рамки.
        SetClassLongPtr(hWnd, GCL_STYLE, prevClassStyle | CS_HREDRAW | CS_VREDRAW);
        std::lock_guard lock(g_rootWindowsMutex);
        auto [it, inserted] = g_classBrushes.try_emplace(
            classAtom,
            ClassBrushEntry{ hPrevBrush, prevClassStyle, 1 });
        if (!inserted)
            it->second.refCount++;
    }

    void RestoreRootClassBrushForHwnd(HWND hWnd) noexcept
    {
        if (!hWnd || !IsWindow(hWnd))
            return;

        const ATOM classAtom = static_cast<ATOM>(GetClassLongPtr(hWnd, GCW_ATOM));
        if (!classAtom)
            return;

        std::lock_guard lock(g_rootWindowsMutex);
        const auto brushIt = g_classBrushes.find(classAtom);
        if (brushIt == g_classBrushes.end())
            return;

        SetClassLongPtr(hWnd, GCLP_HBRBACKGROUND, reinterpret_cast<LONG_PTR>(brushIt->second.hOrigBrush));
        SetClassLongPtr(hWnd, GCL_STYLE, brushIt->second.origClassStyle);
        if (--brushIt->second.refCount <= 0)
            g_classBrushes.erase(brushIt);
    }

    bool ShouldSetComctlScrollHostForRootClassName(const wchar_t* rootClassName) noexcept
    {
        if (!rootClassName || !rootClassName[0])
            return false;
        if (_wcsicmp(rootClassName, L"ComboBoxEx32") == 0)
            return false;
        return true;
    }

    bool RemoveTrackedRootAndDecrementBrush(HWND hWnd)
    {
        bool removed = false;
        {
            std::lock_guard lock(g_rootWindowsMutex);
            auto it = std::find(g_rootWindows.begin(), g_rootWindows.end(), hWnd);
            if (it == g_rootWindows.end())
                return false;
            g_rootWindows.erase(it);
            removed = true;
            const ATOM classAtom = static_cast<ATOM>(GetClassLongPtr(hWnd, GCW_ATOM));
            if (classAtom)
            {
                auto brushIt = g_classBrushes.find(classAtom);
                if (brushIt != g_classBrushes.end() && --brushIt->second.refCount <= 0)
                    g_classBrushes.erase(brushIt);
            }
        }
        if (removed)
            (void)PublishLiveRootCount();
        return true;
    }

    void RestoreOriginalClassBrush(
        HWND hWnd,
        const std::unordered_map<ATOM, ClassBrushEntry>& classBrushes)
    {
        const ATOM classAtom = static_cast<ATOM>(GetClassLongPtr(hWnd, GCW_ATOM));
        const auto brushIt = classBrushes.find(classAtom);
        if (brushIt != classBrushes.end())
        {
            SetClassLongPtr(hWnd, GCLP_HBRBACKGROUND, (LONG_PTR)brushIt->second.hOrigBrush);
            SetClassLongPtr(hWnd, GCL_STYLE, brushIt->second.origClassStyle);
        }
    }

    void RestoreRootNcAfterDetach(HWND hWnd)
    {
        RestoreRootNcAfterDetachImpl(hWnd);
    }

    bool IsWin11ExplorerShellCabinetChromeHwnd(HWND hRoot) noexcept
    {
        if (auto* p = WndCompat::Root::GetRootEventProvider();
            p && p->IsWin11ExplorerShellCabinetChromeHwnd)
            return p->IsWin11ExplorerShellCabinetChromeHwnd(hRoot);
        return Shared::SystemHelper::g_buildNumber >= 22000
            && Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(hRoot)
            && Shared::RootBackdropPolicy::ExplorerShellRootHasShellTabDescendant(hRoot);
    }
}

namespace WndCompat::Root::Detail
{
    namespace
    {
        using RootSubclassSharedFn = LRESULT(CALLBACK*)(
            HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR, SUBCLASSPROC);
        RootSubclassSharedFn s_rootSubclassSharedImpl = nullptr;
    }

    void RegisterRootSubclassSharedImpl(RootSubclassSharedFn fn) noexcept
    {
        s_rootSubclassSharedImpl = fn;
    }

    LRESULT CALLBACK RootSubclassSharedDispatch(
        HWND hWnd,
        UINT uMsg,
        WPARAM wParam,
        LPARAM lParam,
        UINT_PTR uIdSubclass,
        DWORD_PTR dwRefData,
        SUBCLASSPROC selfProc)
    {
        if (!s_rootSubclassSharedImpl)
            return DefSubclassProc(hWnd, uMsg, wParam, lParam);
        return s_rootSubclassSharedImpl(
            hWnd, uMsg, wParam, lParam, uIdSubclass, dwRefData, selfProc);
    }
}

namespace
{
    constexpr UINT_PTR kRootSubclassId = 1;

    using SetCtlColorBkColorProbeAttachFn = void(WINAPI*)(BOOL) noexcept;

    [[nodiscard]] SetCtlColorBkColorProbeAttachFn GetSetCtlColorBkColorProbeAttachFn() noexcept
    {
        static const SetCtlColorBkColorProbeAttachFn pfn = []() noexcept -> SetCtlColorBkColorProbeAttachFn {
            const HMODULE hCore = GetModuleHandleW(L"GlassCore.dll");
            if (!hCore)
                return nullptr;
            return reinterpret_cast<SetCtlColorBkColorProbeAttachFn>(
                GetProcAddress(hCore, "GlassCore_SetCtlColorBkColorProbeAttach"));
        }();
        return pfn;
    }

    void SetCtlColorBkColorProbeAttach(bool enable) noexcept
    {
        const auto pfn = GetSetCtlColorBkColorProbeAttachFn();
        if (pfn)
            pfn(enable ? TRUE : FALSE);
    }

    // BWM WinHandler WM_CTLCOLORSTATIC/LISTBOX — probe кисти через DefSubclassProc + SetBkColor hook.
    [[nodiscard]] bool TryResolveCtlColorStaticBrush(
        HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT& outResult) noexcept
    {
        if (WndCompat::WinEventHook_GetMode() != Shared::InjectionMode::Full)
            return false;
        if (!GlassShared::GlassIsBackdropAllowedForHwnd(hWnd))
            return false;
        if (uMsg != WM_CTLCOLORSTATIC && uMsg != WM_CTLCOLORLISTBOX)
            return false;

        if (uMsg == WM_CTLCOLORLISTBOX)
        {
            const HWND hChild = reinterpret_cast<HWND>(lParam);
            if (GlassShared::GlassGetWindowKind(hChild) == GlassShared::GlassControlKind::ComboLBox
                || WndCompat::IsWindowClass(hChild, L"ComboLBox"))
                return false;
        }

        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetCtlColorBkColorProbeAttach(true);
        const LRESULT defResult = DefSubclassProc(hWnd, uMsg, wParam, lParam);
        SetCtlColorBkColorProbeAttach(false);

        LOGBRUSH lb{};
        if (GetObject(reinterpret_cast<HGDIOBJ>(defResult), sizeof(lb), &lb))
        {
            if (lb.lbStyle == BS_SOLID && GetBkMode(hdc) == OPAQUE)
            {
                if (!lb.lbColor
                    || lb.lbColor == GetSysColor(COLOR_WINDOW)
                    || lb.lbColor == GetSysColor(COLOR_BTNFACE))
                {
                    // sys-фон → чёрная кисть ниже
                }
                else
                {
                    outResult = defResult;
                    return true;
                }
            }
            else if (lb.lbStyle == BS_NULL && GetBkMode(hdc) == TRANSPARENT)
            {
                outResult = defResult;
                return true;
            }
        }

        SetBkColor(hdc, 0);
        outResult = reinterpret_cast<LRESULT>(GetStockObject(BLACK_BRUSH));
        return true;
    }

    LRESULT CALLBACK RootSubclassProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

    LRESULT CALLBACK RootSubclassProcShared(
        HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
        UINT_PTR uIdSubclass, DWORD_PTR dwRefData,
        SUBCLASSPROC selfProc)
    {
        // Снятие субкласса по запросу из чужого потока (Root_DetachAll → SendMessageTimeout).
        // Обрабатываем до шлюза IsShuttingDown — это и есть цель shutdown-пути.
        // RemoveWindowSubclass здесь безопасен: мы в потоке-владельце окна.
        if (uMsg == WndCompat::WM_GLASS_DETACH_ROOT)
            return RootHandleDetachRootMessage(hWnd, selfProc, uIdSubclass);

        if (uMsg == WndCompat::WM_GLASS_REAPPLY_ROOT_BACKDROP)
            return RootHandleReapplyRootBackdropMessage(hWnd);

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

        if (uMsg == WndCompat::WM_GLASS_REMIGRATE_THEMED_CHROME)
        {
            WndCompat::RebindThemedChromeClientsAfterWinFormsWindowMarkedWithGlassCore(
                reinterpret_cast<HWND>(wParam));
            return 0;
        }

        if (uMsg == WndCompat::WM_GLASS_INVALIDATE_TOPLEVEL_VISUAL)
        {
            RedrawWindow(hWnd, nullptr, nullptr,
                RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME | RDW_INTERNALPAINT);
            return 0;
        }

        if (WndCompat::IsShuttingDown())
            return DefSubclassProc(hWnd, uMsg, wParam, lParam);

        switch (uMsg)
        {
        case WM_THEMECHANGED:
            GlassCore_TabChroma_RefreshFromTheme(nullptr);
            break;

        case WM_SHOWWINDOW:
            if (wParam && GetAncestor(hWnd, GA_ROOT) == hWnd && WndCompat::WinEventHook_GetMode()==Shared::InjectionMode::Full
                && GlassShared::GlassIsBackdropAllowedForHwnd(hWnd)
                && Shared::RootPolicyQuery::UsesSwcaBackdrop())
            {
                const auto shouldSwca = GetShouldApplyRootSwcaOnShowWindowFn();
                if (!shouldSwca || shouldSwca(hWnd) == FALSE)
                    break;

                auto provider = WndCompat::Root::GetRootEventProvider();
                if (provider && provider->OnRootSwcaBackdropActiveApply)
                {
                    WndCompat::RootBackdropRefreshThrottle::SetRootSwcaApplyReasonForNextCall(L"SHOWWINDOW");
                    provider->OnRootSwcaBackdropActiveApply(hWnd);
                }
            }
            break;

        case WM_DWMCOLORIZATIONCHANGED:
        {
            if (WndCompat::WinEventHook_GetMode() != Shared::InjectionMode::Full)
                break;
            if (auto* hp = WndCompat::Root::GetRootEventProvider())
            {
                if (hp->OnRootDwmColorizationChanged)
                    hp->OnRootDwmColorizationChanged(hWnd, wParam, lParam);
            }
            WndCompat::PostDwmColorizationDispatch(wParam);
            break;
        }

            // ── Чёрный фон (основа прозрачности через DWM blur) ──────────────────
        case WM_ERASEBKGND:
        {
            if (IsTextVisualTuningWizardRoot(hWnd))
            {
                const LRESULT erased = DefSubclassProc(hWnd, uMsg, wParam, lParam);
                PaintTextVisualTuningFooterBar(hWnd, reinterpret_cast<HDC>(wParam));
                return erased != 0 ? erased : TRUE;
            }

            // Shell-проводник на RootSubclassProc (GLASS_DIAG_EXPLORER_USE_ROOT_SUBCLASS): как ExplorerRootSubclass —
            // не заливаем клиент чёрным, иначе SWCA/DWM не видны (полное «чёрное окно»).
            if (Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(hWnd))
                break;

            if (!GlassShared::GlassIsBackdropAllowedForHwnd(hWnd))
                break;

            // Root: чёрная база под стекло. Дочерний контейнер Shell DocObject View (MMC / DocObject) — то же.
            if (GetWindowLongW(hWnd, GWL_EXSTYLE) & WS_EX_LAYERED)
                break;

            if (WndCompat::WinEventHook_GetMode() == Shared::InjectionMode::Full)
            {
                /*const bool isShellDocObject = WndCompat::IsWindowClass(hWnd, L"Shell DocObject View");
                const bool isGaRoot = (GetAncestor(hWnd, GA_ROOT) == hWnd);
                if (isGaRoot || isShellDocObject)*/
                {
                    HDC hdc = reinterpret_cast<HDC>(wParam);
                    RECT clipRect{};
                    GetClipBox(hdc, &clipRect);
                    PatBlt(hdc, clipRect.left, clipRect.top,
                        clipRect.right - clipRect.left,
                        clipRect.bottom - clipRect.top,
                        BLACKNESS);
                    return 1;
                }
            }
            break;
        }

        // ── Чёрная кисть для дочерних контролов ──────────────────────────────
        case WM_CTLCOLORSTATIC:
        {
            LRESULT ctlResult{};
            if (TryResolveCtlColorStaticBrush(hWnd, uMsg, wParam, lParam, ctlResult))
                return ctlResult;
            break;
        }

        case WM_CTLCOLOREDIT:
            if (WndCompat::IsWindowClass(hWnd, L"ComboBoxEx32") && WndCompat::IsWindowClass(GetParent(hWnd), L"msctls_progress32"))
            {
                break;
            }
        case WM_CTLCOLORSCROLLBAR:
        case WM_CTLCOLORDLG:
        case WM_CTLCOLORMSGBOX:
        case WM_CTLCOLORBTN:
        {
            if (WndCompat::WinEventHook_GetMode() != Shared::InjectionMode::Full)
                break;

            if (!GlassShared::GlassIsBackdropAllowedForHwnd(hWnd))
                break;

            // Визуальный стиль устанавливает SetTextColor на DC через DefWindowProc
            DefSubclassProc(hWnd, uMsg, wParam, lParam);

            // Переопределяем только фон для стекла
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetBkColor(hdc, 0);
            return reinterpret_cast<LRESULT>(GetStockObject(BLACK_BRUSH));
        }

        // ComboLBox / ListBox в Full: как у прочих WM_CTLCOLOR* — сначала Def для SetTextColor темы, потом чёрный фон
        case WM_CTLCOLORLISTBOX:
        {
            HWND hChild = reinterpret_cast<HWND>(lParam);
            const bool comboLBoxChild =
                GlassShared::GlassGetWindowKind(hChild) == GlassShared::GlassControlKind::ComboLBox;
            if (!comboLBoxChild
                && !WndCompat::IsWindowClass(hChild, L"ComboLBox")
                && WndCompat::WinEventHook_GetMode() != Shared::InjectionMode::Full)
                break;

            if (!comboLBoxChild && !GlassShared::GlassIsBackdropAllowedForHwnd(hWnd))
                break;

            if (!comboLBoxChild)
            {
                LRESULT ctlResult{};
                if (TryResolveCtlColorStaticBrush(hWnd, uMsg, wParam, lParam, ctlResult))
                    return ctlResult;
            }

            // Визуальный стиль устанавливает SetTextColor на DC через цепочку DefWindowProc (как WM_CTLCOLOREDIT и др.)
            DefSubclassProc(hWnd, uMsg, wParam, lParam);

            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetBkColor(hdc, 0);
            return reinterpret_cast<LRESULT>(GetStockObject(BLACK_BRUSH));
        }

        // ── Чёрный фон для AnimateWindow ─────────────────────────────────────
        case WM_PRINTCLIENT:
        {
            /*DebugLog(DBG_IMPORTANT,
                L"[WndCompat][Root] WM_PRINTCLIENT: hwnd=0x%p isGaRoot=%d isNetUi=%d kind=%d",
                hWnd,
                (GetAncestor(hWnd, GA_ROOT) == hWnd) ? 1 : 0,
                (GlassShared::GlassGetWindowKind(hWnd) == GlassShared::GlassControlKind::NetUI) ? 1 : 0,
                static_cast<int>(GlassShared::GlassGetWindowKind(hWnd)));*/
            if (lParam != PRF_CLIENT
                || WndCompat::WinEventHook_GetMode() != Shared::InjectionMode::Full)
                break;

            if (!GlassShared::GlassIsBackdropAllowedForHwnd(hWnd))
                break;

            if (Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(hWnd))
                break;

            const GlassShared::GlassControlKind kind = GlassShared::GlassGetWindowKind(hWnd);
            const bool isGaRoot = (GetAncestor(hWnd, GA_ROOT) == hWnd);
            const bool isWinFormsWindow = (kind == GlassShared::GlassControlKind::WinForms_Window);
            const bool isNetUi = (kind == GlassShared::GlassControlKind::NetUI);
            if (!isGaRoot && !isWinFormsWindow && !isNetUi)
                break;

            HDC hdc = reinterpret_cast<HDC>(wParam);
            RECT clipRect{};
            GetClipBox(hdc, &clipRect);
            PatBlt(hdc, clipRect.left, clipRect.top,
                clipRect.right - clipRect.left,
                clipRect.bottom - clipRect.top,
                BLACKNESS);
            return TRUE;
        }

        // Classic menu bar (без визуальной темы «Menu»): BWM WinHandler — ExtTextOut
        // вокруг WM_NCACTIVATE / WM_NCPAINT; HTMENU + MSGF_MENU удерживают хук, NCMOUSELEAVE снимает.
        case WM_NCACTIVATE:
        {
            const BOOL ncActive = (wParam != FALSE);
            const bool wasNcActive = Shared::RootNcActiveCache::Get(hWnd);
            const bool nowNcActive = (ncActive != FALSE);
            const bool ncStateChanged = (wasNcActive != nowNcActive);

            LRESULT lr{};
            if (!WndCompat::MenuOwner_HandleClassicMenuBarNcActivate(hWnd, wParam, lParam, lr))
                lr = DefSubclassProc(hWnd, uMsg, wParam, lParam);

            if (ncStateChanged)
                Shared::RootNcActiveCache::Set(hWnd, nowNcActive);

            // SWCA backdrop на NCACTIVATE — только pre-Win11; Win11 — IAT CAPTION_COLOR / hooks.
            if (ncStateChanged
                && Shared::SystemHelper::g_buildNumber < 22000
                && !WndCompat::RootBackdropRefreshThrottle::ShouldSkip(hWnd)
                && WndCompat::WinEventHook_GetMode() == Shared::InjectionMode::Full
                && GlassShared::GlassIsBackdropAllowedForHwnd(hWnd)
                && Shared::RootPolicyQuery::UsesSwcaBackdrop())
            {
                if (auto* p = WndCompat::Root::GetRootEventProvider())
                {
                    WndCompat::RootBackdropRefreshThrottle::SetRootSwcaApplyReasonForNextCall(L"NCACTIVATE");
                    if (nowNcActive)
                    {
                        if (p->OnRootSwcaBackdropActiveApply)
                            p->OnRootSwcaBackdropActiveApply(hWnd);
                    }
                    else if (p->OnRootSwcaBackdropInactiveApply)
                    {
                        p->OnRootSwcaBackdropInactiveApply(hWnd);
                    }
                    WndCompat::RootBackdropRefreshThrottle::Stamp(hWnd);
                }
            }
            if (GlassShared::GlassGetWindowKind(hWnd) == GlassShared::GlassControlKind::WinForms_Window)
            {
                const HWND gaRoot = GetAncestor(hWnd, GA_ROOT);
                DebugLog(DBG_IMPORTANT,
                    L"[WndCompat][Root][ThemePaint][Chrome] WM_NCACTIVATE WinForms_Window hwnd=0x%p gaRoot=0x%p ncActive=%d",
                    hWnd,
                    gaRoot,
                    nowNcActive ? 1 : 0);
            }

            // После обработки меню — ОС уже согласовала foreground; иначе GetForegroundWindow в Refresh устаревает.
            WndCompat::FinalizeGlassCoreRootActivationVisualUpdate(hWnd, ncActive);
            return lr;
        }
        case WM_NCPAINT:
        {
            LRESULT lr{};
            if (WndCompat::MenuOwner_HandleClassicMenuBarNcPaint(hWnd, wParam, lParam, lr))
                return lr;
            break;
        }
        case WM_NCHITTEST:
        {
            LRESULT lr{};
            (void)WndCompat::MenuOwner_HandleClassicMenuBarNcHitTest(hWnd, wParam, lParam, lr);
            return lr;
        }
        case WM_ENTERIDLE:
            WndCompat::MenuOwner_OnClassicMenuBarEnterIdle(hWnd, wParam, lParam);
            break;
        case WM_NCMOUSELEAVE:
            WndCompat::MenuOwner_OnClassicMenuBarNcMouseLeave(hWnd);
            break;

        case WM_NCDESTROY:
            RootHandleRootAttachedWndNcDestroy(hWnd);
            break;

        // ── CBN_DROPDOWN → AnimateWindow hook для ComboLBox ────────────────────
        case WM_COMMAND:
        {
            HWND hCtrl = reinterpret_cast<HWND>(lParam);
            if (HIWORD(wParam) == CBN_DROPDOWN && IsWindow(hCtrl))
            {
                // Получаем ComboLBox из ComboBox и аттачим перед показом
                COMBOBOXINFO cbi{};
                cbi.cbSize = sizeof(cbi);
                if (GetComboBoxInfo(hCtrl, &cbi) && IsWindow(cbi.hwndList)
                    && !WndCompat::ComboLBox_IsAttached(cbi.hwndList))
                {
                    DebugLog(L"[WndCompat][Root] CBN_DROPDOWN ComboLBox_Attach: list=0x%p combo=0x%p",
                        cbi.hwndList, hCtrl);
                    WndCompat::ComboLBox_Attach(cbi.hwndList);
                }
                // Диагностика: класс родителя ComboBox влияет на ветку FloatNotifySink ниже.
                {
                    HWND comboParent = GetParent(hCtrl);
                    WCHAR parentClass[256]{};
                    if (comboParent && IsWindow(comboParent))
                        GetClassNameW(comboParent, parentClass, static_cast<int>(std::size(parentClass)));
                    DebugLog(L"[WndCompat][Root] CBN_DROPDOWN: comboParent=0x%p class=\"%s\"",
                        comboParent, parentClass[0] ? parentClass : L"(null)");
                    // Дублируем в ODS: при вложенных DebugLog из Attach строка в LogViewer иногда не
                    // появляется сразу или теряется — так видно факт выполнения ветки (DebugView).
                    const std::wstring ods = std::format(
                        L"[WndCompat][Root] CBN_DROPDOWN: comboParent=0x{:X} class=\"{}\"\n",
                        reinterpret_cast<std::uintptr_t>(comboParent),
                        parentClass[0] ? parentClass : L"(null)");
                    OutputDebugStringW(ods.c_str());
                }

                // AnimateWindow hook (только не для FloatNotifySink)
                if (!WndCompat::IsWindowClass(GetParent(hCtrl), L"FloatNotifySink"))
                {
                    DebugLog(L"[WndCompat][Root] CBN_DROPDOWN: combo=0x%p, hwndList=0x%p",
						hCtrl, cbi.hwndList);
                    auto* p = WndCompat::DropDown::GetDropDownEventProvider();
                    if (p && p->OnComboBoxDropDown)
                        p->OnComboBoxDropDown(hCtrl);
                }
            }
            break;
        }

        case WM_DRAWITEM:
        {
            const DRAWITEMSTRUCT* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (!dis)
                break;

            // Нас интересуют только пункты меню
            if (dis->CtlType == ODT_MENU)
            {
                return WndCompat::MenuOwnerSubclass(
                    hWnd,
                    uMsg,
                    wParam,
                    lParam
                );
            }

            break;
        }

        case WM_MEASUREITEM:
        {
            const MEASUREITEMSTRUCT* mis = reinterpret_cast<const MEASUREITEMSTRUCT*>(lParam);
            if (!mis || mis->CtlType != ODT_MENU)
                break;

            return WndCompat::MenuOwnerSubclass(
                hWnd,
                uMsg,
                wParam,
                lParam
            );
        }

        // WM_UAH* на окне-владельце: вся сессия хуков и ветвление — в MenuOwnerSubclass.cpp
        // (WinHandler-режим для DRAW* vs только флаг для INIT/NCPAINT/MEASURE).
        case WM_UAHNCPAINTMENUPOPUP:
        case WM_UAHDRAWMENUITEM:
        case WM_UAHINITMENU:
        case WM_UAHDRAWMENU:
            return WndCompat::MenuOwnerSubclass(
                hWnd,
                uMsg,
                wParam,
                lParam
            );
        case WM_UAHMEASUREMENUITEM:
        {
            const MEASUREITEMSTRUCT* mis = reinterpret_cast<const MEASUREITEMSTRUCT*>(lParam);
            if (!mis)
                break;

            if (mis->CtlType == ODT_MENU)
            {
                return WndCompat::MenuOwnerSubclass(
                    hWnd,
                    uMsg,
                    wParam,
                    lParam
                );
            }

            break;
        }

        // Трекбар / Toolbar: NM_CUSTOMDRAW на родителе (Root).
        case WM_NOTIFY:
        {
            if (WndCompat::WinEventHook_GetMode() != Shared::InjectionMode::Full)
                break;

            auto* nmhdr = reinterpret_cast<LPNMHDR>(lParam);
            if (!nmhdr || nmhdr->code != NM_CUSTOMDRAW)
                break;

            HWND hwndFrom = nmhdr->hwndFrom;
            if (!hwndFrom || !IsWindow(hwndFrom))
                break;

            wchar_t cls[64]{};
            if (GetClassNameW(hwndFrom, cls, static_cast<int>(std::size(cls))) == 0)
                break;

            // ── Toolbar: CDDS_PREERASE — skip дефолтного erase (F4), фон через DTPB родителя.
            if (_wcsicmp(cls, TOOLBARCLASSNAMEW) == 0
                && GlassShared::GlassGetWindowKind(hwndFrom) == GlassShared::GlassControlKind::Toolbar)
            {
                auto* nmcd = reinterpret_cast<LPNMCUSTOMDRAW>(lParam);
                if (nmcd->dwDrawStage == CDDS_PREERASE)
                {
                    if (nmcd->hdc)
                    {
                        RECT rc = nmcd->rc;
                        if (rc.right <= rc.left || rc.bottom <= rc.top)
                            GetClientRect(hwndFrom, &rc);
                        DrawThemeParentBackground(hwndFrom, nmcd->hdc, &rc);
                    }
                    return CDRF_SKIPDEFAULT;
                }
                break;
            }

            if (_wcsicmp(cls, TRACKBAR_CLASSW) != 0)
                break;

            // Live OFF / NoBackdrop: не перехватываем ticks — иначе остаётся чёрный фон comctl.
            if (!GlassShared::GlassIsBackdropAllowedForHwnd(hWnd))
                break;

            auto* nmcd = reinterpret_cast<LPNMCUSTOMDRAW>(lParam);
            if (GlassShared::GlassGetWindowKind(hwndFrom) != GlassShared::GlassControlKind::TrackBar)
                break;

            switch (nmcd->dwDrawStage)
            {
            case CDDS_PREPAINT:
                return CDRF_NOTIFYITEMDRAW;

            case CDDS_ITEMPREPAINT:
                if (nmcd->dwItemSpec == TBCD_THUMB || nmcd->dwItemSpec == TBCD_CHANNEL)
                    return DefSubclassProc(hWnd, uMsg, wParam, lParam);

                if (nmcd->dwItemSpec == TBCD_TICS)
                {
                    HDC        hdc = nmcd->hdc;
                    const RECT rc  = nmcd->rc;
                    if (hdc)
                    {
                        PatBlt(hdc, rc.left, rc.top,
                            rc.right - rc.left, rc.bottom - rc.top, BLACKNESS);
                    }
                    return CDRF_SKIPDEFAULT;
                }

                return DefSubclassProc(hWnd, uMsg, wParam, lParam);

            default:
                break;
            }
            break;
        }

        }

        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }

    LRESULT CALLBACK RootSubclassProc(
        HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
        UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
    {
        return RootSubclassProcShared(
            hWnd, uMsg, wParam, lParam, uIdSubclass, dwRefData, RootSubclassProc);
    }

    struct RootSubclassSharedRegistrar
    {
        RootSubclassSharedRegistrar() noexcept
        {
            WndCompat::Root::Detail::RegisterRootSubclassSharedImpl(RootSubclassProcShared);
        }
    } s_rootSubclassSharedRegistrar;
}


namespace
{
    void ApplyBackdropPolicyToTrackedRootsImpl() noexcept
    {
        const bool allowed = WndCompat::IsGlassVisualsAllowed();

        if (!allowed)
        {
            WndCompat::RestoreWin32Chrome(WndCompat::Win32ChromeRestoreScope::LiveNoBackdrop);
            return;
        }

        WndCompat::BackdropPolicyRescan::RescanAllMissingGlassHostsForBackdropEnable();

        std::vector<HWND> windows;
        {
            std::lock_guard lock(g_rootWindowsMutex);
            windows = g_rootWindows;
        }

        for (HWND hRoot : windows)
        {
            if (!hRoot || !IsWindow(hRoot))
                continue;
            WndCompat::MarshalApplyRootBackdropReapplyForHwnd(hRoot);
        }

        WndCompat::NotifyExplorerRootBackdropPolicyChanged(true);

        WndCompat::TreeView_ApplyBackdropPolicyToTracked();
        WndCompat::ListView_ApplyBackdropPolicyToTracked();
        WndCompat::Button_ApplyBackdropPolicyToTracked();
        WndCompat::DirectUI_ApplyBackdropPolicyToTracked();
        WndCompat::Rebar_ApplyBackdropPolicyToTracked();
        WndCompat::ToolBar_ApplyBackdropPolicyToTracked();
        WndCompat::ScrollBar_ApplyBackdropPolicyToTracked();
        WndCompat::TrackBar_ApplyBackdropPolicyToTracked();
        WndCompat::ExplorerIslandContainer_ApplyBackdropPolicyToTracked();
        // Tray SWCA — отдельная политика Tray\; повторный ApplyEffect здесь сбрасывает XAML Fill после TAP.

        RedrawStartupOpenRootsAfterBackdropSweepIfPending();
    }
} // namespace

namespace WndCompat
{
    void WINAPI ApplyBackdropPolicyToTrackedRoots() noexcept
    {
        ApplyBackdropPolicyToTrackedRootsImpl();
    }

    void NotifyInitialRootScanCompleted() noexcept
    {
        g_startupOpenRootsFullRedrawPending.store(true, std::memory_order_release);
    }

    void ApplyRootBackdropReapplyOnOwnerThread(HWND hRoot) noexcept
    {
        ApplyRootBackdropReapplyOnOwnerThreadImpl(hRoot);
    }

    // Зеркало post-attach визуалов Root_Attach при ENABLE (кисть + SWCA/DWM + redraw).
    // OnRootHooksAttach намеренно не вызывается — IAT: ModuleHandler::ScanBackdropScopedModules.
    void ApplyRootGlassVisualsOnBackdropEnabled(HWND hRoot) noexcept
    {
        if (!hRoot || !IsWindow(hRoot))
            return;
        if (WndCompat::IsShuttingDown())
            return;
        if (!Shared::RootPolicyQuery::IsRootGlassRenderingEnabled())
            return;
        if (GetAncestor(hRoot, GA_ROOT) != hRoot)
            return;

        if (Shared::RootPolicyQuery::IsRootGlassRenderingEnabled())
            WndCompat::Root::ExplorerBridge::ApplyRootClassBlackBrush(hRoot);

        const bool isActive = [&]() noexcept -> bool {
            const HWND fg = GetForegroundWindow();
            if (!fg)
                return false;
            const HWND rWnd = GetAncestor(hRoot, GA_ROOT);
            const HWND rFg = GetAncestor(fg, GA_ROOT);
            return rWnd && rWnd == rFg;
        }();

        if (!Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(hRoot))
        {
            RedrawWindow(hRoot, nullptr, nullptr,
                RDW_INVALIDATE | RDW_ERASE | RDW_ERASENOW | RDW_ALLCHILDREN | RDW_FRAME);
        }

        if (auto* provider = WndCompat::Root::GetRootEventProvider())
        {
            if (Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(hRoot))
            {
                if (Shared::RootPolicyQuery::UsesSwcaBackdrop())
                    WndCompat::ExplorerRoot::ApplyShellBackdropOnBackdropEnabled(hRoot);
                else if (provider->OnRootDwmBackdropApply)
                    provider->OnRootDwmBackdropApply(hRoot);
            }
            else if (Shared::RootPolicyQuery::UsesSwcaBackdrop())
            {
                WndCompat::RootBackdropRefreshThrottle::SetRootSwcaApplyReasonForNextCall(L"REAPPLY");
                if (isActive && provider->OnRootSwcaBackdropActiveApply)
                    provider->OnRootSwcaBackdropActiveApply(hRoot);
                else if (!isActive && provider->OnRootSwcaBackdropInactiveApply)
                    provider->OnRootSwcaBackdropInactiveApply(hRoot);
                else if (provider->OnRootSwcaBackdropActiveApply)
                    provider->OnRootSwcaBackdropActiveApply(hRoot);
            }
            else if (provider->OnRootDwmBackdropApply)
            {
                provider->OnRootDwmBackdropApply(hRoot);
            }
        }

        if (Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(hRoot))
        {
            RedrawWindow(hRoot, nullptr, nullptr,
                RDW_INVALIDATE | RDW_ERASE | RDW_ERASENOW | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_FRAME);
            WndCompat::ExplorerRoot::PostExplorerChromeRedrawIfShellRoot(hRoot);
            return;
        }

        RedrawWindow(hRoot, nullptr, nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
    }

    bool WINAPI Root_IsAttached(HWND hRoot) noexcept
    {
        std::lock_guard lock(g_rootWindowsMutex);
        return IsRootAttachedInternal(hRoot);
    }

    void Root_CopyTrackedRoots(std::vector<HWND>& out) noexcept
    {
        std::lock_guard lock(g_rootWindowsMutex);
        out = g_rootWindows;
    }

    void Root_Attach(HWND hRoot)
    {
        if (!IsWindow(hRoot))
            return;
        
        // Не устанавливать субкласс во время shutdown:
        // Root_DetachAll уже снял все субклассы, новые установки приведут
        // к висячим указателям после FreeLibrary.
        if (WndCompat::IsShuttingDown())
            return;

        wchar_t rootClassName[256]{};
        if (GetClassNameW(hRoot, rootClassName, static_cast<int>(_countof(rootClassName))) <= 0)
            rootClassName[0] = L'\0';

        // Панель задач — только TaskbarHost_Attach (без RootSubclassProc).
        if (_wcsicmp(rootClassName, L"Shell_TrayWnd") == 0
            || _wcsicmp(rootClassName, L"Shell_SecondaryTrayWnd") == 0)
            return;

        // Служебное HWND_MESSAGE ExplorerCompat — не субклассировать (мешает маршалингу XAML shutdown).
        if (wcscmp(rootClassName, ExplorerCompatMsg::kExplorerXamlShutdownWndClass) == 0)
            return;
        if (wcscmp(rootClassName, TaskbarCompatMsg::kTaskbarXamlShutdownWndClass) == 0)
            return;

        // Проверка под локом — предотвращает дублирование записи при одновременном
        // вызове из разных потоков (initial scan thread + WinEvent callback threads).
        if (!WndCompat::Root::ExplorerBridge::TryRegisterRootWindowForAttach(hRoot))
            return;

        GlassShared::GlassMarkRootBackdropAllowed(
            hRoot, Shared::RootPolicyQuery::IsRootGlassRenderingEnabled());

        if (!SetWindowSubclass(hRoot, RootSubclassProc, kRootSubclassId, 0))
        {
            WndCompat::Root::ExplorerBridge::UnregisterRootWindowAfterSubclassFailed(hRoot);

            DebugLog(std::format(
                L"[WndCompat][Root] Root_Attach: FAILED to subclass 0x{:X} class=\"{}\"",
                reinterpret_cast<std::uintptr_t>(hRoot),
                rootClassName
            ).c_str());
            return;
        }

        Shared::ThemeHelper::EnableWindowDarkModeUxTheme(
            hRoot, Shared::ThemeHelper::ResolveEffectiveDarkMode());

        if (WndCompat::WinEventHook_GetMode() == Shared::InjectionMode::Full
            && WndCompat::Root::ExplorerBridge::ShouldSetComctlScrollHostForRootClassName(rootClassName))
        {
            GlassShared::GlassMarkComctlScrollHost(hRoot);
        }

        if (Shared::RootPolicyQuery::IsRootGlassRenderingEnabled())
            WndCompat::Root::ExplorerBridge::ApplyRootClassBlackBrush(hRoot);

        // IAT-хуки по типу root-окна (ветка «window» BWM PaintHandler)
        if (auto* hp = WndCompat::Root::GetRootEventProvider())
        {
            if (hp->OnRootHooksAttach)
                hp->OnRootHooksAttach(hRoot);
        }

        // SetWindowSubclass успешен — выполняем коллбэки и перерисовку вне лока
        if (GetAncestor(hRoot, GA_ROOT) == hRoot
            && GlassShared::GlassIsBackdropAllowedForHwnd(hRoot))
        {
            GlassCore_TabChroma_RefreshFromTheme(nullptr);

            if (!ShouldSkipRootAttachSwca(hRoot))
            {
                auto provider = WndCompat::Root::GetRootEventProvider();
                if (provider)
                {
                    const bool isActive = [&]() noexcept -> bool {
                        const HWND fg = GetForegroundWindow();
                        if (!fg)
                            return false;
                        const HWND rWnd = GetAncestor(hRoot, GA_ROOT);
                        const HWND rFg = GetAncestor(fg, GA_ROOT);
                        return rWnd && rWnd == rFg;
                    }();

                    if (Shared::RootPolicyQuery::UsesSwcaBackdrop())
                    {
                        WndCompat::RootBackdropRefreshThrottle::SetRootSwcaApplyReasonForNextCall(L"ATTACH");
                        if (isActive && provider->OnRootSwcaBackdropActiveApply)
                            provider->OnRootSwcaBackdropActiveApply(hRoot);
                        else if (!isActive && provider->OnRootSwcaBackdropInactiveApply)
                            provider->OnRootSwcaBackdropInactiveApply(hRoot);
                        else if (provider->OnRootSwcaBackdropActiveApply)
                            provider->OnRootSwcaBackdropActiveApply(hRoot);
                        Shared::RootNcActiveCache::Set(hRoot, isActive);
                    }
                    else if (provider->OnRootDwmBackdropApply)
                    {
                        provider->OnRootDwmBackdropApply(hRoot);
                    }
                }

                RedrawWindow(hRoot, nullptr, nullptr,
                    RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
            }
        }

        DebugLog(std::format(
            L"[WndCompat][Root] Root_Attach: subclassed 0x{:X} class=\"{}\"",
            reinterpret_cast<std::uintptr_t>(hRoot),
            rootClassName
        ).c_str());

        if (Shared::RootBackdropPolicy::IsHwndBackdropExempt(hRoot))
            Shared::GlassHostRegistry::MarkSubtreeGlassDisabled(hRoot);

        WndCompat::BackdropPolicyRescan::ScheduleSubtreeRescanAfterRootAttach(hRoot);
    }

    void Root_Detach(HWND hRoot)
    {
        if (!IsWindow(hRoot))
            return;

        if (Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(hRoot))
        {
            ExplorerRoot::Detach(hRoot);
            return;
        }

        if (!WndCompat::Root::ExplorerBridge::RemoveTrackedRootAndDecrementBrush(hRoot))
            return;

        if (auto* hp = WndCompat::Root::GetRootEventProvider())
        {
            if (hp->OnRootHooksDetach)
                hp->OnRootHooksDetach(hRoot);
        }

        // RemoveWindowSubclass требует вызова строго из потока-владельца окна.
        const DWORD curTid = GetCurrentThreadId();
        const DWORD wndTid = GetWindowThreadProcessId(hRoot, nullptr);

        if (wndTid != curTid)
        {
            DebugLog(DBG_WARNING,
                L"[WndCompat][Root] Root_Detach: cross-thread hwnd=0x%p — skip RemoveWindowSubclass",
                hRoot);
            return;
        }

        if (!RemoveWindowSubclass(hRoot, RootSubclassProc, kRootSubclassId))
        {
            DebugLog(std::format(
                L"[WndCompat][Root] Root_Detach: RemoveWindowSubclass FAILED for 0x{:X}",
                reinterpret_cast<std::uintptr_t>(hRoot)
            ).c_str());
        }
        else
        {
            GlassShared::GlassUnmarkWindow(hRoot);
            GlassShared::GlassClearComctlScrollHost(hRoot);
            DebugLog(std::format(
                L"[WndCompat][Root] Root_Detach: 0x{:X}",
                reinterpret_cast<std::uintptr_t>(hRoot)
            ).c_str());
        }

    }

    void Root_DetachAll()
    {
        std::vector<HWND> windows;
        std::unordered_map<ATOM, WndCompat::Root::ExplorerBridge::ClassBrushEntry> classBrushes;
        {
            std::lock_guard lock(g_rootWindowsMutex);
            windows = std::move(g_rootWindows);
            classBrushes = std::move(g_classBrushes);
        }
        SubclassDiag::PublishKindCount(GlassShared::GlassControlKind::Root, 0);

        const DWORD curTid = GetCurrentThreadId();

        ExplorerRoot::DetachAll(windows, classBrushes, curTid);

        unsigned sameOk = 0;
        unsigned crossOk = 0;
        unsigned crossFail = 0;
        unsigned removeFail = 0;

        for (HWND hWnd : windows)
        {
            if (!IsWindow(hWnd))
                continue;

            const DWORD wndTid = GetWindowThreadProcessId(hWnd, nullptr);
            const bool isRoot = (GetAncestor(hWnd, GA_ROOT) == hWnd);

            if (wndTid != curTid)
            {
                DWORD_PTR sendResult = 0;
                // SMTO_BLOCK намеренно не используется: если поток во время
                // обработки WM_GLASS_DETACH_ROOT отправит сообщение обратно в наш поток,
                // SMTO_BLOCK вызвал бы дедлок. SMTO_ABORTIFHUNG достаточен.
                const BOOL sent = SendMessageTimeoutW(
                    hWnd, WM_GLASS_DETACH_ROOT, 0, 0,
                    SMTO_ABORTIFHUNG, 1000, &sendResult);

                if (!sent)
                {
                    ++crossFail;
                    DebugLog(DBG_WARNING,
                        L"[WndCompat][Root] Root_DetachAll: cross-thread SendMessage timeout/fail "
                        L"hwnd=0x%p — субкласс остался на окне",
                        hWnd);
                }
                else
                {
                    ++crossOk;
                    DebugLog(
                        L"[WndCompat][Root] Root_DetachAll: cross-thread detached via SendMessage "
                        L"hwnd=0x%p",
                        hWnd);
                }

                WndCompat::Root::ExplorerBridge::RestoreOriginalClassBrush(hWnd, classBrushes);

                if (isRoot && sent)
                {
                    const bool hadGlassBackdrop =
                        Shared::RootBackdropPolicy::DidGlassApplyRootBackdrop(hWnd);
                    auto provider = WndCompat::Root::GetRootEventProvider();
                    if (provider && provider->OnRootDetach)
                        provider->OnRootDetach(hWnd);

                    if (hadGlassBackdrop)
                        RestoreRootNcAfterDetachImpl(hWnd);
                }
                continue;
            }

            if (auto* hp = WndCompat::Root::GetRootEventProvider())
            {
                if (hp->OnRootHooksDetach)
                    hp->OnRootHooksDetach(hWnd);
            }

            if (!RemoveWindowSubclass(hWnd, RootSubclassProc, kRootSubclassId))
            {
                ++removeFail;
                DebugLog(std::format(
                    L"[WndCompat][Root] Root_DetachAll: RemoveWindowSubclass FAILED for 0x{:X}",
                    reinterpret_cast<std::uintptr_t>(hWnd)
                ).c_str());
            }
            else
            {
                ++sameOk;
                DebugLog(std::format(
                    L"[WndCompat][Root] Root_DetachAll: detached 0x{:X}",
                    reinterpret_cast<std::uintptr_t>(hWnd)
                ).c_str());
            }

            WndCompat::Root::ExplorerBridge::RestoreOriginalClassBrush(hWnd, classBrushes);

            // Same-thread: субкласс снят выше, восстанавливаем дефолтную отрисовку
            if (isRoot)
            {
                const bool hadGlassBackdrop =
                    Shared::RootBackdropPolicy::DidGlassApplyRootBackdrop(hWnd);
                auto provider = WndCompat::Root::GetRootEventProvider();
                if (provider && provider->OnRootDetach)
                    provider->OnRootDetach(hWnd);

                if (hadGlassBackdrop)
                    RestoreRootNcAfterDetachImpl(hWnd);
            }
        }


        DebugLog(
            DBG_IMPORTANT,
            L"[WndCompat][Root] Root_DetachAll: sameOk=%u crossOk=%u crossFail=%u removeFail=%u total=%zu",
            sameOk,
            crossOk,
            crossFail,
            removeFail,
            windows.size());
    }
}

extern "C" __declspec(dllexport) void WINAPI WndCompat_MarshalApplyRootBackdropReapplyForHwnd(HWND hRoot) noexcept
{
    WndCompat::MarshalApplyRootBackdropReapplyForHwnd(hRoot);
}

extern "C" __declspec(dllexport) void WINAPI WndCompat_ApplyBackdropPolicyToTrackedRoots() noexcept
{
    WndCompat::ApplyBackdropPolicyToTrackedRoots();
}

extern "C" __declspec(dllexport) void WINAPI WndCompat_NudgeExplorerShellNativeChrome(HWND hRoot) noexcept
{
    WndCompat::ExplorerRoot::PostExplorerChromeRedrawIfShellRoot(hRoot);
}

extern "C" __declspec(dllexport) void WINAPI WndCompat_SyncExplorerShellChromeRedrawAll() noexcept
{
    WndCompat::ExplorerRoot::SyncExplorerShellChromeRedrawAll();
}

extern "C" __declspec(dllexport) void WINAPI WndCompat_SyncExplorerShellRootRedrawAll() noexcept
{
    WndCompat::ExplorerRoot::SyncExplorerShellRootRedrawAll();
}

namespace WndCompat
{
    /// g_rootWindows ∩ DidGlassApplyRootBackdrop (SetProp; cross-DLL).
    /// SnapshotGlassAppliedRootHwnds из GlassShared static lib — отдельная копия в WndCompat vs GlassCore.
    void CopyTrackedRootsWithAppliedBackdrop(std::vector<HWND>& out) noexcept
    {
        out.clear();
        std::vector<HWND> tracked;
        Root_CopyTrackedRoots(tracked);
        out.reserve(tracked.size());
        for (HWND hRoot : tracked)
        {
            if (!hRoot || !IsWindow(hRoot))
                continue;
            if (!Shared::RootBackdropPolicy::DidGlassApplyRootBackdrop(hRoot))
                continue;
            out.push_back(hRoot);
        }
    }

    void PostLiveRootBackdropReapplyImpl(RootLiveBackdropScope scope) noexcept
    {
        if (!Shared::RootPolicyQuery::IsRootGlassRenderingEnabled())
            return;

        std::vector<HWND> windows;
        if (scope == RootLiveBackdropScopeAppliedOnly)
            CopyTrackedRootsWithAppliedBackdrop(windows);
        else
            Root_CopyTrackedRoots(windows);

        for (HWND hRoot : windows)
        {
            if (!hRoot || !IsWindow(hRoot))
                continue;

            wchar_t cls[256]{};
            if (::GetClassNameW(hRoot, cls, static_cast<int>(_countof(cls))) > 0)
            {
                if (_wcsicmp(cls, L"Shell_TrayWnd") == 0
                    || _wcsicmp(cls, L"Shell_SecondaryTrayWnd") == 0)
                {
                    DispatchTrayReapplyBackdropToOwnerThread(hRoot);
                    continue;
                }
            }

            (void)::PostMessageW(hRoot, WndCompat::WM_GLASS_REAPPLY_ROOT_BACKDROP, 0, 0);
        }
    }

    void PostLiveRootBackdropReapply(RootLiveBackdropScope scope) noexcept
    {
        PostLiveRootBackdropReapplyImpl(scope);
    }

    void PostLiveRootColorizationToAppliedRootsImpl(const LPARAM liveRootScope) noexcept
    {
        if (!Shared::RootPolicyQuery::IsRootGlassRenderingEnabled())
            return;

        std::vector<HWND> windows;
        CopyTrackedRootsWithAppliedBackdrop(windows);
        for (HWND hRoot : windows)
            (void)::PostMessageW(hRoot, WM_DWMCOLORIZATIONCHANGED, 0, liveRootScope);
    }

    void PostLiveRootColorizationToAppliedRoots(const LPARAM liveRootScope) noexcept
    {
        PostLiveRootColorizationToAppliedRootsImpl(liveRootScope);
    }
} // namespace WndCompat

extern "C" __declspec(dllexport) void WINAPI WndCompat_CopyTrackedRoots(
    HWND* outHwnds,
    DWORD* inOutCount) noexcept
{
    if (!inOutCount)
        return;

    std::vector<HWND> windows;
    WndCompat::Root_CopyTrackedRoots(windows);

    if (!outHwnds)
    {
        *inOutCount = static_cast<DWORD>(windows.size());
        return;
    }

    const DWORD capacity = *inOutCount;
    const DWORD toCopy = static_cast<DWORD>(std::min<std::size_t>(windows.size(), capacity));
    for (DWORD i = 0; i < toCopy; ++i)
        outHwnds[i] = windows[i];
    *inOutCount = toCopy;
}

extern "C" __declspec(dllexport) void WINAPI WndCompat_PostLiveRootBackdropReapply(DWORD scope) noexcept
{
    const auto scoped = scope == static_cast<DWORD>(WndCompat::RootLiveBackdropScopeAppliedOnly)
        ? WndCompat::RootLiveBackdropScopeAppliedOnly
        : WndCompat::RootLiveBackdropScopeAllTracked;
    WndCompat::PostLiveRootBackdropReapply(scoped);
}

extern "C" __declspec(dllexport) void WINAPI WndCompat_PostLiveRootColorizationToAppliedRoots(
    const LPARAM liveRootScope) noexcept
{
    WndCompat::PostLiveRootColorizationToAppliedRoots(liveRootScope);
}