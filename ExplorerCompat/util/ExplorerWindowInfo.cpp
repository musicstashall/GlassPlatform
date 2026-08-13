#include "ExplorerPch.h"
#include "ExplorerWindowInfo.hpp"

ExplorerWindowInfo::ExplorerWindowInfo(HWND hwnd)
    : Hwnd(hwnd)
{
    Refresh();
}

bool ExplorerWindowInfo::IsExplorerWindow() const
{
    if (!Hwnd)
        return false;

    wchar_t className[256] = {};
    GetClassNameW(Hwnd, className, 256);

    // Классическое окно Explorer
    if (wcscmp(className, L"CabinetWClass") == 0)
        return true;

    // Новые окна (Win11)
    if (wcscmp(className, L"ExplorerFrame") == 0)
        return true;

    return false;
}

void ExplorerWindowInfo::Refresh()
{
    if (!Hwnd)
        return;

    GetClientRect(Hwnd, &ClientRect);

    DetectXamlIsland();
    DetectWin11Features();
}

bool ExplorerWindowInfo::IsActive() const
{
    return GetForegroundWindow() == Hwnd;
}

bool ExplorerWindowInfo::IsMaximized() const
{
    WINDOWPLACEMENT wp = { sizeof(WINDOWPLACEMENT) };
    if (GetWindowPlacement(Hwnd, &wp))
        return wp.showCmd == SW_SHOWMAXIMIZED;

    return false;
}

void ExplorerWindowInfo::DetectXamlIsland()
{
    XamlIsland = nullptr;

    EnumChildWindows(Hwnd, [](HWND child, LPARAM lParam) -> BOOL
        {
            wchar_t cls[256] = {};
            GetClassNameW(child, cls, 256);

            // XAML Island (WinUI)
            if (wcscmp(cls, L"Windows.UI.Composition.DesktopWindowContentBridge") == 0 ||
                wcscmp(cls, L"DesktopWindowXamlSource") == 0)
            {
                *reinterpret_cast<HWND*>(lParam) = child;
                return FALSE;
            }

            return TRUE;
        }, reinterpret_cast<LPARAM>(&XamlIsland));
}

void ExplorerWindowInfo::DetectWin11Features()
{
    // Простейшая эвристика: наличие XAML Island → Win11
    IsWin11 = (XamlIsland != nullptr);

    // Определение вкладок (22621+)
    HasTabs = false;

    EnumChildWindows(Hwnd, [](HWND child, LPARAM lParam) -> BOOL
        {
            wchar_t cls[256] = {};
            GetClassNameW(child, cls, 256);

            // ShellTabWindowClass — окно вкладок
            if (wcscmp(cls, L"ShellTabWindowClass") == 0)
            {
                *reinterpret_cast<bool*>(lParam) = true;
                return FALSE;
            }

            return TRUE;
        }, reinterpret_cast<LPARAM>(&HasTabs));
}
