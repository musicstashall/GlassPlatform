#pragma once

#include "WndPch.h"

namespace UiDispatcher
{
    // Инициализация и завершение работы диспетчера (в GUI‑потоке)
    void Initialize();
    void Shutdown();
    /// Host завершает процесс — сброс учёта без Root_Detach.
    void ShutdownHostExit() noexcept;

    // Обработка сообщений (вызывается из WH_GETMESSAGE)
    bool ProcessWindowsMessage(UINT msg, WPARAM wParam, LPARAM lParam);
}
