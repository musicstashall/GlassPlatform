#pragma once
#include <windows.h>

namespace WndCompat
{
    [[nodiscard]] bool WinUi_IsUnderShellTrayRoot(HWND hWnd) noexcept;

    // Tray trackbar Full — маркер готовности XAML острова задачи для TaskbarCompat.
    void WinUi_TrayTrackBar_ReportIxdeIntent(HWND hWnd) noexcept;
    void WinUi_TrayTrackBar_ReleaseIxdeIntent(HWND hWnd) noexcept;

    void WinUi_ResetTaskbarCompatNotifyProcCache() noexcept;

    void WinUi_OnTaskbarIxdeOutcome(BOOL succeeded) noexcept;

    // UWP: Windows.UI.Composition.DesktopWindowContentBridge под Shell_TrayWnd — без субкласса HWND.
    void WinUiContentBridge_Attach(HWND hWnd) noexcept;
    void WinUiContentBridge_Detach(HWND hWnd) noexcept;
    void WinUiContentBridge_DetachAll() noexcept;
    // После загрузки TaskbarCompat в explorer — доставить отложенные hwnd из очереди (см. GlassCompat DoInitialize).
    void WinUiContentBridge_FlushPendingAfterTaskbarCompatReady() noexcept;

    // Поздний мост вторичной панели / холодного explorer: без повторной полной очереди Flush.
    void WinUiContentBridge_RescanShellTraysDesktopBridges() noexcept;
}
