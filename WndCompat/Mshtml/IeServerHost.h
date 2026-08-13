#pragma once
#include <windows.h>

namespace WndCompat
{
    // Только при InjectionMode::Full (проверка внутри).
    void IeServer_Attach(HWND hWnd);
    void IeServer_Detach(HWND hWnd);
    void IeServer_DetachAll();
}
