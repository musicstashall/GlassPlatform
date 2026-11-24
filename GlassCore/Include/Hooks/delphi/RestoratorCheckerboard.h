#pragma once
#include <windows.h>

namespace GlassCore::RestoratorCheckerboard
{
    // A picture with transparency of its own - a PNG resource, the PNG frame of
    // an icon - reaches GDI already flattened: the EXE fills a bitmap of the
    // picture's size with the colour of its window and draws the picture over
    // it. What was transparent is then a patch of that colour, and no alpha is
    // left to tell it from the picture. Like the donor, that one fill is
    // replaced where it is made by a checkerboard; the EXE itself blends the
    // picture over it, so every edge stays as smooth as the picture has it.
    // Nothing is drawn into a window here.
    void Startup() noexcept;
    void Shutdown() noexcept;

    // `viewer`, a window of one of this EXE's picture viewer classes, paints
    // on glass. The first such call of a start asks for the EXE's own imports
    // of CreateDIBSection and FillRect - both or none, held until Shutdown;
    // every call names the top-level window that answers, from then on,
    // whether glass is still on.
    void Arm(HWND viewer) noexcept;
}
