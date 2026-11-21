#pragma once
#include <windows.h>

namespace GlassCore::DelphiBomeMemo
{
    // Paint-only EXE slots. Keep ANSI measurement/mutation and unsupported DCs native.
    int DrawAnsiText(decltype(&::DrawTextA) original, HDC dc, LPCSTR text,
        int count, LPRECT rect, UINT flags);
    int FillBackground(decltype(&::FillRect) original, HDC dc,
        const RECT* rect, HBRUSH brush);
}
