#pragma once
#include "WndPch.h"

namespace WndCompat
{
    void ListView_Attach(HWND hWnd);
    void ListView_Detach(HWND hWnd);
    void ListView_DetachAll();

    [[nodiscard]] bool ListView_IsTracked(HWND hWnd) noexcept;

    void ListView_ApplyBackdropPolicyToTracked() noexcept;
}
