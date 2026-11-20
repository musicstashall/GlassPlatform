#pragma once
#include <windows.h>
namespace GlassCore::DelphiBackground
{
    void Startup() noexcept;
    void Shutdown() noexcept;
    void ClearObservedListTail(HWND hwnd, const DRAWITEMSTRUCT* dis) noexcept;
}
