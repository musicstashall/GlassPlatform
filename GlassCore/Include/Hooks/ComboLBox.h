#pragma once
#include <windows.h>

namespace GlassCore::ComboLBox
{
    // -- Lifecycle ---------------------------------------------------------------
    void Prepare();
    void Startup();
    void Shutdown();

    // -- Provider callbacks (called from WndCompat via GlassCoreProviders) -------

    // WM_SHOWWINDOW: backdrop effect + animation
    void OnComboLBoxShow(HWND hWnd, bool isComboBoxAnimation);

    // WM_PRINTCLIENT: прямое включение/выключение хуков (PaintDispatcher не перехватывает)
    void OnComboLBoxPrintClientBegin(HWND hWnd);
    void OnComboLBoxPrintClientEnd(HWND hWnd);

    // WM_NCDESTROY: cleanup DWM effects
    void OnComboLBoxDetach(HWND hWnd);

    // CBN_DROPDOWN: arm user32!AnimateWindow Detour (comctl32 не импортирует AnimateWindow в IAT)
    void OnComboBoxDropDown(HWND hComboBox);
}
