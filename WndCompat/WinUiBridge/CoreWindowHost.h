#pragma once

#include <Windows.h>

namespace WndCompat
{
    // NC surfaces без subclass HWND: CoreWindow (SEH/SMEH/Search) +
    // ControlCenterWindow (ShellHost Quick Settings). → UICompat IXDE.
    // Lazy-load UICompat → NotifyCoreWindow (IXDE interest).
    void CoreWindowHost_Attach(HWND hWnd) noexcept;
    void CoreWindowHost_Detach(HWND hWnd) noexcept;
    void CoreWindowHost_DetachAll() noexcept;
    void CoreWindowHost_FlushPendingAfterUICompatReady() noexcept;
    // S3: H32 не постить Special на scan — Attach всех живых CoreWindow после UICompat.
    void CoreWindowHost_AttachExistingInProcess() noexcept;
    // После успешного IXDE: повторный Notify по уже прикреплённым CoreWindow (первый был ixdeOk:false).
    void CoreWindowHost_OnIxdeReady() noexcept;
    void CoreWindowHost_ResetUICompatNotifyProcCache() noexcept;
}
