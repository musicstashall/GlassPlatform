#pragma once
#include <Windows.h>
#include <MenuContext.h>
#include <MenuEventProvider.h>

namespace GlassCore::Menu
{
    void OnPopupCreated(HWND hWnd, const WndCompat::Menu::MenuContext& ctx);
    void OnPopupShown(HWND hWnd, const WndCompat::Menu::MenuContext& ctx);
    void OnPopupSized(HWND hWnd, const WndCompat::Menu::MenuContext& ctx);
    void OnPopupDestroyed(HWND hWnd, const WndCompat::Menu::MenuContext& ctx);

    void OnPaintFlyoutHostBorder(
        HWND hWnd,
        WndCompat::Menu::FlyoutBorderPass pass,
        HRGN clipRgn) noexcept;

    /// Per-popup HWND: рамка отложена до uncloak flyout-анимации.
    void SetFlyoutBorderDeferred(HWND hWnd, bool deferred) noexcept;
    [[nodiscard]] bool IsFlyoutBorderDeferred(HWND hWnd) noexcept;
    void ClearFlyoutBorderDeferred(HWND hWnd) noexcept;
    /// Синхронная отрисовка рамки после uncloak (GUI-поток меню).
    void PaintDeferredFlyoutBorderIfNeeded(HWND hWnd) noexcept;

    void EnsureNativeThemeFlyoutBorder(HWND hWnd, bool forceRefresh = false) noexcept;

    void OnOwnerDrawItem(
        HWND hWnd,
        const WndCompat::Menu::MenuContext& ctx,
        const DRAWITEMSTRUCT* dis,
        const WndCompat::Menu::ContextMenuRenderingData* data
    );

    // ── Owner-draw guard ─────────────────────────────────────────────────────
    // Thread_local флаг, живущий внутри GlassCore.dll.
    // WndCompat устанавливает его через OnOwnerDrawBegin/End вокруг
    // DefSubclassProc(WM_DRAWITEM). ImmersiveHooks и TraverseLogHooks
    // читают его вместо WndCompat::g_drawItemStruct, чтобы не пересекать
    // DLL-границу (у thread_local нет единой копии между DLL).
    bool IsInsideOwnerDraw() noexcept;

    // Вызываются строго вокруг DefSubclassProc(WM_DRAWITEM).
    void OnOwnerDrawBegin(
        HWND hWnd,
        const WndCompat::Menu::MenuContext& ctx,
        const DRAWITEMSTRUCT* dis);

    void OnOwnerDrawEnd(
        HWND hWnd,
        const WndCompat::Menu::MenuContext& ctx);
    // ─────────────────────────────────────────────────────────────────────────

    void OnButtonUp(
        HWND hWnd,
        const WndCompat::Menu::MenuContext& ctx,
        const MENUBARINFO& mbi
    );

    void OnAttachHooks(HWND hWnd, const WndCompat::Menu::MenuContext& ctx);
    void OnDetachHooks(HWND hWnd, const WndCompat::Menu::MenuContext& ctx);

    void SyncMenuContextFromMenuHandler(WndCompat::Menu::MenuContext* ctx) noexcept;
    void ApplyUaInitMenuNativeOrClassic(LRESULT defSubclassResult, WndCompat::Menu::MenuContext* ctx) noexcept;
    void FinalizeNativeThemeAfterPrintWindowProbe(WndCompat::Menu::MenuContext* ctx) noexcept;
    void OnUxHooksBegin(HWND hWnd, const WndCompat::Menu::MenuContext& ctx);
    void OnUxHooksEnd(HWND hWnd, const WndCompat::Menu::MenuContext& ctx);

    void OnMenuBarUxThemeDrawBegin(HWND hWnd, const WndCompat::Menu::MenuContext& ctx);
    void OnMenuBarUxThemeDrawEnd(HWND hWnd, const WndCompat::Menu::MenuContext& ctx);

    void OnMenuBarClassicNcActivateBracketBegin(HWND hWnd);
    void OnMenuBarClassicNcActivateBracketEnd(HWND hWnd);
    void OnMenuBarClassicNcPaintBracketBegin(HWND hWnd);
    void OnMenuBarClassicNcPaintBracketEnd(HWND hWnd);
    void OnMenuBarClassicEtwHoldEnsure(HWND hWnd);
    void OnMenuBarClassicEtwHoldRelease(HWND hWnd);

    void OnMenuBarClassicUahInitMenuBracketBegin(HWND hWnd);
    void OnMenuBarClassicUahInitMenuBracketEnd(HWND hWnd);

    void OnClassicMenuPopupPaintExtTextOutBegin(HWND hWnd);
    void OnClassicMenuPopupPaintExtTextOutEnd(HWND hWnd);

    void ShutdownClassicMenuBarNcExtTextOut() noexcept;
}