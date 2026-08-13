#pragma once
#include "WndPch.h"
#include <GlassMessages.h>

namespace WndCompat
{
    void TabControl_Attach(HWND hWnd);
    void TabControl_Detach(HWND hWnd);
    void TabControl_DetachAll();
}
