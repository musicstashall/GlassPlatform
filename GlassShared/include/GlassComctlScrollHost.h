#pragma once
#include <windows.h>

// Второй маркер на HWND: «кандидат на Ensure scrollbar IAT (comctl32)».
// Ортогонален GlassControlKind — не перезаписывает тип контрола.

namespace GlassShared
{
    inline ATOM GetComctlScrollHostAtom() noexcept
    {
        static const ATOM s_atom = GlobalAddAtomW(L"GlassPlatform.ComctlScrollHost");
        return s_atom;
    }

    inline void GlassMarkComctlScrollHost(HWND hWnd) noexcept
    {
        SetPropW(hWnd, MAKEINTATOM(GetComctlScrollHostAtom()),
            reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(1)));
    }

    inline void GlassClearComctlScrollHost(HWND hWnd) noexcept
    {
        RemovePropW(hWnd, MAKEINTATOM(GetComctlScrollHostAtom()));
    }

    [[nodiscard]] inline bool GlassIsComctlScrollHost(HWND hWnd) noexcept
    {
        return GetPropW(hWnd, MAKEINTATOM(GetComctlScrollHostAtom())) != nullptr;
    }
}
