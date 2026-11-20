#pragma once
#include <windows.h>

// Optional, passthrough-only owner-draw probe; never changes the supplied DC.
namespace GlassCore::DelphiListBox
{
    bool BeginDrawProbe(HWND parent, const DRAWITEMSTRUCT* dis) noexcept;
    void EndDrawProbe() noexcept;
}
