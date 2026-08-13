#include "SharedPch.h"

#include "framework.h"
#include "Ui/MainWindow/UiMainWindowImpl.h"
#include "Ui/MainWindow/UiMainWindowSubclasses.h"
#include "Ui/Messages/LogViewerAppMessages.h"
#include "UiConsts.h"
#include <commctrl.h>

#pragma comment(lib, "Comctl32.lib")

static bool IsLogListHwnd(const UiMainWindowImpl* impl, HWND h) noexcept
{
    if (!impl || !h)
        return false;
    for (const auto& t : impl->m_tabs)
        if (t.hList == h)
            return true;
    return false;
}

bool LogViewerSearchNavigateBackwardFromModifiers() noexcept
{
    return (GetKeyState(VK_CONTROL) & 0x8000) != 0
        || (GetKeyState(VK_SHIFT) & 0x8000) != 0;
}

LRESULT CALLBACK LogViewerMainWndEditSubclassProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData)
{
    auto* impl = reinterpret_cast<UiMainWindowImpl*>(dwRefData);
    HWND hwndMain = impl ? impl->hwnd : nullptr;
    if (msg == WM_KILLFOCUS)
    {
        HWND to = reinterpret_cast<HWND>(wParam);
        if (IsLogListHwnd(impl, to))
            return DefSubclassProc(hwnd, msg, wParam, lParam);
        if (hwndMain)
            PostMessageW(hwndMain, WM_APP_HIDE_EDIT, 0, 0);
    }
    else if (msg == WM_KEYDOWN)
    {
        if (wParam == VK_ESCAPE)
        {
            if (hwndMain)
                PostMessageW(hwndMain, WM_APP_HIDE_EDIT, 0, 0);
            return 0;
        }
        if (wParam == VK_RETURN)
        {
            if (hwndMain)
            {
                KillTimer(hwndMain, IDT_SEARCH);
                const bool back = LogViewerSearchNavigateBackwardFromModifiers();
                SendMessageW(hwndMain, WM_COMMAND, MAKEWPARAM(back ? IDC_FIND_PREV : IDC_FIND_NEXT, 0), 0);
            }
            return 0;
        }
        if (wParam == VK_F3)
        {
            if (hwndMain)
            {
                const bool back = LogViewerSearchNavigateBackwardFromModifiers();
                SendMessageW(hwndMain, WM_COMMAND, MAKEWPARAM(back ? IDC_FIND_PREV : IDC_FIND_NEXT, 0), 0);
            }
            return 0;
        }
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK LogViewerMainWndTabCtrlSubclassProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData)
{
    HWND hwndMain = reinterpret_cast<HWND>(dwRefData);
    switch (msg)
    {
    case WM_NOTIFY:
    case WM_CONTEXTMENU:
    case WM_MOUSEWHEEL:
        return SendMessageW(hwndMain, msg, wParam, lParam);
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}
