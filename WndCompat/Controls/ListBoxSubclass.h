#pragma once
#include "WndPch.h"
#include <GlassControlKind.h>

namespace WndCompat
{
    void ListBox_Attach(HWND hWnd, GlassShared::GlassControlKind kind);
    void ListBox_Detach(HWND hWnd);
    void ListBox_DetachAll();
}
