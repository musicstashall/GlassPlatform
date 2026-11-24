#pragma once
#include <windows.h>

namespace GlassCore::RestoratorImages
{
    // Pictures of exact TResTreeView / TImgViewBox: the startup placeholders
    // and the picture resources the viewer shows - bitmaps the EXE copies into
    // the window's own DC while it paints. Each is drawn as it is, opaque, its
    // own background included; only the colour of the window the EXE composed
    // it on is cut from its outline. No other window, DC, ROP or bitmap is
    // touched. Register before PaintDispatcher::Startup().
    void Startup() noexcept;
    void Shutdown() noexcept;
}
