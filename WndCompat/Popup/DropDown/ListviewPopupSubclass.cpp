#include "WndPch.h"
#include <WndCompat.h>
#include "ListviewPopupSubclass.h"
#include "DropDownSubclass.h"
#include <Tracker/WindowClassUtils.h>
#include <DropDownEventProvider.h>
#include <MenuUtils.h>


namespace
{
    LRESULT CALLBACK ListviewPopupSubclassProc(
        HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
        UINT_PTR /*uIdSubclass*/, DWORD_PTR /*dwRefData*/)
    {
        switch (uMsg)
        {
        case WM_PAINT:
        {
            if (WndCompat::IsWindowClass(hWnd, L"ListviewPopup"))
            {
                auto* p = WndCompat::DropDown::GetDropDownEventProvider();

                if (p && p->OnPaintBegin)
                    p->OnPaintBegin(hWnd);

                LRESULT result = DefSubclassProc(hWnd, uMsg, wParam, lParam);

                if (p && p->OnPaintEnd)
                    p->OnPaintEnd(hWnd);

                return result;
            }
            
            break;
        }

        case WM_ERASEBKGND:
        {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            RECT clipRect{};
            GetClipBox(hdc, &clipRect);
            PatBlt(hdc, clipRect.left, clipRect.top,
                clipRect.right - clipRect.left,
                clipRect.bottom - clipRect.top,
                BLACKNESS);
            return TRUE;
        }

        case WM_DRAWITEM:
        {
            auto* dis = reinterpret_cast<LPDRAWITEMSTRUCT>(lParam);
            if (!dis)
                break;

            if (dis->CtlType == ODT_LISTVIEW)
            {
                auto* p = WndCompat::DropDown::GetDropDownEventProvider();

                if (p && p->OnDrawItemBegin)
                    p->OnDrawItemBegin(hWnd, dis);

                LRESULT result = DefSubclassProc(hWnd, uMsg, wParam, lParam);

                if (p && p->OnDrawItemEnd)
                    p->OnDrawItemEnd(hWnd);

                return result;
            }
            break;
        }

        case WM_NCDESTROY:
            WndCompat::ListviewPopup_Detach(hWnd);
            break;
        }

        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }
}

namespace WndCompat
{
    void ListviewPopup_Attach(HWND hWnd)
    {
        if (!IsWindow(hWnd))
            return;

        // Паритет TF: subclass на том же HWND, что ApplyEffect (shell DropDown > GA_ROOT > listview).
        HWND const target = Shared::MenuUtils::ResolveDropDownEffectHwnd(hWnd);
        if (target)
            DropDown_AttachIfNeeded(target);

        auto* p = WndCompat::DropDown::GetDropDownEventProvider();
        if (p && p->OnListviewPopupAttach)
            p->OnListviewPopupAttach(hWnd, target);

        // ── Сабклассируем сам ListviewPopup ──────────────────────────────────
        if (SetWindowSubclass(hWnd, ListviewPopupSubclassProc, 1, 0))
        {
            WndCompat::RegisterTextGlowPopupClientWithGlassCore(hWnd, 0);
            DebugLog(L"[WndCompat][ListviewPopup] ListviewPopup_Attach: subclassed 0x{:x}",
                reinterpret_cast<std::uintptr_t>(hWnd));
        }
        else
        {
            DebugLog(L"[WndCompat][ListviewPopup] ListviewPopup_Attach: FAILED 0x{:x}",
                reinterpret_cast<std::uintptr_t>(hWnd));
        }
    }

    void ListviewPopup_AttachIfNeeded(HWND hWnd)
    {
        if (!IsWindow(hWnd) || WndCompat::IsShuttingDown())
            return;

        DWORD_PTR ref{};
        if (::GetWindowSubclass(hWnd, ListviewPopupSubclassProc, 1, &ref) != FALSE)
            return;

        ListviewPopup_Attach(hWnd);
    }

    void ListviewPopup_Detach(HWND hWnd)
    {
        WndCompat::UnregisterTextGlowClientWithGlassCore(hWnd);
        if (IsWindow(hWnd))
            RemoveWindowSubclass(hWnd, ListviewPopupSubclassProc, 1);

        DebugLog(L"[WndCompat][ListviewPopup] ListviewPopup_Detach: 0x{:x}",
            reinterpret_cast<std::uintptr_t>(hWnd));
    }
}