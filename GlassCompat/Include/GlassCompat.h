#pragma once
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

    // Явная инициализация (для ручного тестирования / обратная совместимость)
    __declspec(dllexport) BOOL WINAPI GlassCompat_Initialize(HMODULE hModule);

    // CreateRemoteThread entry после LoadLibrary в AppContainer SEH/SMEH (LPVOID игнор).
    // Возврат: 1 = GUI/init запущен, 0 = нет.
    __declspec(dllexport) DWORD WINAPI GlassCompat_RemoteBootstrap(LPVOID);

    // Host OOC → remote: LPVOID = HWND CoreWindow (SHOW). Синтетический attach UICompat.
    // Возврат: 1 = HandleWinEvent вызван, 0/2/3 = ошибка.
    __declspec(dllexport) DWORD WINAPI GlassCompat_RemoteCoreWindowShow(LPVOID hwndAsPtr);

    // Host → remote: догрузить UICompat если Toolhelp/карта потеряли bit (LPVOID игнор).
    __declspec(dllexport) DWORD WINAPI GlassCompat_RemoteEnsureUICompat(LPVOID);

    // Запрос корректного shutdown
    __declspec(dllexport) BOOL WINAPI GlassCompat_RequestShutdown(HMODULE hSelf);

    // Перечитать AttachList из реестра и обновить режим текущего процесса.
    __declspec(dllexport) BOOL WINAPI GlassCompat_ReloadAttachList() noexcept;

    // Live Root NoBackdrop: forwarder в ExplorerCompat через g_hExplorer (не GetModuleHandle из WndCompat).
    __declspec(dllexport) void WINAPI GlassCompat_NotifyExplorerRootBackdropPolicyChanged(BOOL enabled) noexcept;

    // Запрос загрузки TaskbarCompat после Tray-аттача (Win11; идемпотентно; Load на GlassGuiThread).
    __declspec(dllexport) void WINAPI GlassCompat_EnsureTaskbarCompatLoaded() noexcept;

    // Запрос загрузки ExplorerCompat после FileExplorerShellRoot (Win11; идемпотентно).
    __declspec(dllexport) void WINAPI GlassCompat_EnsureExplorerCompatLoaded() noexcept;

    // Запрос загрузки UICompat после CoreWindow-attach (Win11; SEH/ShellHost/SMEH).
    __declspec(dllexport) void WINAPI GlassCompat_EnsureUICompatLoaded() noexcept;

    // Глобальный WinEvent-колбэк (WINEVENT_INCONTEXT, все процессы/потоки).
    // Устанавливается через SetWinEventHook из Main /start.
    __declspec(dllexport) void CALLBACK GlassCompat_WinEventProc(
        HWINEVENTHOOK, DWORD event, HWND hWnd,
        LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime);

    // Экспорт для Rundll32:  rundll32 GlassCompat.dll,Main /start|/stop
    __declspec(dllexport) void CALLBACK Main(
        HWND hwnd, HINSTANCE hinst, LPSTR lpszCmdLine, int nCmdShow);

#ifdef __cplusplus
}
#endif

namespace GlassCompat
{
    bool Initialize(HMODULE hSelf);
    bool RequestShutdown();
}
