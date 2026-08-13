#pragma once
#include "WndPch.h"

namespace WndCompat
{
    // Подключить сабкласс к popup-окну ListView
    void ListviewPopup_Attach(HWND hWnd);

    // Идемпотентно — для EVENT_OBJECT_SHOW, если CREATE был потерян до готовности WndCompat
    void ListviewPopup_AttachIfNeeded(HWND hWnd);

    // Снять сабкласс
    void ListviewPopup_Detach(HWND hWnd);
}
