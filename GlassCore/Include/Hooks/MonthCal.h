#pragma once
#include <windows.h>

namespace GlassCore::MonthCal
{
    void Prepare();
    void Startup();
    void Shutdown();

    void OnMonthCalAttach(HWND hWndMonthCal, HWND hWndDropDown);

    // WM_PRINTCLIENT — явное включение/выключение IAT-хуков.
    // PaintDispatcher не перехватывает DC, переданный через wParam напрямую.
    void OnMonthCalPrintClientBegin(HWND hWnd);
    void OnMonthCalPrintClientEnd(HWND hWnd);
}