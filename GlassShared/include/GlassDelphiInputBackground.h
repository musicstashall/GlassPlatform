#pragma once
#include <windows.h>

// Only a confirmed default solid CTLCOLOR brush authorises clearing TNumberBox.
namespace GlassShared
{
    inline constexpr wchar_t kDelphiInputDefaultBackgroundProp[] = L"GlassPlatform.DelphiInputDefaultBackground";
    inline void SetDelphiInputDefaultBackground(HWND hwnd, bool isDefault) noexcept
    {
        if (!hwnd) return;
        if (!isDefault || !SetPropW(hwnd, kDelphiInputDefaultBackgroundProp, reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(1))))
            RemovePropW(hwnd, kDelphiInputDefaultBackgroundProp);
    }
    [[nodiscard]] inline bool HasDelphiInputDefaultBackground(HWND hwnd) noexcept
    {
        return hwnd && GetPropW(hwnd, kDelphiInputDefaultBackgroundProp) != nullptr;
    }
}
