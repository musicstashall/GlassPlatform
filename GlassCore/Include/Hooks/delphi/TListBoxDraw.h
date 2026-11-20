#pragma once
#include <windows.h>
namespace GlassCore::DelphiListBox
{
    void Startup() noexcept;
    void Shutdown() noexcept;
    bool IsInsideDraw() noexcept;
    bool BeginDraw(HWND parent, const DRAWITEMSTRUCT* dis) noexcept;
    void EndDraw() noexcept;
}
