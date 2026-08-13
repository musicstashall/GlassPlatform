#pragma once
#include "WndPch.h"
#include <GlassMessages.h>
#include <GlassControlKind.h>

namespace WndCompat
{

    // Сабкласс ComboBox (внутри FloatNotifySink).
    // Перехватывает WM_LBUTTONDOWN → включает AnimateWindow hook
    // в comctl32, чтобы при открытии ComboLBox подменить
    // системную анимацию на ShowWindow(SW_SHOW).
    void ComboBox_Attach(HWND hWnd);
    void ComboBox_Attach(HWND hWnd, GlassShared::GlassControlKind kind);
    void ComboBox_Detach(HWND hWnd);

    // Снять сабклассы со всех ComboBox (Shutdown)
    void ComboBox_DetachAll();
}
