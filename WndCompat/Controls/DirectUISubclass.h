#pragma once
#include "WndPch.h"

namespace WndCompat
{
    // Регистрация / снятие регистрации DirectUIHWND-окна.
    // SetWindowSubclass не используется — только учёт в реестре.
    // DirectUI_Detach вызывается из EVENT_OBJECT_DESTROY в WinEventHandler.
    void DirectUI_Attach(HWND hWnd);
    void DirectUI_Detach(HWND hWnd);

    // Перерисовать все зарегистрированные DirectUIHWND-окна.
    // Вызывается из WinEventHook_Shutdown при выгрузке.
    void DirectUI_DetachAll();

    // Live RootBackdropMode: повтор attach-эффекта (кисть + RootHooks) или сброс при Off.
    void DirectUI_ApplyBackdropPolicyToTracked() noexcept;
}
