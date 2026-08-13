#pragma once
#include "WndPch.h"

namespace WndCompat
{
    void StatusBar_Attach(HWND hWnd);
    void StatusBar_Detach(HWND hWnd);
    void StatusBar_DetachAll();
}
