#pragma once
#include <Windows.h>

extern "C" {

// Подготовка TAP-пути; после интереса трея (TrackBar/DesktopWindowContentBridge) — IXDE через ModuleHandler+поток.
[[nodiscard]] HRESULT TaskbarCompat_PrepareXamlDiagnostics() noexcept;

// Дополнительный триггер: msctls_trackbar32 под Shell_*TrayWnd (полный режим в WinEvent).
// FALSE: отклонено (TAP не готов / shutdown) — WndCompat не помечает мост «доставленным».
__declspec(dllexport) BOOL WINAPI TaskbarCompat_NotifyTrayTrackBarHwnd(HWND hwnd) noexcept;

__declspec(dllexport) BOOL WINAPI TaskbarCompat_NotifyContentBridgeWindow(HWND hwnd) noexcept;

// Пара ContentBridge_Destroy / TrackBar_Detach к BumpSurfaceInterest().
__declspec(dllexport) void WINAPI TaskbarCompat_ReleaseTrayIxdeSurfaceInterest() noexcept;

// WndCompat: после финального FAILED бёрста сбросить счётчик перед повторными Attach-бампами.
__declspec(dllexport) void WINAPI TaskbarCompat_ResetIxdeSchedulingAfterFailure() noexcept;

void TaskbarCompat_StopXamlDiagnostics() noexcept;

}
