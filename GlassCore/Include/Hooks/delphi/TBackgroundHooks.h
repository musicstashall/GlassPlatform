#pragma once
#include <windows.h>
namespace GlassCore::DelphiBackground
{
    void Startup() noexcept;
    void Shutdown() noexcept;
    void PrepareObservedListBackground(HWND hwnd, const DRAWITEMSTRUCT* dis) noexcept;
}
