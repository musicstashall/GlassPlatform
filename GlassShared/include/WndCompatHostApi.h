#pragma once
#include <windows.h>

namespace Shared::WndCompatHost
{
    using MenuFlyoutBorderRefreshFn = void (*)(HWND hwnd) noexcept;

    void RegisterMenuFlyoutBorderRefresh(MenuFlyoutBorderRefreshFn fn) noexcept;
    void InvokeMenuFlyoutBorderRefresh(HWND hwnd) noexcept;
}
