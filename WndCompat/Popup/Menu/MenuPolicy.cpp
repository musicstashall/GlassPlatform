#include "WndPch.h"
#include "MenuPolicy.h"
#include "MenuContext.h"
//#include "MenuRenderer.h"

namespace WndCompat::Menu::Policy
{
    void DetectMenuType(HWND hWnd)
    {
        // Тип уже определён в MenuSubclassProc при MN_SIZEWINDOW
        // Ничего не делаем
    }

    void OnPopupShow(HWND hWnd, bool isThemedMenu)
    {
        DebugLog(L"[WndCompat][Menu] MenuPolicy::OnPopupShow: hwnd=0x{:x}, type={}",
            reinterpret_cast<std::uintptr_t>(hWnd),
            static_cast<int>(g_MenuContext.type));
    }

    void OnPopupDestroy(HWND hWnd)
    {
        DebugLog(L"[WndCompat][Menu] MenuPolicy::OnPopupDestroy: 0x{:x}",
            reinterpret_cast<std::uintptr_t>(hWnd));
        ResetContext();
    }

    void OnPopupShow_NT(HWND hWnd)
    {
        ResetContext();
        g_MenuContext.type = MenuType::Classic;
        DebugLog(L"[WndCompat][Menu] MenuPolicy::OnPopupShow_NT: 0x{:x}",
            reinterpret_cast<std::uintptr_t>(hWnd));
    }

    void OnPopupDestroy_NT(HWND hWnd)
    {
        DebugLog(L"[WndCompat][Menu] MenuPolicy::OnPopupDestroy_NT: 0x{:x}",
            reinterpret_cast<std::uintptr_t>(hWnd));
        ResetContext();
    }

    void OnOwnerShow(HWND hWnd)
    {
        DebugLog(L"[WndCompat][Menu] MenuPolicy::OnOwnerShow: hwnd=0x{:x}, type={}",
            reinterpret_cast<std::uintptr_t>(hWnd),
            static_cast<int>(g_MenuContext.type));
    }

    void OnOwnerDestroy(HWND hWnd)
    {
        DebugLog(L"[WndCompat][Menu] MenuPolicy::OnOwnerDestroy: 0x{:x}",
            reinterpret_cast<std::uintptr_t>(hWnd));
        ResetContext();
    }

    bool OnOwnerMeasureItem(HWND, MEASUREITEMSTRUCT*)
    {
        return false;
    }

    bool OnOwnerDrawItem(HWND, DRAWITEMSTRUCT* dis)
    {
        if (!dis) return false;
        if (dis->CtlType != ODT_MENU) return false;
        return false;
        // Позволяем Renderer'у обработать отрисовку
        //return Renderer::DrawOwnerItem(*dis);
    }
}