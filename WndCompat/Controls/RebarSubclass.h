#pragma once
#include "WndPch.h"

namespace WndCompat
{
    void Rebar_Attach(HWND hWnd);
    // Догоняющий проход по поддереву root-окна: native rebar, созданные до
    // инжекции или до Windows-события CREATE, иначе остаются без сабкласса.
    void Rebar_CatchUp(HWND hRoot) noexcept;
    void Rebar_Detach(HWND hWnd);
    void Rebar_DetachAll();

    void Rebar_ApplyBackdropPolicyToTracked() noexcept;
}
