#pragma once
#include <Windows.h>

// Старт WinUI XAML Diagnostics (InitializeXamlDiagnosticsEx + TAP). Вызывать из ExplorerCompat_Initialize.
// Событийная модель: WinEvent/bootstrap при появлении DesktopChildSiteBridge / DesktopWindowHost.
// UWP-сессия (зона трея) удалена — таск-бар обслуживается отдельной DLL TaskbarCompat.
[[nodiscard]] HRESULT ExplorerCompat_StartXamlDiagnostics();

// Остановка watcher и выгрузка FrameworkUdk, если мы поднимали счётчик LoadLibrary.
void ExplorerCompat_StopXamlDiagnostics() noexcept;

// CoFreeUnusedLibraries на STA потоке IXDE (HWND_MESSAGE reclaim).
void ExplorerCompat_RunIxdeComReclaim() noexcept;

// WinEvent / bootstrap: создан остров WinUI (как HookManager::HookedCreateWindowExW в FileBrowsWindowPatch).
// Экспорт для WndCompat (WinUiDesktopBridgeHost): ранний триггер глобального UDK (InstallUdk → SetSite → Advise).
extern "C" void WINAPI ExplorerCompat_NotifyDesktopBridgeWindow(HWND hwnd) noexcept;
