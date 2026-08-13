#pragma once
#include "WndPch.h"
#include <GlassMessages.h>

namespace WndCompat
{
    void TreeView_Attach(HWND hWnd);
    void TreeView_Detach(HWND hWnd);
    void TreeView_DetachAll();

    [[nodiscard]] bool TreeView_IsTracked(HWND hWnd) noexcept;

    void TreeView_ApplyBackdropPolicyToTracked() noexcept;
}
