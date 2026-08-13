#pragma once
#include <windows.h>

namespace Shared::ColorUtils
{
    inline BYTE GetAlphaFromARGB(DWORD argb)
    {
        return (argb >> 24) & 0xFF;
    }

    inline BYTE GetRedFromARGB(DWORD argb)
    {
        return (argb >> 16) & 0xFF;
    }

    inline BYTE GetGreenFromARGB(DWORD argb)
    {
        return (argb >> 8) & 0xFF;
    }

    inline BYTE GetBlueFromARGB(DWORD argb)
    {
        return argb & 0xFF;
    }

    inline COLORREF ARGBToCOLORREF(DWORD argb)
    {
        return RGB(GetRedFromARGB(argb), GetGreenFromARGB(argb), GetBlueFromARGB(argb));
    }

    inline HBRUSH CreateSolidBrushFromARGB(DWORD argb)
    {
        return CreateSolidBrush(ARGBToCOLORREF(argb));
    }
}