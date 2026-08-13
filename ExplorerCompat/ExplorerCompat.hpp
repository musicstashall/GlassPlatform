#pragma once
#include <windows.h>

// Публичный API ExplorerCompat
// Упрощённый интерфейс: инфраструктурный shutdown, не зависящий от UI thread

extern "C"
{
    // ============================================
    // LIFECYCLE MANAGEMENT
    // ============================================

    // Инициализация модуля ExplorerCompat.
    // Вызывается из GUI thread при нормальной работе.
    BOOL WINAPI ExplorerCompat_Initialize();

    // Завершение работы модуля ExplorerCompat.
    // Thread-agnostic, non-blocking, idempotent.
    // Не зависит от UI thread, не гарантирует UI cleanup.
    BOOL WINAPI ExplorerCompat_Shutdown();

    // ============================================
    // EVENT NOTIFICATIONS
    // ============================================

    // WinEvent (GlassCompat → тот же SRWLOCK, что и WndCompat). Диагностика CabinetWClass и т.п.
    void WINAPI ExplorerCompat_HandleWinEvent(
        DWORD event,
        HWND hwnd,
        LONG idObject,
        LONG idChild,
        DWORD dwEventThread,
        DWORD dwmsEventTime);

    // Live смена Root\NoBackdrop в explorer.exe (WndCompat → GetProcAddress).
    void WINAPI ExplorerCompat_OnRootBackdropPolicyChanged(BOOL enabled) noexcept;
}
