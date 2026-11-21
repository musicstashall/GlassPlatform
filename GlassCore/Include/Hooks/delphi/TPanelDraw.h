#pragma once
#include <windows.h>
#include <uxtheme.h>

namespace GlassCore::DelphiPanel
{
    void Startup() noexcept;
    void Shutdown() noexcept;

    // Bracket native WM_PAINT / WM_ERASEBKGND / WM_PRINTCLIENT. Only a
    // successful Begin may be paired with End; suppliedDc comes from wParam.
    bool BeginPaint(HWND hwnd, UINT message, HDC suppliedDc) noexcept;
    void EndPaint() noexcept;

    // Shared VCL themed-caption compositor. The root dispatcher owns the EXE
    // IAT slot and passes its actual original (including the IAT chain).
    // Also retains the existing TGroupBox/TTabSheet caption path.
    HRESULT DrawContainerThemeTextEx(decltype(&::DrawThemeTextEx) original,
        HTHEME theme, HDC dc, int part, int state, LPCWSTR text, int count,
        DWORD flags, LPRECT rect, const DTTOPTS* options);
}
