#pragma once
#include <windows.h>

namespace WndCompat::Rebar
{
    struct RebarEventProvider
    {
        // WM_PAINT: включить/выключить DrawThemeBackground / DrawTextExW хуки.
        void (*OnPaintBegin)(HWND hWnd) = nullptr;
        void (*OnPaintEnd)()   = nullptr;
    };

    void SetRebarEventProvider(const RebarEventProvider& provider);
    const RebarEventProvider* GetRebarEventProvider();
}
