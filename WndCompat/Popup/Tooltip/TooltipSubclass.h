#pragma once
#include <windows.h>

namespace WndCompat
{
    inline constexpr UINT kTooltipSubclassId = 1;

    // TOOLTIPS_CLASSW: как в BWM — попытка аттача на любом WinEvent (кроме destroy).
    void Tooltip_AttachIfNeeded(HWND hWnd);

    // THintWindow: без проверки темы/баллуна.
    void Tooltip_AttachHintWindow(HWND hWnd);

    void Tooltip_Detach(HWND hWnd);
    void Tooltip_DetachAll();

    void Tooltip_BroadcastThemeChanged(HWND exceptHwnd);
}
