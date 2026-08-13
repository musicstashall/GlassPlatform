#pragma once
#include <windows.h>

namespace WndCompat
{
    // WinUI island host (Microsoft.UI.Content.DesktopChildSiteBridge): без субкласса HWND.
    // Attach вызывает ExplorerCompat_NotifyDesktopBridgeWindow → глобальный UDK (InstallUdk → SetSite → Advise).
    void WinUiDesktopBridge_Attach(HWND hWnd) noexcept;
    void WinUiDesktopBridge_Detach(HWND hWnd) noexcept;
    void WinUiDesktopBridge_DetachAll() noexcept;
    // После загрузки ExplorerCompat и g_diagRunning — вызывается из ExplorerCompat_StartXamlDiagnostics.
    void WinUiDesktopBridge_FlushPendingAfterExplorerCompatReady() noexcept;
}
