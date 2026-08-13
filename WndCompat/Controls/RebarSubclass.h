#pragma once
#include "WndPch.h"

namespace WndCompat
{
    void Rebar_Attach(HWND hWnd);
    void Rebar_Detach(HWND hWnd);
    void Rebar_DetachAll();

    void Rebar_ApplyBackdropPolicyToTracked() noexcept;
}
