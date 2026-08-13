#pragma once
#include "WndPch.h"
#include <GlassControlKind.h>

namespace WndCompat
{
    void DateTimePicker_Attach(HWND hWnd, GlassShared::GlassControlKind kind);
    void DateTimePicker_Detach(HWND hWnd);
    void DateTimePicker_DetachAll();
}
