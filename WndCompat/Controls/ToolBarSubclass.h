#pragma once
#include "WndPch.h"

namespace WndCompat
{
    void ToolBar_Attach(HWND hWnd);
    void ToolBar_Detach(HWND hWnd);
    void ToolBar_DetachAll();

    void ToolBar_ApplyBackdropPolicyToTracked() noexcept;
}
