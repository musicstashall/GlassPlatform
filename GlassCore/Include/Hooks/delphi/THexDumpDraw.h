#pragma once
#include <windows.h>

namespace GlassCore::DelphiHexDump
{
    // Rows of exact THexDump, the hex viewer control of Borland's HexDump.pas,
    // in whichever application compiled the unit in: the ANSI text the EXE
    // writes into the window's own DC while it paints (ExtTextOutA for the
    // address and character columns, TabbedTextOutA for the bytes). The glyphs
    // are drawn with alpha at the caller's positions; no other window, DC or
    // call is touched. Register before PaintDispatcher::Startup().
    void Startup() noexcept;
    void Shutdown() noexcept;
}
