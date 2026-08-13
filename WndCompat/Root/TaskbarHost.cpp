// Shell_TrayWnd / Shell_SecondaryTrayWnd: backdrop через RootEventProvider, без RootSubclassProc (*Host).
#include "WndPch.h"
#include "Root/TaskbarHost.h"

#include <WndCompat.h>
#include <GlassMessages.h>
#include <GlassControlKind.h>
#include <GlassComctlScrollHost.h>
#include <RootEventProvider.h>
#include <RootBackdropPolicy.h>
#include <GlassBackdropMark.h>
#include <GlassHostRegistry.h>
#include <DebugLog.h>
#include <InjectionMode.h>
#include <SystemHelper.h>

#include "Root/RootExplorerSubclassBridge.h"
#include "Root/RootBackdropRefreshThrottle.h"
#include "Tracker/WinEventHandler.h"

#include <algorithm>
#include <mutex>
#include <vector>

namespace
{
    std::mutex        g_taskbarTrayMx;
    std::vector<HWND> g_taskbarTrayHwnds;

    [[nodiscard]] bool IsShellTrayTopLevelClass(HWND hWnd, wchar_t* outClassFirst,
                                                int capacity) noexcept
    {
        if (!hWnd || !IsWindow(hWnd))
            return false;
        if (!outClassFirst || capacity <= 1)
            return false;
        if (GetAncestor(hWnd, GA_ROOT) != hWnd)
            return false;
        if (::GetClassNameW(hWnd, outClassFirst, capacity) <= 0)
        {
            outClassFirst[0] = L'\0';
            return false;
        }
        return (_wcsicmp(outClassFirst, L"Shell_TrayWnd") == 0
                || _wcsicmp(outClassFirst, L"Shell_SecondaryTrayWnd") == 0);
    }

    void TaskbarHost_ApplyBackdrop(HWND hRoot, const wchar_t* applyReason) noexcept
    {
        if (GetAncestor(hRoot, GA_ROOT) != hRoot)
            return;

        if (auto* hp = WndCompat::Root::GetRootEventProvider())
        {
            if (hp->OnRootWmThemeChangedForTabChroma)
                hp->OnRootWmThemeChangedForTabChroma(nullptr);
        }

        auto provider = WndCompat::Root::GetRootEventProvider();
        if (provider && provider->OnRootSwcaBackdropActiveApply)
        {
            WndCompat::RootBackdropRefreshThrottle::SetRootSwcaApplyReasonForNextCall(applyReason);
            provider->OnRootSwcaBackdropActiveApply(hRoot);
        }
    }

    void TaskbarHost_TeardownProvidersImpl(HWND hWnd, bool requireOwnerThread) noexcept
    {
        if (requireOwnerThread)
        {
            const DWORD curTid = GetCurrentThreadId();
            const DWORD wndTid = GetWindowThreadProcessId(hWnd, nullptr);
            if (wndTid != curTid)
            {
                DebugLog(
                    DBG_WARNING,
                    L"[WndCompat][TaskbarHost] teardown: wrong thread hwnd=0x%p skip",
                    hWnd);
                return;
            }
        }

        if (auto* hp = WndCompat::Root::GetRootEventProvider())
        {
            if (hp->OnRootHooksDetach)
                hp->OnRootHooksDetach(hWnd);
        }

        const bool hadGlassBackdrop = Shared::RootBackdropPolicy::DidGlassApplyRootBackdrop(hWnd);
        auto provider = WndCompat::Root::GetRootEventProvider();
        if (provider && provider->OnRootDetach)
            provider->OnRootDetach(hWnd);

        if (hadGlassBackdrop)
            WndCompat::Root::ExplorerBridge::RestoreRootNcAfterDetach(hWnd);

        GlassShared::GlassUnmarkWindow(hWnd);
        GlassShared::GlassClearComctlScrollHost(hWnd);
    }

    void TaskbarHost_TeardownProviders(HWND hWnd) noexcept
    {
        TaskbarHost_TeardownProvidersImpl(hWnd, true);
    }

    void TaskbarHost_TeardownProvidersShutdownAnyThread(HWND hWnd) noexcept
    {
        TaskbarHost_TeardownProvidersImpl(hWnd, false);
    }
}

namespace WndCompat
{
    bool TaskbarHost_IsTracked(HWND hTrayRoot) noexcept
    {
        if (!hTrayRoot)
            return false;
        std::lock_guard lock(g_taskbarTrayMx);
        return std::find(g_taskbarTrayHwnds.begin(), g_taskbarTrayHwnds.end(), hTrayRoot)
            != g_taskbarTrayHwnds.end();
    }

    void TaskbarHost_HandleReapplyBackdropMessage(HWND hTrayRoot) noexcept
    {
        if (!hTrayRoot || !IsWindow(hTrayRoot))
            return;
        if (WndCompat::IsShuttingDown())
            return;

        GlassShared::GlassMarkRootBackdropAllowed(hTrayRoot, true);
        TaskbarHost_ApplyBackdrop(hTrayRoot, L"RELOAD");

        SetWindowPos(
            hTrayRoot,
            nullptr,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }

    void TaskbarHost_Attach(HWND hTrayRoot) noexcept
    {
        if (WinEventHook_GetMode() != Shared::InjectionMode::Full)
            return;
        if (!IsWindow(hTrayRoot))
            return;
        if (WndCompat::IsShuttingDown())
            return;

        wchar_t cls[256]{};
        if (!IsShellTrayTopLevelClass(hTrayRoot, cls, static_cast<int>(_countof(cls))))
            return;

        if (wcscmp(cls, ExplorerCompatMsg::kExplorerXamlShutdownWndClass) == 0)
            return;
        if (wcscmp(cls, TaskbarCompatMsg::kTaskbarXamlShutdownWndClass) == 0)
            return;

        // Zone1 DLL: только Win11 + процесс с Shell_*TrayWnd (живой WinEvent / initial scan).
        if (Shared::SystemHelper::g_buildNumber >= 22000)
        {
            if (const HMODULE hGlassCompat = GetModuleHandleW(L"GlassCompat.dll"))
            {
                using EnsureFn = void(WINAPI*)() noexcept;
                if (const auto pfnEnsure = reinterpret_cast<EnsureFn>(
                        GetProcAddress(hGlassCompat, "GlassCompat_EnsureTaskbarCompatLoaded")))
                {
                    pfnEnsure();
                }
            }
        }

        {
            std::lock_guard lock(g_taskbarTrayMx);
            if (std::find(g_taskbarTrayHwnds.begin(), g_taskbarTrayHwnds.end(), hTrayRoot)
                != g_taskbarTrayHwnds.end())
                return;
            g_taskbarTrayHwnds.push_back(hTrayRoot);
        }

        if (auto* hp = WndCompat::Root::GetRootEventProvider())
        {
            if (hp->OnRootHooksAttach)
                hp->OnRootHooksAttach(hTrayRoot);
        }

        GlassShared::GlassMarkRootBackdropAllowed(hTrayRoot, true);
        TaskbarHost_ApplyBackdrop(hTrayRoot, L"ATTACH");

        DebugLog(
            DBG_IMPORTANT,
            L"[WndCompat][TaskbarHost] Attach hwnd=0x%p class=\"%s\"",
            hTrayRoot,
            cls);

        if (Shared::RootBackdropPolicy::IsHwndBackdropExempt(hTrayRoot))
            Shared::GlassHostRegistry::MarkSubtreeGlassDisabled(hTrayRoot);
    }

    void TaskbarHost_Detach(HWND hTrayRoot) noexcept
    {
        bool hadTracked = false;
        {
            std::lock_guard lock(g_taskbarTrayMx);
            const auto it =
                std::find(g_taskbarTrayHwnds.begin(), g_taskbarTrayHwnds.end(), hTrayRoot);
            if (it != g_taskbarTrayHwnds.end())
            {
                g_taskbarTrayHwnds.erase(it);
                hadTracked = true;
            }
        }

        if (!hadTracked && !IsWindow(hTrayRoot))
            return;

        if (!hadTracked)
            return;

        TaskbarHost_TeardownProviders(hTrayRoot);

        DebugLog(DBG_IMPORTANT, L"[WndCompat][TaskbarHost] Detach hwnd=0x%p", hTrayRoot);
    }

    void TaskbarHost_DetachAll() noexcept
    {
        std::vector<HWND> windows;
        {
            std::lock_guard lock(g_taskbarTrayMx);
            windows.swap(g_taskbarTrayHwnds);
        }

        for (HWND hWnd : windows)
        {
            if (!IsWindow(hWnd))
                continue;
            TaskbarHost_TeardownProvidersShutdownAnyThread(hWnd);
        }
    }

    void TaskbarHost_ReapplyBackdropToTracked() noexcept
    {
        if (WinEventHook_GetMode() != Shared::InjectionMode::Full)
            return;
        if (WndCompat::IsShuttingDown())
            return;

        std::vector<HWND> windows;
        {
            std::lock_guard lock(g_taskbarTrayMx);
            windows = g_taskbarTrayHwnds;
        }

        DebugLog(DBG_IMPORTANT,
            L"[WndCompat][TaskbarHost] ReapplyBackdropToTracked: count=%zu",
            windows.size());

        for (HWND hTrayRoot : windows)
        {
            if (!IsWindow(hTrayRoot))
                continue;
            WndCompat::DispatchTrayReapplyBackdropToOwnerThread(hTrayRoot);
        }
    }
}