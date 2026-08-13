#pragma once
#include <windows.h>

namespace WndCompat
{
    inline constexpr UINT kKbxLabelSubclassId = 2;

    void KbxLabel_Attach(HWND hWnd);
    void KbxLabel_Detach(HWND hWnd);
    void KbxLabel_DetachAll();
}
