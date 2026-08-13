#include "SharedPch.h"
#include <WndCompatHostApi.h>

namespace Shared::WndCompatHost
{
    namespace
    {
        MenuFlyoutBorderRefreshFn s_menuFlyoutBorderRefresh{};
    }

    void RegisterMenuFlyoutBorderRefresh(MenuFlyoutBorderRefreshFn fn) noexcept
    {
        s_menuFlyoutBorderRefresh = fn;
    }

    void InvokeMenuFlyoutBorderRefresh(HWND hwnd) noexcept
    {
        if (s_menuFlyoutBorderRefresh)
            s_menuFlyoutBorderRefresh(hwnd);
    }
}
