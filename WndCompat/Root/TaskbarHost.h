#pragma once

#include "WndPch.h"

namespace WndCompat
{
    // Shell_*TrayWnd: backdrop/SWCA через RootEventProvider, без SetWindowSubclass (*Host.cpp).
    void TaskbarHost_Attach(HWND hTrayRoot) noexcept;
    void TaskbarHost_Detach(HWND hTrayRoot) noexcept;
    void TaskbarHost_DetachAll() noexcept;
    /// Явный reload (/reloadRender): SWCA для tracked Shell_*TrayWnd, не из sweep Root-окон.
    void TaskbarHost_ReapplyBackdropToTracked() noexcept;
    /// GetMsgHookProc → после SWCA/CornerType на потоке владельца Shell_*TrayWnd.
    void TaskbarHost_HandleReapplyBackdropMessage(HWND hTrayRoot) noexcept;
    [[nodiscard]] bool TaskbarHost_IsTracked(HWND hTrayRoot) noexcept;
}
