#pragma once
#include <windows.h>

namespace GlassCore::DelphiPanel
{
    void Startup() noexcept;
    void Shutdown() noexcept;

    // Bracket native WM_PAINT / WM_ERASEBKGND / WM_PRINTCLIENT. Only a
    // successful Begin may be paired with End; suppliedDc comes from wParam.
    bool BeginPaint(HWND hwnd, UINT message, HDC suppliedDc) noexcept;
    void EndPaint() noexcept;
}
