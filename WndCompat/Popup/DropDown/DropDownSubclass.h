#pragma once
#include "WndPch.h"

namespace WndCompat
{
    // Подключить сабкласс к popup-окну DropDown
    void DropDown_Attach(HWND hWnd);

    // Идемпотентный attach (WinEvent CREATE/SHOW до ListviewPopup)
    void DropDown_AttachIfNeeded(HWND hWnd);

    // Снять сабкласс
    void DropDown_Detach(HWND hWnd);
}
