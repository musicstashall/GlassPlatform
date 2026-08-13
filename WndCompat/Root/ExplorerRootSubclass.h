#pragma once
#include <Windows.h>

#include <unordered_map>
#include <vector>

#include "Root/RootExplorerSubclassBridge.h"

namespace WndCompat
{
    // CabinetWClass / ExploreWClass (GA_ROOT): свой набор сообщений — см. ExplorerRootSubclass.cpp (не RootSubclassProcShared).
    LRESULT CALLBACK ExplorerRootSubclassProc(
        HWND hWnd,
        UINT uMsg,
        WPARAM wParam,
        LPARAM lParam,
        UINT_PTR uIdSubclass,
        DWORD_PTR dwRefData);
}

// Раннее подклассирование из IAT CreateWindowExW (GlassCore → WndCompat).
extern "C" __declspec(dllexport)
void WINAPI WndCompat_EarlyExplorerRootAttach(HWND hWnd) noexcept;

namespace WndCompat::ExplorerRoot
{
    inline constexpr UINT_PTR ExplorerRootSubclassId = 2;

    // WM_GLASS_EXPLORER_CHROME_REDRAW: только CabinetWClass / ExploreWClass на GA_ROOT.
    // true — обработано, outLr обычно 0; false — не наше окно, вызывать DefSubclassProc.
    [[nodiscard]] bool TryHandleChromeRedraw(HWND hWnd, LRESULT& outLr) noexcept;

    void PostExplorerChromeRedrawIfShellRoot(HWND hRoot) noexcept;

    /// Live OFF / STOP S0: sync CHROME по всем CabinetWClass/ExploreWClass (stock top-margin).
    void SyncExplorerShellChromeRedrawAll() noexcept;

    /// Полный RedrawWindow на root (паритет RestoreRootNcAfterDetachImpl для Win11 explorer).
    void RedrawExplorerShellRootFull(HWND hRoot) noexcept;

    /// Process-wide: sync full redraw по всем CabinetWClass/ExploreWClass (поток владельца HWND).
    void SyncExplorerShellRootRedrawAll() noexcept;

    // Live ON sweep: паритет ExplorerRoot::Attach (SWCA + CHROME).
    void ApplyShellBackdropOnBackdropEnabled(HWND hRoot) noexcept;

    void Attach(HWND hRoot);

    void Detach(HWND hRoot);

    /// Снимок shutdown из Root_DetachAll: только File Explorer shell root из windows.
    void DetachAll(
        const std::vector<HWND>& windows,
        const std::unordered_map<ATOM, WndCompat::Root::ExplorerBridge::ClassBrushEntry>& classBrushes,
        DWORD curTid);
}
