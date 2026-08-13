#pragma once
#include <windows.h>
#include <functional>
#include <MenuContext.h>

namespace Shared::MenuUtils
{
    bool IsOwnerDrawMenu(HWND hWnd);
    bool IsImmersiveMenu(HWND hWnd);
    bool IsImmersiveDarkMode(HWND hWnd); // просто вызывает IsSystemDarkMode
    bool IsTraverseLogMenu(HWND hWnd);
    HWND GetCurrentMenuOwner();
    bool IsSystemDarkMode();
    bool IsPopupMenu(HWND hWnd);
    /// Flyout DropDown: shell «DropDown» в цепочке родителей, иначе GA_ROOT, иначе hWnd.
    [[nodiscard]] HWND ResolveDropDownEffectHwnd(HWND hWnd) noexcept;
    void WalkMenuItems(HMENU hMenu, const std::function<bool(bool ownerDraw, ULONG_PTR itemData, UINT itemID)>& callback);
    void* GetContextMenuDataForItem(HWND hWnd, ULONG_PTR itemData, UINT itemID); // возвращаем void*, т.к. структура определена в WndCompat
    // Рисует рамку NC области popup меню.
    // Возвращает true если рамка нарисована кастомно (оригинал вызывать не нужно).
    bool HandlePopupMenuNonClientBorderColors(
        HDC hdc,
        const RECT& paintRect,
        const WndCompat::Menu::BorderContext& border);
    /// GDI-контур flyout: explicit ARGB, ImmersiveStart DTB (default), Win10 XOR (DONOTROUND).
    [[nodiscard]] bool PaintFlyoutDefaultBorderOutline(
        HDC hdc,
        HWND hwnd,
        const RECT& paintRect,
        const WndCompat::Menu::BorderContext& border,
        bool darkMode) noexcept;

    [[nodiscard]] bool PaintFlyoutImmersiveStartBorderGdi(
        HDC hdc,
        const RECT& paintRect,
        bool darkMode) noexcept;

    /// BWM: XOR 1px NC ring + DefSubclassProc(WM_NCPAINT).
    [[nodiscard]] bool PaintFlyoutSystemNcBorderXor(
        HWND hWnd,
        HRGN clipRgn = nullptr) noexcept;

    /// GDI-контур NC для flyout (делегирует PaintFlyoutDefaultBorderOutline).
    [[nodiscard]] bool PaintFlyoutNcOutlineGdi(
        HDC hdc,
        const RECT& paintRect,
        const WndCompat::Menu::BorderContext& border,
        bool darkMode) noexcept;

    /// Client-edge рамка для DWM thumbnail (SOURCECLIENTAREAONLY) во время defer.
    [[nodiscard]] bool PaintFlyoutClientEdgeBorderForThumbnail(
        HDC hdc,
        HWND hwnd,
        const WndCompat::Menu::BorderContext& border,
        bool darkMode) noexcept;

    /// DWMWA_BORDER_COLOR для NativeTheme flyout (паритет MenuPopupIn proxy).
    void ApplyMenuDwmBorderColorFromTheme(
        HWND hwnd,
        bool darkMode,
        const WndCompat::Menu::BorderContext& border) noexcept;

    /// NC-рамка NativeTheme: PatBlt + MENU_POPUPBORDERS DTB (паритет MyPopupDrawNonClientArea).
    [[nodiscard]] bool PaintNativeThemePopupNcBorder(
        HWND hwnd,
        const WndCompat::Menu::BorderContext& border,
        HRGN clipRgn = nullptr) noexcept;

    MARGINS GetPopupMenuNonClientMargins(HWND hWnd);
}