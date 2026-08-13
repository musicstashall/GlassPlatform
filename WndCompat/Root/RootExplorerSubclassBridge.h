#pragma once
#include <Windows.h>
#include <CommCtrl.h>

#include <unordered_map>
#include <vector>

// Общий учёт g_rootWindows / g_classBrushes + корректное снятие (WM_GLASS_DETACH_ROOT / WM_NCDESTROY).
// Реализации в RootSubclass.cpp; ExplorerRootSubclass.cpp вызывает для Attach/Detach/DetachAll.

namespace WndCompat::Root::ExplorerBridge
{
    struct ClassBrushEntry
    {
        HBRUSH hOrigBrush;
        LONG_PTR origClassStyle;
        int    refCount;
    };

    [[nodiscard]] LRESULT OnDetachRoot(
        HWND hWnd,
        SUBCLASSPROC selfProc,
        UINT_PTR uIdSubclass);

    void OnRootAttachedWndNcDestroy(HWND hWnd);

    /// Регистрация в g_rootWindows под локом; false — уже в списке.
    [[nodiscard]] bool TryRegisterRootWindowForAttach(HWND hWnd);

    void UnregisterRootWindowAfterSubclassFailed(HWND hWnd);

    void ApplyRootClassBlackBrush(HWND hWnd);

    /// Восстановить GCLP_HBRBACKGROUND/GCL_STYLE из g_classBrushes; окно остаётся в g_rootWindows.
    void RestoreRootClassBrushForHwnd(HWND hWnd) noexcept;

    [[nodiscard]] bool ShouldSetComctlScrollHostForRootClassName(const wchar_t* rootClassName) noexcept;

    /// Как в Root_Detach: снять из списка и декремент кисти; false — не было в списке.
    [[nodiscard]] bool RemoveTrackedRootAndDecrementBrush(HWND hWnd);

    void RestoreOriginalClassBrush(
        HWND hWnd,
        const std::unordered_map<ATOM, ClassBrushEntry>& classBrushes);

    void RestoreRootNcAfterDetach(HWND hWnd);

    /// См. `RootEventProvider::IsWin11ExplorerShellCabinetChromeHwnd` (fallback — только ShellTab, если Core ещё не выдал провайдер).
    [[nodiscard]] bool IsWin11ExplorerShellCabinetChromeHwnd(HWND hRoot) noexcept;
}
