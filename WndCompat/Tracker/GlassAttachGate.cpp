#include "WndPch.h"
#include "Tracker/GlassAttachGate.h"
#include "Tracker/WinEventHandler.h"
#include "WindowClassUtils.h"
#include <ExplorerCompatHostPolicy.h>
#include <GlassHostRegistry.h>
#include <RootBackdropPolicy.h>
#include <cstdio>

namespace WndCompat::GlassAttachGate
{
    bool ShouldAttachGlassToHwnd(HWND hWnd) noexcept
    {
        if (WndCompat::WinEventHook_GetMode() != Shared::InjectionMode::Full)
            return true;

        if (!hWnd || !IsWindow(hWnd))
            return true;

        // ComboLBox — отдельное top-level popup списка комбобокса; не наследует запрет поддерева
        // от exempt root (Rainmeter и т.п.). Единственный запрет — отсутствие инжекции в процесс.
        if (WndCompat::IsWindowClass(hWnd, L"ComboLBox"))
            return true;

        {
            const auto cls = Shared::RootBackdropPolicy::NormalizeWindowClass(hWnd);
            if (Shared::ExplorerCompatHostPolicy::IsHostWindowClass(cls)
                && !Shared::ExplorerCompatHostPolicy::ShouldWndCompatHandleHostWindow(cls))
                return false;
        }

        const HWND hRoot = GetAncestor(hWnd, GA_ROOT);
        if (!hRoot || !IsWindow(hRoot))
            return true;

        // CoreWindow / ControlCenterWindow: carve-out ТОЛЬКО в NC/Start/Search host
        // (иначе explorer Root-path). ControlCenterWindow — ShellHost Quick Settings.
        if (hWnd == hRoot
            && (WndCompat::IsWindowClass(hWnd, L"Windows.UI.Core.CoreWindow")
                || WndCompat::IsWindowClass(hWnd, L"ControlCenterWindow")))
        {
            if (WndCompat::IsWindowClass(hWnd, L"ControlCenterWindow"))
                return Shared::RootBackdropPolicy::IsShellNotificationHostProcess();
            return Shared::RootBackdropPolicy::IsCoreWindowHostProcess();
        }

        if (Shared::GlassHostRegistry::IsSubtreeGlassDisabled(hRoot))
            return false;

        if (Shared::RootBackdropPolicy::IsHwndBackdropExempt(hRoot))
        {
            // Под NC-surface не кэшируем subtree-disable — дети просто return false.
            if (!WndCompat::IsWindowClass(hRoot, L"Windows.UI.Core.CoreWindow")
                && !WndCompat::IsWindowClass(hRoot, L"ControlCenterWindow"))
                Shared::GlassHostRegistry::MarkSubtreeGlassDisabled(hRoot);
            return false;
        }

        return true;
    }
}
