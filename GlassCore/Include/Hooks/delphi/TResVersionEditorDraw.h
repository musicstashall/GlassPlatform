#pragma once
#include <windows.h>

namespace GlassCore::DelphiResVersionEditor
{
    // Exact TResVersionEditor, a custom VCL control an ANSI Delphi EXE paints
    // itself: while the EXE paints that window, the ANSI text it writes into
    // the window's own DC (ExtTextOutA) is drawn with alpha at the caller's
    // position, and what it fills with the default window or button-face
    // colour (FillRect) is cleared. No other window, DC or call is touched.
    // Register before PaintDispatcher::Startup().
    void Startup() noexcept;
    void Shutdown() noexcept;
}
