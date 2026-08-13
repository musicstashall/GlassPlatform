#pragma once
#include <windows.h>

namespace GlassCore::ExplorerFrameControl
{
    enum class ExplorerFrameRestoreScope
    {
        LiveOff,
        Shutdown,
    };

    void Prepare();
    void Startup();
    void Shutdown();

    /// Единая точка restore explorerframe IAT/chrome (live OFF vs shutdown).
    void RestoreExplorerFrameChrome(ExplorerFrameRestoreScope scope) noexcept;

    /// STOP S0: passthrough ExtendFrame/DwmSetWindowAttribute до ExplorerCompat_Shutdown (хуки ещё в IAT).
    void BeginExplorerShellChromeRestore() noexcept;

    void ClearExplorerIATExtendMarker(HWND hRoot) noexcept;

    void ClearExplorerStockTopMarginMarker(HWND hRoot) noexcept;

    /// Запомненный cyTopHeight из native DwmExtendFrameIntoClientArea explorerframe (0 если нет).
    [[nodiscard]] int GetExplorerStockTopMargin(HWND hWnd) noexcept;

    void OnExplorerRootAttach(HWND hRoot) noexcept;
    void OnExplorerRootDetach(HWND hRoot) noexcept;

    void OnExplorerModuleLoaded(HMODULE hModule, const wchar_t* baseName) noexcept;
    void OnExplorerModuleUnloaded(HMODULE hModule, const wchar_t* baseName) noexcept;

    /// Перечитать политику Root из реестра и включить/выключить IAT на explorerframe
    /// (Root SWCA + Win11 — как FileBrows: без ветвления по SwcaBackDropTypeRoot в теле хука).
    void SyncExplorerFrameBackdropHookFromRegistry() noexcept;

    /// true если для HWND зашли в IAT DwmExtendFrameIntoClientArea из explorerframe.dll
    /// (маркер ставится до pre-orig SWCA; у «Панель управления» explorerframe сюда не заходит).
    /// False → вызывающий код должен принудительно применить DwmExtendFrameIntoClientArea там, где нужно.
    [[nodiscard]] bool DidExplorerIATExtendApply(HWND hWnd) noexcept;

    /// Win11 + Cabinet/Explore: есть ShellTabWindowClass в дереве и/или уже был IAT-extend из explorerframe.
    [[nodiscard]] bool IsWin11ExplorerShellCabinetChromeHwnd(HWND hRoot) noexcept;
}

