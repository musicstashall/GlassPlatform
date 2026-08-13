#pragma once
#include <windows.h>
#include <cstdint>
#include <MenuContext.h>

namespace WndCompat::Menu
{
    /// NativeTheme flyout: SetupAndPaint (DWM + NC один раз / post-uncloak) vs NcPaintOnly (WM_NCPAINT).
    enum class FlyoutBorderPass : uint8_t
    {
        SetupAndPaint = 0,
        NcPaintOnly = 1,
        /// Post-uncloak (WM_GLASS): повторный DWM border + ring NC без затирания client.
        PostUncloakSetupAndPaint = 2,
    };

    struct MenuEventProvider
    {
        // Popup lifecycle
        void (*OnPopupCreated)(HWND hWnd, const MenuContext& ctx) = nullptr;
        void (*OnPopupShown)(HWND hWnd, const MenuContext& ctx) = nullptr;
        void (*OnPopupSized)(HWND hWnd, const MenuContext& ctx) = nullptr;
        void (*OnPopupDestroyed)(HWND hWnd, const MenuContext& ctx) = nullptr;

        /// GUI-поток меню: рамка flyout (NativeTheme DTB / FlyoutFrame). clipRgn — из WM_NCPAINT wParam.
        void (*OnPaintFlyoutHostBorder)(
            HWND hWnd,
            FlyoutBorderPass pass,
            HRGN clipRgn) noexcept = nullptr;

        bool (*IsFlyoutBorderDeferred)(HWND hWnd) noexcept = nullptr;

        // Owner draw / input
        void (*OnOwnerDrawItem)(
            HWND hWnd,
            const MenuContext& ctx,
            const DRAWITEMSTRUCT* dis,
            const ContextMenuRenderingData* data) = nullptr;

        // ── Immersive draw guard ──────────────────────────────────────────────
        // WndCompat вызывает Begin строго перед DefSubclassProc(WM_DRAWITEM),
        // End — сразу после. GlassCore в их реализации устанавливает/сбрасывает
        // собственную thread_local-метку, которую ImmersiveHooks проверяет
        // вместо WndCompat::Menu::g_drawItemStruct (не виден через DLL-границу).
        void (*OnOwnerDrawBegin)(
            HWND hWnd,
            const MenuContext& ctx,
            const DRAWITEMSTRUCT* dis) = nullptr;

        void (*OnOwnerDrawEnd)(
            HWND hWnd,
            const MenuContext& ctx) = nullptr;
        // ─────────────────────────────────────────────────────────────────────

        void (*OnButtonUp)(
            HWND hWnd,
            const MenuContext& ctx,
            const MENUBARINFO& mbi) = nullptr;

        // Hook control
        void (*OnAttachHooks)(HWND hWnd, const MenuContext& ctx) = nullptr;
        void (*OnDetachHooks)(HWND hWnd, const MenuContext& ctx) = nullptr;

        /// Копирует useDarkMode/useUxTheme из GlassCore::MenuHandler в ctx (перед WM_UAHINITMENU / после PrintWindow).
        void (*SyncMenuContextFromMenuHandler)(MenuContext* ctx) = nullptr;
        /// Правило BWM после DefSubclassProc(WM_UAHINITMENU): NativeTheme vs Classic и синхронизация с MenuHandler.
        void (*ApplyUaInitMenuNativeOrClassic)(LRESULT defSubclassResult, MenuContext* ctx) = nullptr;
        /// После PrintWindow: если ctx=Classic, но MenuHandler уже видит useUxTheme (DTB после раннего INIT), промотировать в NativeTheme.
        void (*FinalizeNativeThemeAfterPrintWindowProbe)(MenuContext* ctx) = nullptr;
        void (*OnUxHooksBegin)(HWND hWnd, const MenuContext& ctx) = nullptr;
        void (*OnUxHooksEnd)(HWND hWnd, const MenuContext& ctx) = nullptr;

        // Root WM_UAHDRAWMENU*: сессия отрисовки меню-бара (GlassCore::UxThemeHooks::Begin/EndMenuBarUxThemeDraw).
        void (*OnMenuBarUxThemeDrawBegin)(HWND hWnd, const MenuContext& ctx) = nullptr;
        void (*OnMenuBarUxThemeDrawEnd)(HWND hWnd, const MenuContext& ctx) = nullptr;

        // BWM WinHandler: classic menu bar — ExtTextOut вокруг NC, без опоры только на UAH.
        void (*OnMenuBarClassicNcActivateBracketBegin)(HWND hWnd) = nullptr;
        void (*OnMenuBarClassicNcActivateBracketEnd)(HWND hWnd) = nullptr;
        void (*OnMenuBarClassicNcPaintBracketBegin)(HWND hWnd) = nullptr;
        void (*OnMenuBarClassicNcPaintBracketEnd)(HWND hWnd) = nullptr;
        void (*OnMenuBarClassicEtwHoldEnsure)(HWND hWnd) = nullptr;
        void (*OnMenuBarClassicEtwHoldRelease)(HWND hWnd) = nullptr;

        /// Classic bar без visual styles: WM_UAHINITMENU — HWND+ETW на весь DefSubclass
        /// (при деактивации ExtTextOut идёт с DC памяти; WindowFromDC не даёт владельца).
        void (*OnMenuBarClassicUahInitMenuBracketBegin)(HWND hWnd) = nullptr;
        void (*OnMenuBarClassicUahInitMenuBracketEnd)(HWND hWnd) = nullptr;

        /// Classic #32768: ExtTextOut вокруг WM_PAINT/WM_PRINT (BWM MenuNT WM_PAINT).
        /// Не смешивать с refcount/hwnd меню-бара — только на время отрисовки попапа.
        void (*OnClassicMenuPopupPaintExtTextOutBegin)(HWND hWnd) = nullptr;
        void (*OnClassicMenuPopupPaintExtTextOutEnd)(HWND hWnd) = nullptr;

        /// Root NoBackdrop: сброс g_classicMenuBarHoldHwnds и ExtTextOut (process-wide).
        void (*OnMenuBarBackdropPolicyDisabled)() = nullptr;
    };

    void SetMenuEventProvider(const MenuEventProvider& provider);
    const MenuEventProvider* GetMenuEventProvider();
}