#pragma once
#include "WndPch.h"

namespace WndCompat
{
    // Подключить сабкласс к ViewControl popup
    void ViewControl_Attach(HWND hWnd);

    // Идемпотентно — для EVENT_OBJECT_SHOW
    void ViewControl_AttachIfNeeded(HWND hWnd);

    // Снять сабкласс
    void ViewControl_Detach(HWND hWnd);
}
