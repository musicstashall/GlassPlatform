#pragma once

#include "WndPch.h"
#include <InjectionMode.h>
#include <RootPolicyQuery.h>
#include <vector>

namespace WndCompat
{
    void DispatchTrayAttachToOwnerThread(HWND hTrayRoot) noexcept;
    void DispatchTrayDetachToOwnerThread(HWND hWnd) noexcept;
    void DispatchTrayReapplyBackdropToOwnerThread(HWND hTrayRoot) noexcept;

    void DispatchRootAttachToOwnerThread(HWND hRoot) noexcept;
    void DispatchExplorerRootAttachToOwnerThread(HWND hRoot) noexcept;

    /// WH_GETMESSAGE для cross-thread PostMessage glass attach/detach.
    void EnsureGetMsgHookForHwnd(HWND hWnd) noexcept;

    bool WinEventHook_Initialize(Shared::InjectionMode mode);
    void WinEventHook_Shutdown();
    /// <summary>Host-процесс завершается: unhook без каскада *_DetachAll.</summary>
    void WinEventHook_ShutdownHostExit();
    void WinEventHook_Uninstall();
    bool WinEventHook_IsInstalled();
    Shared::InjectionMode WinEventHook_GetMode();
    void WinEventHook_SetMode(Shared::InjectionMode mode) noexcept;

    /// NoBackdrop + InjectionMode Full (локально в WndCompat; не static-lib flag).
    [[nodiscard]] inline bool IsGlassVisualsAllowed() noexcept
    {
        return Shared::RootPolicyQuery::IsRootGlassRenderingEnabled()
            && WinEventHook_GetMode() == Shared::InjectionMode::Full;
    }

    /// Live AttachList: Full→не-Full — LiveNoBackdrop strip; не-Full→Full — initial-style scan + apply.
    /// Порядок относительно SetInjectionMode задаёт GlassCompat (strip до смены mode, scan после).
    void WinEventHook_ApplyInjectionModeVisuals(
        Shared::InjectionMode oldMode,
        Shared::InjectionMode newMode) noexcept;

    /// Live ON: catch-up *_Attach для дочерних HWND (делегирует в BackdropPolicyRescan).
    void RescanMissingControlsUnderTrackedRoots(const std::vector<HWND>& roots) noexcept;
}
