#pragma once

#include <string>

namespace Shared::ExplorerCompatHostPolicy
{
    // explorer.exe — единственный процесс, где WndCompat маршрутизирует WinUI host HWND в ExplorerCompat.
    [[nodiscard]] bool IsExplorerProcess() noexcept;

    // WinUI host HWND: attach только через ExplorerCompat (не generic WndCompat Root/Control).
    [[nodiscard]] bool IsHostWindowClass(const std::wstring& normalizedCls) noexcept;

    // DesktopChildSiteBridge — единственный host-класс, который WndCompat обрабатывает (→ ExplorerCompat notify).
    [[nodiscard]] bool IsWndCompatBridgeClass(const std::wstring& normalizedCls) noexcept;

    // true только в explorer.exe для DesktopChildSiteBridge; иначе WndCompat Ignored.
    [[nodiscard]] bool ShouldWndCompatHandleHostWindow(const std::wstring& normalizedCls) noexcept;
}
