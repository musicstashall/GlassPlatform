#pragma once
#include <string>
#include <string_view>
#include <windows.h>

namespace WndCompat
{
    // Возвращает нормализованное имя класса:
    //  - Win32: "Button"
    //  - WinForms: ".Button"
    //  - WPF: "WPF_HwndWrapper"
    //  - WinUI: "WinUI_Frame"
    std::wstring NormalizeWindowClass(HWND hWnd);

    // Универсальная проверка имени класса:
    //  - IsWindowClass(hWnd, L"Button") → true для Win32 и WinForms
    //  - IsWindowClass(hWnd, L".Button") → true для Win32 и WinForms
    bool IsWindowClass(HWND hWnd, std::wstring_view expected);

    /// Preview-блоки wizard GlassSettings (custom WM_PAINT; без Root_Attach).
    [[nodiscard]] bool IsGlassSettingsBitmapSwitchHwnd(HWND hWnd) noexcept;
}
