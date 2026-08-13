#include "SharedPch.h"
#include <MenuUtils.h>
#include <Helpers/ThemeHelper.h>
#include <Helpers/SystemHelper.h>
#include <dwmapi.h>
#include <ColorUtils.h>
#include <Helpers/WinApiUtils.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <commctrl.h>
#pragma comment(lib, "uxtheme.lib")

namespace Shared::MenuUtils
{
    bool IsPopupMenu(HWND hWnd)
    {
        return (GetClassLongPtr(hWnd, GCW_ATOM) == 32768);
    }

    namespace
    {
        [[nodiscard]] bool IsDropDownShellClass(HWND hWnd) noexcept
        {
            if (!hWnd || !IsWindow(hWnd))
                return false;
            wchar_t cls[64]{};
            return GetClassNameW(hWnd, cls, static_cast<int>(std::size(cls))) > 0
                && _wcsicmp(cls, L"DropDown") == 0;
        }
    } // namespace

    HWND ResolveDropDownEffectHwnd(HWND hWnd) noexcept
    {
        if (!hWnd || !IsWindow(hWnd))
            return nullptr;

        if (IsDropDownShellClass(hWnd))
            return hWnd;

        for (HWND parent = GetParent(hWnd); parent; parent = GetParent(parent))
        {
            if (IsDropDownShellClass(parent))
                return parent;
        }

        if (HWND const root = GetAncestor(hWnd, GA_ROOT))
            return root;

        return hWnd;
    }

    bool IsOwnerDrawMenu(HWND hWnd)
    {
        HMENU hMenu = reinterpret_cast<HMENU>(SendMessageW(hWnd, MN_GETHMENU, 0, 0));
        if (!hMenu) return false;

        MENUITEMINFOW mii{ sizeof(mii), MIIM_FTYPE };
        int count = GetMenuItemCount(hMenu);
        for (int i = 0; i < count; i++)
        {
            if (GetMenuItemInfoW(hMenu, i, TRUE, &mii) && (mii.fType & MFT_OWNERDRAW))
                return true;
        }
        return false;
    }

    bool IsImmersiveMenu(HWND hWnd)
    {
        bool found = false;
        EnumPropsExW(hWnd,
            [](HWND, LPWSTR name, HANDLE, ULONG_PTR param) -> BOOL
            {
                if (HIWORD(name) && wcsstr(name, L"ImmersiveContextMenuArray_"))
                {
                    *reinterpret_cast<bool*>(param) = true;
                    return FALSE;
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&found));
        return found;
    }

    bool IsImmersiveDarkMode(HWND /*hWnd*/)
    {
        return IsSystemDarkMode();
    }

    bool IsTraverseLogMenu(HWND hWnd)
    {
        return GetPropW(hWnd, L"COwnerDrawPopupMenu_This") != nullptr;
    }

    HWND GetCurrentMenuOwner()
    {
        GUITHREADINFO gi{ sizeof(gi) };
        if (!GetGUIThreadInfo(GetCurrentThreadId(), &gi))
            return nullptr;
        return gi.hwndMenuOwner;
    }

    bool IsSystemDarkMode()
    {
        return Shared::ThemeHelper::IsSystemDarkMode();
    }

    void WalkMenuItems(HMENU hMenu, const std::function<bool(bool ownerDraw, ULONG_PTR itemData, UINT itemID)>& callback)
    {
        MENUITEMINFOW mii{};
        for (UINT i = 0; ; ++i)
        {
            mii.cbSize = sizeof(mii);
            mii.fMask = MIIM_FTYPE | MIIM_DATA | MIIM_ID;
            if (!GetMenuItemInfoW(hMenu, i, TRUE, &mii))
                break;
            if (!callback((mii.fType & MFT_OWNERDRAW) != 0, mii.dwItemData, mii.wID))
                break;
        }
    }

    void* GetContextMenuDataForItem(HWND hWnd, ULONG_PTR itemData, UINT itemID)
    {
        if (!itemData) return nullptr;
        wchar_t propName[128];
        swprintf_s(propName, L"ImmersiveContextMenuArray_%lu-%u", static_cast<ULONG>(itemData), itemID);
        return GetPropW(hWnd, propName);
    }

    MARGINS GetPopupMenuNonClientMargins(HWND hWnd)
    {
        RECT windowRect{};
        GetWindowRect(hWnd, &windowRect);

        MENUBARINFO mbi{ sizeof(MENUBARINFO) };
        GetMenuBarInfo(hWnd, OBJID_CLIENT, 0, &mbi);

        return {
            mbi.rcBar.left - windowRect.left,
            windowRect.right - mbi.rcBar.right,
            mbi.rcBar.top - windowRect.top,
            windowRect.bottom - mbi.rcBar.bottom
        };
    }

    bool HandlePopupMenuNonClientBorderColors(
    HDC hdc,
    const RECT& paintRect,
    const WndCompat::Menu::BorderContext& border)
{
    if (!border.colorUseNone)
    {
        if (border.colorUseDefault)
            return false;

        wil::unique_hbrush brush{
            Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(
                Shared::ColorUtils::ARGBToCOLORREF(border.color),
                Shared::ColorUtils::GetAlphaFromARGB(border.color)) };
        if (brush)
            FrameRect(hdc, &paintRect, brush.get());
    }
    else
    {
        FrameRect(hdc, &paintRect, GetStockBrush(BLACK_BRUSH));
    }

    return true;
}

    bool PaintFlyoutImmersiveStartBorderGdi(
        HDC hdc,
        const RECT& paintRect,
        bool darkMode) noexcept
    {
        if (!hdc)
            return false;

        const wchar_t* themeClass = darkMode
            ? L"DarkMode_ImmersiveStart::Menu"
            : L"LightMode_ImmersiveStart::Menu";

        wil::unique_htheme themeHandle{ OpenThemeData(nullptr, themeClass) };
        if (!themeHandle)
            return false;

        constexpr int kOutlinePx = 1;
        const int savedDC = SaveDC(hdc);
        ExcludeClipRect(hdc,
            paintRect.left + kOutlinePx,
            paintRect.top + kOutlinePx,
            paintRect.right - kOutlinePx,
            paintRect.bottom - kOutlinePx);

        const HRESULT hr = DrawThemeBackground(
            themeHandle.get(),
            hdc,
            MENU_POPUPBORDERS,
            0,
            &paintRect,
            nullptr);

        RestoreDC(hdc, savedDC);

        const bool painted = SUCCEEDED(hr);
        DebugLog(DBG_IMPORTANT,
            L"[GlassShared][MenuUtils] PaintFlyoutImmersiveStartBorderGdi painted=%d darkMode=%d",
            painted ? 1 : 0,
            darkMode ? 1 : 0);
        return painted;
    }

    bool PaintFlyoutSystemNcBorderXor(HWND hWnd, HRGN clipRgn) noexcept
    {
        if (!hWnd || !IsWindow(hWnd))
            return false;

        RECT windowRect{};
        if (!GetWindowRect(hWnd, &windowRect))
            return false;

        constexpr int kSystemOutlineSize = 1;
        wil::unique_hrgn windowRegion{ CreateRectRgnIndirect(&windowRect) };
        wil::unique_hrgn innerRegion{ CreateRectRgn(
            windowRect.left + kSystemOutlineSize,
            windowRect.top + kSystemOutlineSize,
            windowRect.right - kSystemOutlineSize,
            windowRect.bottom - kSystemOutlineSize) };
        if (!windowRegion || !innerRegion)
            return false;

        CombineRgn(windowRegion.get(), windowRegion.get(), innerRegion.get(), RGN_XOR);

        WPARAM const ncParam = clipRgn
            ? reinterpret_cast<WPARAM>(clipRgn)
            : reinterpret_cast<WPARAM>(windowRegion.get());

        (void)DefSubclassProc(hWnd, WM_NCPAINT, ncParam, 0);

        DebugLog(DBG_IMPORTANT,
            L"[GlassShared][MenuUtils] PaintFlyoutSystemNcBorderXor hwnd=0x%p",
            hWnd);
        return true;
    }

    bool PaintFlyoutDefaultBorderOutline(
        HDC hdc,
        HWND hwnd,
        const RECT& paintRect,
        const WndCompat::Menu::BorderContext& border,
        bool darkMode) noexcept
    {
        if (!hdc)
            return false;

        if (border.colorUseNone)
            return true;

        if (HandlePopupMenuNonClientBorderColors(hdc, paintRect, border))
            return true;

        if (!border.colorUseDefault)
            return false;

        if (Shared::SystemHelper::g_buildNumber < 22000
            && border.cornerType == DWMWCP_DONOTROUND
            && hwnd
            && IsWindow(hwnd))
        {
            return PaintFlyoutSystemNcBorderXor(hwnd, nullptr);
        }

        return PaintFlyoutImmersiveStartBorderGdi(hdc, paintRect, darkMode);
    }

    bool PaintFlyoutNcOutlineGdi(
        HDC hdc,
        const RECT& paintRect,
        const WndCompat::Menu::BorderContext& border,
        bool darkMode) noexcept
    {
        return PaintFlyoutDefaultBorderOutline(hdc, nullptr, paintRect, border, darkMode);
    }

    bool PaintFlyoutClientEdgeBorderForThumbnail(
        HDC hdc,
        HWND hwnd,
        const WndCompat::Menu::BorderContext& border,
        bool darkMode) noexcept
    {
        if (!hdc || !hwnd || !IsWindow(hwnd))
            return false;

        RECT clientRect{};
        if (!GetClientRect(hwnd, &clientRect))
            return false;

        const int width = clientRect.right - clientRect.left;
        const int height = clientRect.bottom - clientRect.top;
        if (width <= 0 || height <= 0)
            return false;

        const bool painted = PaintFlyoutDefaultBorderOutline(
            hdc, hwnd, clientRect, border, darkMode);

        DebugLog(
            DBG_IMPORTANT,
            L"[GlassShared][MenuUtils] PaintFlyoutClientEdgeBorderForThumbnail hwnd=0x%p cornerType=%d defer=1 painted=%d",
            hwnd,
            static_cast<int>(border.cornerType),
            painted ? 1 : 0);

        return painted;
    }

    void ApplyMenuDwmBorderColorFromTheme(
        HWND hwnd,
        bool darkMode,
        const WndCompat::Menu::BorderContext& border) noexcept
    {
        if (!hwnd || !IsWindow(hwnd))
            return;

        if (Shared::SystemHelper::g_buildNumber < 22000)
            return;

        if (border.colorUseNone)
        {
            COLORREF noColor = DWMWA_COLOR_NONE;
            (void)DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &noColor, sizeof(noColor));
            return;
        }

        if (!border.colorUseDefault)
        {
            COLORREF cr = RGB(
                (border.color >> 16) & 0xFF,
                (border.color >> 8) & 0xFF,
                border.color & 0xFF);
            (void)DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &cr, sizeof(cr));
            return;
        }

        const wchar_t* themeClass = darkMode
            ? L"DarkMode_ImmersiveStart::Menu"
            : L"LightMode_ImmersiveStart::Menu";

        wil::unique_htheme themeHandle{ OpenThemeData(nullptr, themeClass) };
        if (!themeHandle)
            return;

        COLORREF color{};
        if (SUCCEEDED(GetThemeColor(themeHandle.get(), MENU_POPUPBORDERS, 0, TMT_FILLCOLORHINT, &color)))
            (void)DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &color, sizeof(color));
    }

    bool PaintNativeThemePopupNcBorder(
        HWND hwnd,
        const WndCompat::Menu::BorderContext& border,
        HRGN clipRgn) noexcept
    {
        if (!hwnd || !IsWindow(hwnd))
            return false;

        if (border.cornerType == DWMWCP_DONOTROUND)
            return false;

        auto hdc = wil::GetWindowDC(hwnd);
        if (!hdc)
            return false;

        if (clipRgn)
            SelectClipRgn(hdc.get(), clipRgn);

        RECT paintRect{};
        if (!GetWindowRect(hwnd, &paintRect))
            return false;

        OffsetRect(&paintRect, -paintRect.left, -paintRect.top);

        MARGINS ncMargins = GetPopupMenuNonClientMargins(hwnd);
        if (ncMargins.cxLeftWidth == 0 && ncMargins.cxRightWidth == 0 &&
            ncMargins.cyTopHeight == 0 && ncMargins.cyBottomHeight == 0)
        {
            ncMargins = { 1, 1, 1, 1 };
        }

        // Только кольцо NC — не заливать клиент (пункты меню), паритет DropDown/FlyoutFrame.
        const int savedDcBlack = SaveDC(hdc.get());
        ExcludeClipRect(hdc.get(),
            paintRect.left + ncMargins.cxLeftWidth,
            paintRect.top + ncMargins.cyTopHeight,
            paintRect.right - ncMargins.cxRightWidth,
            paintRect.bottom - ncMargins.cyBottomHeight);
        PatBlt(hdc.get(), paintRect.left, paintRect.top,
            paintRect.right - paintRect.left,
            paintRect.bottom - paintRect.top,
            BLACKNESS);
        RestoreDC(hdc.get(), savedDcBlack);

        if (HandlePopupMenuNonClientBorderColors(hdc.get(), paintRect, border))
            return true;

        constexpr int kOutlinePx = 1;
        const int savedDC = SaveDC(hdc.get());
        ExcludeClipRect(hdc.get(),
            paintRect.left + kOutlinePx,
            paintRect.top + kOutlinePx,
            paintRect.right - kOutlinePx,
            paintRect.bottom - kOutlinePx);

        wil::unique_htheme menuTheme{ OpenThemeData(nullptr, L"Menu") };
        if (menuTheme)
        {
            (void)DrawThemeBackground(
                menuTheme.get(),
                hdc.get(),
                MENU_POPUPBORDERS,
                0,
                &paintRect,
                nullptr);
        }

        RestoreDC(hdc.get(), savedDC);
        return menuTheme != nullptr;
    }
}