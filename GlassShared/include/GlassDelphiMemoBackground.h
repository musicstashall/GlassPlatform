#pragma once
#include <windows.h>

// HWND property содержит только COLORREF + 1, не владеет кистью или указателем.
// Её заполняет WndCompat после подтверждённой сплошной CTLCOLOR-кисти;
// GlassCore дополнительно сверяет её с фоном текста в текущем WM_PAINT.
namespace GlassShared
{
    inline constexpr wchar_t kDelphiMemoBackgroundProp[] = L"GlassPlatform.DelphiMemoSolidBackground";

    inline void ClearDelphiMemoBackground(HWND hwnd) noexcept
    {
        if (hwnd) RemovePropW(hwnd, kDelphiMemoBackgroundProp);
    }

    inline bool SetDelphiMemoBackground(HWND hwnd, COLORREF color) noexcept
    {
        if (!hwnd) return false;
        return SetPropW(hwnd, kDelphiMemoBackgroundProp,
            reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>((color & 0xFFFFFFu) + 1u))) != FALSE;
    }

    [[nodiscard]] inline COLORREF GetDelphiMemoBackground(HWND hwnd) noexcept
    {
        if (!hwnd) return CLR_INVALID;
        const ULONG_PTR value = reinterpret_cast<ULONG_PTR>(GetPropW(hwnd, kDelphiMemoBackgroundProp));
        if (!value || value > 0x1000000u) return CLR_INVALID;
        return static_cast<COLORREF>(value - 1u);
    }
}
