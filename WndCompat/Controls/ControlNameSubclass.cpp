#include "WndPch.h"
#include "ControlNameSubclass.h"

namespace
{
    LRESULT CALLBACK ControlNameSubclassProc(
        HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
        UINT_PTR id, DWORD_PTR data)
    {
        switch (uMsg)
        {
        case WM_NCDESTROY:
            WndCompat::ControlName_Detach(hWnd);
            break;
        }

        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }
}

namespace WndCompat
{
    void ControlName_Attach(HWND hWnd)
    {
        SetWindowSubclass(hWnd, ControlNameSubclassProc, 1, 0);
    }

    void ControlName_Detach(HWND hWnd)
    {
        RemoveWindowSubclass(hWnd, ControlNameSubclassProc, 1);
    }
}
