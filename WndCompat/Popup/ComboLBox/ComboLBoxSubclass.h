#pragma once
#include "WndPch.h"
#include <GlassMessages.h>

namespace WndCompat
{

    // Проверка: ComboLBox уже сабклассирован?
    bool ComboLBox_IsAttached(HWND hWnd);

    // Подключение сабкласса к ComboLBox
    void ComboLBox_Attach(HWND hWnd);

    // Снять субкласс (при уничтожении)
    void ComboLBox_Detach(HWND hWnd);

    // Снять субклассы со всех ComboLBox (Shutdown)
    void ComboLBox_DetachAll();
}
