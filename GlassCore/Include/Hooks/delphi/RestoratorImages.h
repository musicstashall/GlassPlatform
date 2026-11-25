#pragma once
#include <windows.h>

namespace GlassCore::RestoratorImages
{
    // Pictures of exact TResTreeView / TImgViewBox: the startup placeholders
    // and the picture resources the viewer shows - bitmaps the EXE copies into
    // the window's own DC while it paints. Each is drawn as it is, opaque, with
    // everything the EXE composed it on; nothing is masked or cut out of it. No
    // other window, DC, ROP or bitmap is touched. Every paint of such a window
    // on glass also arms RestoratorCheckerboard. Register before
    // PaintDispatcher::Startup().
    void Startup() noexcept;
    void Shutdown() noexcept;
}
