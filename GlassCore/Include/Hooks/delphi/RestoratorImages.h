#pragma once
#include <windows.h>

namespace GlassCore::RestoratorImages
{
    // Startup placeholders of exact TResTreeView / TImgViewBox: a picture on
    // black that the EXE copies into the window's own DC while it paints.
    // Its black matte is removed: blocks opaque, text by brightness,
    // background transparent. No other window, DC, ROP or bitmap is touched.
    // Register before PaintDispatcher::Startup().
    void Startup() noexcept;
    void Shutdown() noexcept;
}
