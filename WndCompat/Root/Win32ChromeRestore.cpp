#include "WndPch.h"
#include "Root/Win32ChromeRestore.h"
#include "Root/ExplorerRootSubclass.h"
#include "Root/RootSubclass.h"
#include "Root/TaskbarHost.h"

#include <Controls/ButtonSubclass.h>
#include <Controls/DateTimePickerSubclass.h>
#include <Controls/DirectUISubclass.h>
#include <Controls/EditSubclass.h>
#include <Controls/HeaderSubclass.h>
#include <Controls/ListViewSubclass.h>
#include <Controls/MonthCalSubclass.h>
#include <Controls/NetUISubclass.h>
#include <Controls/RebarSubclass.h>
#include <Controls/ScrollBarHost.h>
#include <Controls/StaticSubclass.h>
#include <Controls/TabControlSubclass.h>
#include <Controls/ToolbarSubclass.h>
#include <Controls/TrackBarHost.h>
#include <Controls/TreeViewSubclass.h>
#include <Controls/UpDownHost.h>
#include <Explorer/ExplorerIslandContainerSubclass.h>
#include <MenuEventProvider.h>
#include <Mshtml/IeServerHost.h>
#include <Popup/ComboLBox/ComboBoxSubclass.h>
#include <Popup/ComboLBox/ComboLBoxSubclass.h>
#include <Popup/Menu/MenuOwnerSubclass.h>
#include <Popup/Tooltip/KbxLabelSubclass.h>
#include <Popup/Tooltip/TooltipSubclass.h>
#include "Root/RootExplorerSubclassBridge.h"
#include <RootBackdropPolicy.h>
#include <RootEventProvider.h>
#include <RootPolicyQuery.h>
#include <GlassBackdropMark.h>
#include <WndCompat.h>
#include <WinUiBridge/WinUiContentBridgeHost.h>
#include <WinUiBridge/WinUiDesktopBridgeHost.h>
#include <WinUiBridge/CoreWindowHost.h>

namespace WndCompat
{
    void NotifyExplorerRootBackdropPolicyChanged(bool allowed) noexcept
    {
        using NotifyFn = void(WINAPI*)(BOOL);
        const BOOL enabled = allowed ? TRUE : FALSE;

        if (const HMODULE hGlassCompat = GetModuleHandleW(L"GlassCompat.dll"))
        {
            const auto pfnGlassCompat = reinterpret_cast<NotifyFn>(
                GetProcAddress(hGlassCompat, "GlassCompat_NotifyExplorerRootBackdropPolicyChanged"));
            if (pfnGlassCompat)
            {
                pfnGlassCompat(enabled);
                return;
            }

            DebugLog(
                DBG_WARNING,
                L"[WndCompat][Win32Chrome] NotifyExplorerCompat: GlassCompat export not found (allowed=%d)",
                allowed ? 1 : 0);
        }

        const HMODULE hExplorerCompat = GetModuleHandleW(L"ExplorerCompat.dll");
        if (hExplorerCompat)
        {
            const auto pfnExplorer = reinterpret_cast<NotifyFn>(
                GetProcAddress(hExplorerCompat, "ExplorerCompat_OnRootBackdropPolicyChanged"));
            if (pfnExplorer)
            {
                pfnExplorer(enabled);
                return;
            }

            DebugLog(
                DBG_WARNING,
                L"[WndCompat][Win32Chrome] NotifyExplorerCompat: ExplorerCompat export not found (allowed=%d)",
                allowed ? 1 : 0);
        }

        DebugLog(
            DBG_WARNING,
            L"[WndCompat][Win32Chrome] NotifyExplorerRootBackdropPolicyChanged: no route (allowed=%d)",
            allowed ? 1 : 0);
    }

    void RestoreRootWin32Chrome(HWND hRoot, Win32ChromeRestoreScope scope) noexcept
    {
        if (scope == Win32ChromeRestoreScope::ShutdownDetach)
            return;

        if (!hRoot || !IsWindow(hRoot))
            return;
        if (GetAncestor(hRoot, GA_ROOT) != hRoot)
            return;
        if (IsShuttingDown())
            return;

        GlassShared::GlassMarkRootBackdropAllowed(hRoot, false);

        HMODULE hCore = GetModuleHandleW(L"GlassCore.dll");
        using TransitionFn = void(WINAPI*)(HWND);
        const auto pfnTransition = hCore
            ? reinterpret_cast<TransitionFn>(
                GetProcAddress(hCore, "GlassCore_ApplyRootBackdropPolicyTransitionForHwnd"))
            : nullptr;

        if (Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(hRoot))
        {
            if (pfnTransition)
                pfnTransition(hRoot);
            ExplorerRoot::RedrawExplorerShellRootFull(hRoot);
            return;
        }

        MenuOwner_ReleaseClassicMenuBarHold(hRoot);
        Root::ExplorerBridge::RestoreRootClassBrushForHwnd(hRoot);
        if (Shared::RootBackdropPolicy::DidGlassApplyRootBackdrop(hRoot))
            Root::ExplorerBridge::RestoreRootNcAfterDetach(hRoot);

        if (auto* hp = Root::GetRootEventProvider())
        {
            if (hp->OnRootHooksDetach)
                hp->OnRootHooksDetach(hRoot);
        }
        if (pfnTransition)
            pfnTransition(hRoot);
    }

    void RestoreTrackedRootsWin32Chrome(Win32ChromeRestoreScope scope) noexcept
    {
        if (scope == Win32ChromeRestoreScope::ShutdownDetach)
            return;

        std::vector<HWND> windows;
        Root_CopyTrackedRoots(windows);
        for (HWND hRoot : windows)
        {
            if (!hRoot || !IsWindow(hRoot))
                continue;
            MarshalApplyRootBackdropReapplyForHwnd(hRoot);
        }
    }

    void RestoreMenuBarWin32Chrome(Win32ChromeRestoreScope scope) noexcept
    {
        if (scope != Win32ChromeRestoreScope::LiveNoBackdrop)
            return;

        if (const auto* menuProvider = WndCompat::Menu::GetMenuEventProvider();
            menuProvider && menuProvider->OnMenuBarBackdropPolicyDisabled)
        {
            menuProvider->OnMenuBarBackdropPolicyDisabled();
        }
    }

    void RestoreTrackedControlsWin32Chrome(Win32ChromeRestoreScope scope) noexcept
    {
        if (scope == Win32ChromeRestoreScope::LiveNoBackdrop)
        {
            TreeView_ApplyBackdropPolicyToTracked();
            ListView_ApplyBackdropPolicyToTracked();
            Button_ApplyBackdropPolicyToTracked();
            DirectUI_ApplyBackdropPolicyToTracked();
            Rebar_ApplyBackdropPolicyToTracked();
            ToolBar_ApplyBackdropPolicyToTracked();
            ScrollBar_ApplyBackdropPolicyToTracked();
            TrackBar_ApplyBackdropPolicyToTracked();
            ExplorerIslandContainer_ApplyBackdropPolicyToTracked();
            return;
        }

        Tooltip_DetachAll();
        KbxLabel_DetachAll();
        DateTimePicker_DetachAll();
        MonthCal_DetachAll();
        ComboBox_DetachAll();
        ComboLBox_DetachAll();
        Button_DetachAll();
        DirectUI_DetachAll();
        NetUI_DetachAll();
        NetUiToolWindow_DetachAll();
        TreeView_DetachAll();
        TabControl_DetachAll();
        TrackBar_DetachAll();
        UpDown_DetachAll();
        Header_DetachAll();
        ListView_DetachAll();
        Rebar_DetachAll();
        ToolBar_DetachAll();
        Edit_DetachAll();
        RichEdit_DetachAll();
        Static_DetachAll();
        IeServer_DetachAll();
        WinUiContentBridge_DetachAll();
        WinUiDesktopBridge_DetachAll();
        CoreWindowHost_DetachAll();
        ScrollBar_DetachAll();
        ExplorerIslandContainer_DetachAll();
        Root_DetachAll();
    }

    void RestoreWin32Chrome(Win32ChromeRestoreScope scope) noexcept
    {
        DebugLog(
            DBG_IMPORTANT,
            L"[WndCompat][Win32Chrome] RestoreWin32Chrome scope=%s",
            scope == Win32ChromeRestoreScope::ShutdownDetach ? L"ShutdownDetach" : L"LiveNoBackdrop");

        if (scope == Win32ChromeRestoreScope::LiveNoBackdrop)
        {
            NotifyExplorerRootBackdropPolicyChanged(false);
            RestoreTrackedRootsWin32Chrome(scope);
            RestoreMenuBarWin32Chrome(scope);
            RestoreTrackedControlsWin32Chrome(scope);
            return;
        }

        TaskbarHost_DetachAll();
        RestoreTrackedControlsWin32Chrome(scope);
    }
}
