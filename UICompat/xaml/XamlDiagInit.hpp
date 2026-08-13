#pragma once
#include <Windows.h>

extern "C" {

// Подготовка TAP-пути; IXDE после интереса CoreWindow + ModuleHandler Windows.UI.Xaml.
[[nodiscard]] HRESULT UICompat_PrepareXamlDiagnostics() noexcept;

// CoreWindow NC: SEH/ShellHost → UI-thread IXDE (как ShellSurface); иначе legacy STA.
__declspec(dllexport) BOOL WINAPI UICompat_NotifyCoreWindow(HWND hwnd) noexcept;

// Пуск / Поиск / NC — IXDE на UI-потоке окна (Windhawk Start Menu Styler / NC Styler).
__declspec(dllexport) BOOL WINAPI UICompat_NotifyShellSurfaceWindow(HWND hwnd) noexcept;

// Пара CoreWindow_Detach к BumpSurfaceInterest().
__declspec(dllexport) void WINAPI UICompat_ReleaseIxdeSurfaceInterest() noexcept;

__declspec(dllexport) void WINAPI UICompat_ResetIxdeSchedulingAfterFailure() noexcept;

void UICompat_StopXamlDiagnostics() noexcept;

}

namespace UICompat
{
    // Tid UI-потока окна, где поднят IXDE (Start/Search/NC). 0 = не UI-path / не готов.
    // VTC фильтрует чужие потоки по этому значению.
    [[nodiscard]] DWORD GetShellSurfaceThreadId() noexcept;

    // SEH: несколько CoreWindow → IXDE CAS на tid первого HWND, а VTC Add NC-панелей
    // приходит на tid окна с деревом. Adopt потока элемента (только ShellNotification + IXDE OK).
    [[nodiscard]] bool TryAdoptShellSurfaceThreadIdForNcVtc(DWORD tid) noexcept;

    // Маршалинг proc на UI-поток hwnd (WH_CALLWNDPROC + SendMessageTimeout).
    // Используется IXDE init и NC restore на Stop.
    [[nodiscard]] bool RunFromWindowThread(
        HWND hwnd,
        void (*fn)(void*) noexcept,
        void* param) noexcept;
}
