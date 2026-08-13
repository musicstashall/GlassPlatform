#pragma once
#include "WndPch.h"
#include "MenuContext.h"

namespace WndCompat::Menu::Policy
{
    // Основные события popup-меню
    void OnPopupShow(HWND hWnd, bool isThemedMenu);
    void OnPopupDestroy(HWND hWnd);

    // NT popup
    void OnPopupShow_NT(HWND hWnd);
    void OnPopupDestroy_NT(HWND hWnd);

    // Owner-draw
    void OnOwnerShow(HWND hWnd);
    void OnOwnerDestroy(HWND hWnd);

    bool OnOwnerMeasureItem(HWND hWnd, MEASUREITEMSTRUCT* mis);
    bool OnOwnerDrawItem(HWND hWnd, DRAWITEMSTRUCT* dis);
    bool OnOwnerMeasureItem(HWND, MEASUREITEMSTRUCT*);
    // Определение типа меню (перенос из BWM)
    void DetectMenuType(HWND hWnd);
}
