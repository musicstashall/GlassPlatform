#pragma once
#include <windows.h>
#include <cwchar>

namespace GlassShared
{
    // Только явные VCL-панели и подтверждённые классы sample; никаких
    // подстрок Panel/Frame/Form и никаких выводов из имени темы или подписи.
    [[nodiscard]] inline bool IsDelphiPanelClassName(const wchar_t* name) noexcept
    {
        if (!name) return false;
        return _wcsicmp(name, L"TPanel") == 0
            || _wcsicmp(name, L"TFlowPanel") == 0
            || _wcsicmp(name, L"TGridPanel") == 0
            || _wcsicmp(name, L"TStackPanel") == 0
            || _wcsicmp(name, L"TRelativePanel") == 0
            || _wcsicmp(name, L"TCategoryPanel") == 0
            || _wcsicmp(name, L"TCategoryPanelGroup") == 0
            || _wcsicmp(name, L"TScrollBox") == 0
            || _wcsicmp(name, L"TFrame") == 0
            || _wcsicmp(name, L"TSampleFrame") == 0
            || _wcsicmp(name, L"TEmbeddedForm") == 0;
    }

    [[nodiscard]] inline bool IsDelphiPanelWindow(HWND hwnd) noexcept
    {
        wchar_t name[64]{};
        return hwnd && GetClassNameW(hwnd, name, static_cast<int>(_countof(name))) > 0
            && IsDelphiPanelClassName(name);
    }

    // Text-only exception, not membership of the panel background/themed set.
    // TPageControl/TTabSheet and WM_ERASEBKGND/GetDC are intentionally excluded.
    [[nodiscard]] inline bool IsDelphiPageAnsiPaintWindow(HWND hwnd, UINT message) noexcept
    {
        if (message != WM_PAINT && message != WM_PRINTCLIENT) return false;
        wchar_t name[64]{};
        return hwnd && GetClassNameW(hwnd, name, static_cast<int>(_countof(name))) > 0
            && _wcsicmp(name, L"TPage") == 0;
    }
}
