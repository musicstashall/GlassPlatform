#pragma once
#include <windows.h>
namespace GlassCore::DelphiInput
{
    bool IsSpinEdit(HWND hwnd) noexcept;
    bool IsNumberBox(HWND hwnd) noexcept;
    bool IsHotKey(HWND hwnd) noexcept;
    bool ClearNumberBoxGlyphBackground(HWND hwnd, HDC dc, const RECT* rect, UINT options) noexcept;
    bool ClearHotKeyBackground(HWND hwnd, HDC dc, const RECT* rect, HBRUSH brush) noexcept;
}
