#pragma once
#include <windows.h>
#include <DropDownContext.h>

namespace GlassCore::DropDown
{
    // ── Lifecycle ────────────────────────────────────────────────────────────
    void Prepare();
    void Startup();
    void Shutdown();

    // ── Provider callbacks (вызываются из WndCompat через GlassCoreProviders) ─
    // Вокруг WM_PAINT на ListviewPopup
    void OnPaintBegin(HWND hWnd);
    void OnPaintEnd(HWND hWnd);

    // Вокруг WM_DRAWITEM (ODT_LISTVIEW) на ListviewPopup
    void OnDrawItemBegin(HWND hWnd, const DRAWITEMSTRUCT* dis);
    void OnDrawItemEnd(HWND hWnd);

    // ── Internal guards (используются хуками внутри GlassCore) ──────────────
    bool IsInsidePaint()     noexcept;
    bool IsInsideDrawItem()  noexcept;
    const DRAWITEMSTRUCT* GetDrawItemStruct() noexcept;

    // ── Hook control ─────────────────────────────────────────────────────────
    void EnableHooks(bool enable);
    void DisableHooks();

    void OnListviewPopupAttach(HWND hWndListview, HWND hWndRoot);
    void OnViewControlAttach(HWND hWnd);

    void OnToolbarPaintBegin(HWND hWnd);
    void OnToolbarPaintEnd(HWND hWnd);
    void OnDropDownShow(HWND hWnd);

    /// Post-uncloak NC-рамка DropDown/ComboLBox/ViewControl (sync, FrameOnly).
    void PaintDeferredFlyoutBorderIfNeeded(HWND hWnd) noexcept;
    void OnFlyoutAnimationFinished(HWND hWnd) noexcept;
    void RefreshFlyoutClientAfterUncloak(HWND root) noexcept;
    bool QueryDropDownContext(WndCompat::DropDownContext* out) noexcept;
    void ClearFlyoutBorderDeferred(HWND hWnd) noexcept;

    // ComboLBox и ComboBox хуки перенесены в GlassCore::ComboLBox
    // (см. Hooks/ComboLBox.h)
}