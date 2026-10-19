#pragma once
#include <windows.h>
#include <GlassControlKind.h>
#include <RootPolicyQuery.h>
#include <RootBackdropPolicy.h>

namespace GlassShared
{
    inline ATOM GetRootBackdropAllowedAtom() noexcept
    {
        static const ATOM s_atom = GlobalAddAtomW(L"GlassPlatform.RootBackdropAllowed");
        return s_atom;
    }

    inline void GlassMarkRootBackdropAllowed(HWND hRoot, bool allowed) noexcept
    {
        if (!hRoot)
            return;
        // 2 = разрешён, 1 = запрещён (отличие от «prop не задан»).
        SetPropW(hRoot, MAKEINTATOM(GetRootBackdropAllowedAtom()),
            reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(allowed ? 2u : 1u)));
    }

    [[nodiscard]] inline bool GlassIsRootBackdropAllowed(HWND hRoot) noexcept
    {
        if (!hRoot)
            return false;
        const HANDLE p = GetPropW(hRoot, MAKEINTATOM(GetRootBackdropAllowedAtom()));
        if (p == reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(2u)))
        {
            // Prop мог остаться от attach до ReapplyOpenRootBackdrops при NoBackdrop=1.
            if (!Shared::RootPolicyQuery::IsRootGlassRenderingEnabled())
                return false;
            return true;
        }
        if (p == reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(1u)))
            return false;
        return Shared::RootPolicyQuery::IsRootGlassRenderingEnabled();
    }

    [[nodiscard]] inline bool IsShellTrayRootHwnd(HWND hRoot) noexcept
    {
        if (!hRoot)
            return false;
        wchar_t cls[256]{};
        if (GetClassNameW(hRoot, cls, static_cast<int>(std::size(cls))) <= 0)
            return false;
        return _wcsicmp(cls, L"Shell_TrayWnd") == 0
            || _wcsicmp(cls, L"Shell_SecondaryTrayWnd") == 0;
    }

    [[nodiscard]] inline bool GlassIsBackdropAllowedForHwnd(HWND hWnd) noexcept
    {
        if (!hWnd)
            return false;

        // DropDown popup списка комбобокса: метка при ComboLBox_Attach, не RootBackdropMode.
        if (GlassGetWindowKind(hWnd) == GlassControlKind::ComboLBox)
            return true;

        // Toolbar внутри ViewControl popup: PaintDispatcher без RootBackdropMode prop.
        if (GlassGetWindowKind(hWnd) == GlassControlKind::Toolbar)
        {
            const HWND parent = GetParent(hWnd);
            if (parent)
            {
                wchar_t cls[256]{};
                if (GetClassNameW(parent, cls, static_cast<int>(std::size(cls))) > 0
                    && _wcsicmp(cls, L"ViewControlClass") == 0)
                {
                    return true;
                }
            }
        }

        if (!IsWindow(hWnd))
            return false;

        const HWND hRoot = GetAncestor(hWnd, GA_ROOT);
        if (!hRoot)
            return false;

        // Attach/reapply уже записал 1 или 2. Список классов и wstring — только для корня без метки.
        const HANDLE mark = GetPropW(hRoot, MAKEINTATOM(GetRootBackdropAllowedAtom()));
        if (mark == reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(2u))
            || mark == reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(1u)))
        {
            return GlassIsRootBackdropAllowed(hRoot);
        }

        if (IsShellTrayRootHwnd(hRoot))
            return true;

        if (Shared::RootBackdropPolicy::IsHwndBackdropExempt(hRoot))
            return false;

        return GlassIsRootBackdropAllowed(hRoot);
    }
}
