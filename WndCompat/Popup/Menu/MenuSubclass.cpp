// WndCompat/Popup/Menu/MenuSubclass.cpp
// Единая реализация сабкласса для всех типов меню:
//   Immersive, TraverseLog, NativeTheme, Classic, ClassicOrNativeTheme, ThirdParty
//
// Ключевые точки:
//   MN_SIZEWINDOW   — определение типа меню, применение эффектов (CallOnce per HWND)
//   WM_PAINT/PRINT  — включение ETW-хуков (Classic) вокруг отрисовки (сабкласс живёт до WM_NCDESTROY)
//   WM_NCPAINT      — отрисовка non-client области только для Immersive/TraverseLog
//   WM_ERASEBKGND   — залить фон чёрным для всех типов (PatBlt BLACKNESS)
//
#include "WndPch.h"
#include <WndCompat.h>
#include "MenuSubclass.h"
#include "MenuPolicy.h"
#include "MenuUtils.h"
#include <Root/RootSubclass.h>
#include <Root/ExplorerRootSubclass.h>
#include <RootBackdropPolicy.h>
#include <SystemHelper.h>
#include <RegHelper.h>
#include "MenuEventProvider.h"
#include "MenuContext.h"
#include <Helpers/ThemeHelper.h>
#include <WndCompatHostApi.h>
#include <GlassMessages.h>
#include <Helpers/MenuUtils.h>
#include <dwmapi.h>
// Имя property для CallOnce-guard на MN_SIZEWINDOW (как BWM HwndCallOnce)
static constexpr wchar_t kSizeWindowOnceProp[] = L"GlassMenuSizeOnce";
// Первый DefSubclass(MN_SIZEWINDOW) может синхронно вызвать WM_UAH до основного OnAttachHooks —
// ранний OnAttachHooks (только «простое» меню без owner-draw) ставит детуры заранее.
static constexpr wchar_t kEarlyThemeHooksProp[] = L"GlassMenuEarlyThemeHooks";

namespace
{
    [[nodiscard]] bool IsFlyoutBorderDeferredForMenu(
        HWND hWnd,
        const WndCompat::Menu::MenuEventProvider* provider) noexcept
    {
        return provider && provider->IsFlyoutBorderDeferred
            && provider->IsFlyoutBorderDeferred(hWnd);
    }

    [[nodiscard]] bool TryEarlyAttachUxThemeHooksBeforeFirstMnDefSubclass(
        HWND hMenuWnd,
        const WndCompat::Menu::MenuEventProvider* provider,
        HWND owner,
        HMENU hMenu) noexcept
    {
        using WndCompat::Menu::MenuContext;
        using WndCompat::Menu::MenuType;

        if (!provider || !provider->OnAttachHooks || !hMenu)
            return false;

        if (owner && GetPropW(owner, L"COwnerDrawPopupMenu_This") != nullptr)
            return false;

        bool anyOwnerDraw = false;
        Shared::MenuUtils::WalkMenuItems(hMenu,
            [&](bool ownerDraw, ULONG_PTR /*itemData*/, UINT /*itemID*/) -> bool
            {
                if (ownerDraw)
                {
                    anyOwnerDraw = true;
                    return false;
                }
                return true;
            });
        if (anyOwnerDraw)
            return false;

        MenuContext ctx{};
        ctx.type = MenuType::ClassicOrNativeTheme;
        ctx.useDarkMode = false;
        ctx.useUxTheme = false;
        ctx.noSystemDropShadow =
            Shared::RegHelper::Get<DWORD>({ L"Menu" }, L"NoSystemDropShadow", 0) != 0;
        ctx.immersiveStyle =
            Shared::RegHelper::Get<DWORD>({ L"Menu" }, L"EnableImmersiveStyle", 1) != 0;

        provider->OnAttachHooks(hMenuWnd, ctx);
        SetPropW(hMenuWnd, kEarlyThemeHooksProp, reinterpret_cast<HANDLE>(1));
        DebugLog(L"[WndCompat][Menu] Early OnAttachHooks (до первого DefSubclass MN_SIZEWINDOW) hwnd=0x%p",
            hMenuWnd);
        return true;
    }

    // Catch-up: меню открылось до WinEvent attach gaRoot. Никогда не attach'ить child menu owner (Edit и т.п.).
    static void EnsureGaRootAttachedForMenuOwner(HWND menuOwner) noexcept
    {
        if (!menuOwner || !IsWindow(menuOwner))
            return;

        HWND gaRoot = GetAncestor(menuOwner, GA_ROOT);
        if (!gaRoot || !IsWindow(gaRoot))
            return;

        const bool ownerIsChild = (menuOwner != gaRoot)
            && ((GetWindowLongPtrW(menuOwner, GWL_STYLE) & WS_CHILD) != 0);
        const HWND attachTarget = ownerIsChild ? gaRoot : menuOwner;

        if (WndCompat::Root_IsAttached(attachTarget))
            return;

        if (Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(attachTarget)
            && Shared::SystemHelper::g_buildNumber >= 22000)
        {
            WndCompat::ExplorerRoot::Attach(attachTarget);
            DebugLog(L"[WndCompat][Menu] ExplorerRoot::Attach (MN_SIZEWINDOW catch-up) hwnd=0x%p owner=0x%p",
                attachTarget, menuOwner);
        }
        else
        {
            WndCompat::Root_Attach(attachTarget);
            DebugLog(L"[WndCompat][Menu] Root_Attach (MN_SIZEWINDOW catch-up) hwnd=0x%p owner=0x%p",
                attachTarget, menuOwner);
        }
    }

    // Immersive/TraverseLog: WM_DRAWITEM идёт на menuOwner (часто child shell view).
    // RootSubclassProc на owner обязателен — gaRoot-only catch-up недостаточен.
    static void EnsureMenuOwnerAttachedForImmersive(HWND menuOwner) noexcept
    {
        if (!menuOwner || !IsWindow(menuOwner))
            return;

        if (WndCompat::Root_IsAttached(menuOwner))
            return;

        if (Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(menuOwner)
            && Shared::SystemHelper::g_buildNumber >= 22000)
        {
            WndCompat::ExplorerRoot::Attach(menuOwner);
            DebugLog(L"[WndCompat][Menu] ExplorerRoot::Attach (Immersive menuOwner catch-up) hwnd=0x%p",
                menuOwner);
        }
        else
        {
            WndCompat::Root_Attach(menuOwner);
            DebugLog(L"[WndCompat][Menu] Root_Attach (Immersive menuOwner catch-up) hwnd=0x%p",
                menuOwner);
        }
    }

    static RECT GetMenuNcPaintRect(HWND hWnd)
    {
        RECT paintRect{};
        GetWindowRect(hWnd, &paintRect);
        OffsetRect(&paintRect, -paintRect.left, -paintRect.top);
        return paintRect;
    }

    static MARGINS GetMenuNcMargins(HWND hWnd)
    {
        MARGINS mr = Shared::MenuUtils::GetPopupMenuNonClientMargins(hWnd);
        if (mr.cxLeftWidth <= 0 || mr.cyTopHeight <= 0 ||
            mr.cxRightWidth <= 0 || mr.cyBottomHeight <= 0)
        {
            const int b = GetSystemMetrics(SM_CXEDGE);
            mr = { b, b, b, b };
        }
        return mr;
    }

    static void PaintMenuNcBlackFill(HDC hdc, HWND hWnd, const RECT& paintRect, const MARGINS& mr)
    {
        const int savedDC = SaveDC(hdc);
        ExcludeClipRect(hdc,
            paintRect.left + mr.cxLeftWidth,
            paintRect.top + mr.cyTopHeight,
            paintRect.right - mr.cxRightWidth,
            paintRect.bottom - mr.cyBottomHeight);
        PatBlt(hdc, paintRect.left, paintRect.top,
            wil::rect_width(paintRect), wil::rect_height(paintRect), BLACKNESS);
        RestoreDC(hdc, savedDC);
    }

    // GDI-рамка только при DWMWCP_DONOTROUND (как BWM MenuHandler WM_NCPAINT).
    static bool PaintMenuNcBorderDonotRound(
        HDC hdc,
        HWND hWnd,
        const RECT& paintRect,
        bool useSystemNcFallback)
    {
        using WndCompat::Menu::g_MenuContext;

        if (g_MenuContext.border.cornerType != DWMWCP_DONOTROUND)
            return false;

        HWND hwndForOutline = nullptr;
        if (useSystemNcFallback
            && g_MenuContext.border.colorUseDefault
            && !g_MenuContext.border.colorUseNone)
        {
            hwndForOutline = hWnd;
        }

        return Shared::MenuUtils::PaintFlyoutDefaultBorderOutline(
            hdc,
            hwndForOutline,
            paintRect,
            g_MenuContext.border,
            g_MenuContext.useDarkMode);
    }

    static void PaintClassicMenuNC(HDC hdc, HWND hWnd)
    {
        const RECT paintRect = GetMenuNcPaintRect(hWnd);
        const MARGINS mr = GetMenuNcMargins(hWnd);
        PaintMenuNcBlackFill(hdc, hWnd, paintRect, mr);
        (void)PaintMenuNcBorderDonotRound(hdc, hWnd, paintRect, false);
    }

    // WM_PRINT / DWM thumbnail: black NC ring → outline по краю window rect (паритет FlyoutFrame/DropDown).
    static void PaintMenuNcRingForThumbnailCapture(HDC hdc, HWND hWnd) noexcept
    {
        using WndCompat::Menu::g_MenuContext;

        const RECT paintRect = GetMenuNcPaintRect(hWnd);
        const MARGINS mr = GetMenuNcMargins(hWnd);
        PaintMenuNcBlackFill(hdc, hWnd, paintRect, mr);

        (void)Shared::MenuUtils::PaintFlyoutDefaultBorderOutline(
            hdc,
            nullptr,
            paintRect,
            g_MenuContext.border,
            g_MenuContext.useDarkMode);
    }

    static void PaintImmersiveMenuNonClient(HWND hWnd, HRGN clipRgn, LPCWSTR reason) noexcept
    {
        using WndCompat::Menu::g_MenuContext;

        auto hdc = wil::GetWindowDC(hWnd);
        if (!hdc)
            return;

        if (clipRgn)
            SelectClipRgn(hdc.get(), clipRgn);

        const RECT paintRect = GetMenuNcPaintRect(hWnd);
        const MARGINS mr = GetMenuNcMargins(hWnd);
        PaintMenuNcBlackFill(hdc.get(), hWnd, paintRect, mr);
        if (g_MenuContext.border.cornerType == DWMWCP_DONOTROUND)
            (void)PaintMenuNcBorderDonotRound(hdc.get(), hWnd, paintRect, true);
        else
        {
            (void)Shared::MenuUtils::PaintFlyoutDefaultBorderOutline(
                hdc.get(),
                hWnd,
                paintRect,
                g_MenuContext.border,
                g_MenuContext.useDarkMode);
        }

        DwmFlush();
        UNREFERENCED_PARAMETER(reason);
    }

    [[nodiscard]] bool IsDarkModeActiveForImmeriveMenu(
        bool useDarkTheme,
        bool useSystemPadding) noexcept
    {
        if (!useDarkTheme)
        {
            if (!useSystemPadding)
                return Shared::ThemeHelper::IsSystemDarkMode();

            return false;
        }

        return true;
    }

    LRESULT CALLBACK MenuSubclassProc(
        HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
        UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
    {
        using namespace WndCompat::Menu;

        auto provider = GetMenuEventProvider();

        if (uMsg == WndCompat::WM_GLASS_PAINT_FLYOUT_HOST_BORDER)
        {
            if (WndCompat::IsShuttingDown())
                return 0;

            DebugLog(DBG_IMPORTANT,
                L"[WndCompat][Menu] WM_GLASS_PAINT_FLYOUT_HOST_BORDER hwnd=0x%p type=%d",
                hWnd,
                static_cast<int>(g_MenuContext.type));

            if (provider && provider->OnPaintFlyoutHostBorder)
            {
                if (g_MenuContext.type == MenuType::NativeTheme)
                {
                    provider->OnPaintFlyoutHostBorder(
                        hWnd,
                        FlyoutBorderPass::PostUncloakSetupAndPaint,
                        nullptr);
                }
                else if (g_MenuContext.type == MenuType::Immersive)
                {
                    provider->OnPaintFlyoutHostBorder(
                        hWnd,
                        FlyoutBorderPass::SetupAndPaint,
                        nullptr);

                    WndCompat::Menu_FlyoutBorderRefresh(hWnd);

                    (void)SetWindowPos(
                        hWnd,
                        nullptr,
                        0,
                        0,
                        0,
                        0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

                    (void)RedrawWindow(
                        hWnd,
                        nullptr,
                        nullptr,
                        RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOCHILDREN);
                }
                else
                {
                    provider->OnPaintFlyoutHostBorder(
                        hWnd,
                        FlyoutBorderPass::SetupAndPaint,
                        nullptr);

                    WndCompat::Menu_FlyoutBorderRefresh(hWnd);
                }
            }

            return 0;
        }

        // -----------------------------------------------------------------------
        // WM_WINDOWPOSCHANGED — скрытие системной тени при SWP_SHOWWINDOW.
        // BWM делает это здесь. WM_SHOWWINDOW не обрабатывается в themed-сабклассе BWM.
        // -----------------------------------------------------------------------
        if (uMsg == WM_WINDOWPOSCHANGED)
        {
            const auto& windowPos = *reinterpret_cast<WINDOWPOS*>(lParam);
            if (windowPos.flags & SWP_SHOWWINDOW)
            {
                auto result = DefSubclassProc(hWnd, uMsg, wParam, lParam);

                if (g_MenuContext.noSystemDropShadow)
                {
                    HWND backdropWindow = GetWindow(hWnd, GW_HWNDNEXT);
                    if (backdropWindow)
                    {
                        wchar_t cls[16]{};
                        GetClassNameW(backdropWindow, cls, _countof(cls));
                        if (wcscmp(cls, L"SysShadow") == 0)
                            ShowWindow(backdropWindow, SW_HIDE);
                    }
                }

                if (g_MenuContext.type == MenuType::Immersive ||
                    g_MenuContext.type == MenuType::TraverseLog)
                {
                    PaintImmersiveMenuNonClient(hWnd, nullptr, L"show");
                }
                return result;
            }
        }

        // -----------------------------------------------------------------------
        // WM_NCPAINT — Immersive/TraverseLog: полная замена NC.
        // Classic: сначала DefSubclass (системная рамка), затем наша рамка поверх —
        // при смене пункта меню-бара часто идут NCPAINT/PAINT без WM_PRINT.
        // NativeTheme → OnPaintFlyoutHostBorder (DTB + DWM, без DefSubclassProc).
        // -----------------------------------------------------------------------
        if (uMsg == WM_NCPAINT)
        {
            if (g_MenuContext.type == MenuType::Immersive ||
                g_MenuContext.type == MenuType::TraverseLog)
            {
                HRGN clipRgn = nullptr;
                if (wParam != NULLREGION && wParam != ERROR)
                    clipRgn = reinterpret_cast<HRGN>(wParam);

                PaintImmersiveMenuNonClient(hWnd, clipRgn, L"ncpaint");
                return 0;
            }
            if (g_MenuContext.type == MenuType::Classic)
            {
                if (IsFlyoutBorderDeferredForMenu(hWnd, provider))
                    return DefSubclassProc(hWnd, uMsg, wParam, lParam);

                DebugLog(L"[WndCompat][Menu] Menu: WM_NCPAINT (Classic) hwnd=%p wParam=%p", hWnd, (void*)wParam);
                (void)DefSubclassProc(hWnd, uMsg, wParam, lParam);
                auto hdc = wil::GetWindowDC(hWnd);
                if (hdc)
                {
                    if (wParam != NULLREGION && wParam != ERROR)
                        SelectClipRgn(hdc.get(), reinterpret_cast<HRGN>(wParam));
                    else
                        SelectClipRgn(hdc.get(), nullptr);
                    PaintClassicMenuNC(hdc.get(), hWnd);
                }
                return 0;
            }
            if (g_MenuContext.type == MenuType::NativeTheme &&
                g_MenuContext.border.cornerType != DWMWCP_DONOTROUND)
            {
                if (IsFlyoutBorderDeferredForMenu(hWnd, provider))
                    return DefSubclassProc(hWnd, uMsg, wParam, lParam);

                HRGN clipRgn = nullptr;
                if (wParam != NULLREGION && wParam != ERROR)
                    clipRgn = reinterpret_cast<HRGN>(wParam);

                if (provider && provider->OnPaintFlyoutHostBorder)
                {
                    provider->OnPaintFlyoutHostBorder(
                        hWnd,
                        FlyoutBorderPass::NcPaintOnly,
                        clipRgn);
                }
                return 0;
            }
        }

        // -----------------------------------------------------------------------
        // WM_ERASEBKGND — чернение фона для ВСЕХ типов меню (как в BWM).
        // Критично для анимации: thumbnail должен захватывать чёрный фон с альфа-каналом,
        // иначе изображение меню будет непрозрачным во время анимации.
        // -----------------------------------------------------------------------
        if (uMsg == WM_ERASEBKGND)
        {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            RECT clipRect{};
            GetClipBox(hdc, &clipRect);
            PatBlt(hdc,
                clipRect.left, clipRect.top,
                clipRect.right - clipRect.left,
                clipRect.bottom - clipRect.top,
                BLACKNESS);
            return TRUE;
        }

        // -----------------------------------------------------------------------
        // WM_PAINT — Classic: ETW + UAH bracket; после DefSubclass — PaintClassicMenuNC
        // на window DC (путь «соседнее меню» часто только WM_PAINT без WM_PRINT).
        // -----------------------------------------------------------------------
        if (uMsg == WM_PAINT)
        {
            if (g_MenuContext.type == MenuType::Classic)
            {
                DebugLog(L"[WndCompat][Menu] Menu (Classic): WM_PAINT hwnd=0x%p", hWnd);

                if (provider && provider->OnClassicMenuPopupPaintExtTextOutBegin)
                    provider->OnClassicMenuPopupPaintExtTextOutBegin(hWnd);

                if (provider && provider->OnUxHooksBegin)
                    provider->OnUxHooksBegin(hWnd, g_MenuContext);

                auto result = DefSubclassProc(hWnd, uMsg, wParam, lParam);

                if (provider && provider->OnUxHooksEnd)
                    provider->OnUxHooksEnd(hWnd, g_MenuContext);

                if (provider && provider->OnClassicMenuPopupPaintExtTextOutEnd)
                    provider->OnClassicMenuPopupPaintExtTextOutEnd(hWnd);

                if (!IsFlyoutBorderDeferredForMenu(hWnd, provider))
                {
                    if (auto hdcWin = wil::GetWindowDC(hWnd))
                        PaintClassicMenuNC(hdcWin.get(), hWnd);
                }

                return result;
            }
        }

        // -----------------------------------------------------------------------
        // WM_PRINT — Immersive/TraverseLog: NC-ring для thumbnail (black fill + outline).
        //            Для Classic и ClassicOrNativeTheme: ETW + uxhooks bracket (усиление пробы).
        // -----------------------------------------------------------------------
        if (uMsg == WM_PRINT)
        {
            if (g_MenuContext.type == MenuType::Immersive ||
                g_MenuContext.type == MenuType::TraverseLog)
            {
                auto result = DefSubclassProc(hWnd, uMsg, wParam, lParam);
                if (lParam & PRF_NONCLIENT)
                    PaintMenuNcRingForThumbnailCapture(reinterpret_cast<HDC>(wParam), hWnd);
                return result;
            }
            else if (g_MenuContext.type == MenuType::Classic ||
                g_MenuContext.type == MenuType::ClassicOrNativeTheme)
            {
                DebugLog(L"[WndCompat][Menu] Menu (%s): WM_PRINT hwnd=0x%p",
                    g_MenuContext.type == MenuType::Classic ? L"Classic" : L"ClassicOrNativeTheme",
                    hWnd);

                if (lParam & PRF_CLIENT)
                {
                    HDC hdcPrint = reinterpret_cast<HDC>(wParam);
                    RECT clipRect{};
                    GetClipBox(hdcPrint, &clipRect);
                    PatBlt(hdcPrint,
                        clipRect.left, clipRect.top,
                        clipRect.right - clipRect.left,
                        clipRect.bottom - clipRect.top,
                        BLACKNESS);
                }

                if (provider && provider->OnClassicMenuPopupPaintExtTextOutBegin)
                    provider->OnClassicMenuPopupPaintExtTextOutBegin(hWnd);

                if (provider && provider->OnUxHooksBegin)
                    provider->OnUxHooksBegin(hWnd, g_MenuContext);

                auto result = DefSubclassProc(hWnd, uMsg, wParam, lParam);

                if (provider && provider->OnUxHooksEnd)
                    provider->OnUxHooksEnd(hWnd, g_MenuContext);

                if (provider && provider->OnClassicMenuPopupPaintExtTextOutEnd)
                    provider->OnClassicMenuPopupPaintExtTextOutEnd(hWnd);

                if ((lParam & PRF_NONCLIENT) && !IsFlyoutBorderDeferredForMenu(hWnd, provider))
                    PaintClassicMenuNC(reinterpret_cast<HDC>(wParam), hWnd);
                return result;
            }
        }

        // -----------------------------------------------------------------------
        // MN_BUTTONUP — fade-out анимация при клике по пункту
        // -----------------------------------------------------------------------
        if (uMsg == MN_BUTTONUP)
        {
            if (g_MenuContext.type == MenuType::Immersive ||
                g_MenuContext.type == MenuType::TraverseLog)
            {
                if (provider && provider->OnButtonUp)
                {
                    UINT itemPos = static_cast<UINT>(wParam);
                    MENUBARINFO mbi{};
                    mbi.cbSize = sizeof(mbi);
                    if (GetMenuBarInfo(hWnd, OBJID_CLIENT, static_cast<LONG>(itemPos) + 1, &mbi))
                    {
                        BOOL selFade = TRUE;
                        SystemParametersInfoW(SPI_GETSELECTIONFADE, 0, &selFade, 0);
                        if (selFade)
                            SystemParametersInfoW(SPI_SETSELECTIONFADE, 0, reinterpret_cast<PVOID>(FALSE), 0);

                        provider->OnButtonUp(hWnd, g_MenuContext, mbi);

                        LRESULT result = DefSubclassProc(hWnd, uMsg, wParam, lParam);

                        if (selFade)
                            SystemParametersInfoW(SPI_SETSELECTIONFADE, 0, reinterpret_cast<PVOID>(TRUE), 0);

                        return result;
                    }
                }
            }
        }

        // -----------------------------------------------------------------------
        // MN_SIZEWINDOW — CallOnce per HWND (как BWM HwndCallOnce):
        // определение типа, применение хуков и эффектов выполняется однократно.
        // -----------------------------------------------------------------------
        if (uMsg == MN_SIZEWINDOW)
        {
            // До первого DefSubclass(MN_SIZEWINDOW) синхронно могут прийти WM_UAH — без детуров
            // MyDrawThemeBackground/MyDrawTextExW не участвуют (сломанный nativeTheme).
            if (GetPropW(hWnd, kSizeWindowOnceProp) == nullptr)
            {
                HWND ownerPre = Shared::MenuUtils::GetCurrentMenuOwner();
                HMENU hMenuPre = reinterpret_cast<HMENU>(SendMessageW(hWnd, MN_GETHMENU, 0, 0));
                (void)TryEarlyAttachUxThemeHooksBeforeFirstMnDefSubclass(hWnd, provider, ownerPre, hMenuPre);
            }

            LRESULT result = DefSubclassProc(hWnd, uMsg, wParam, lParam);

            // CallOnce: повторный MN_SIZEWINDOW на этом HWND — пропускаем
            if (GetPropW(hWnd, kSizeWindowOnceProp) != nullptr)
                return result;
            SetPropW(hWnd, kSizeWindowOnceProp, reinterpret_cast<HANDLE>(1));

            HWND owner = Shared::MenuUtils::GetCurrentMenuOwner();

            HMENU hMenu = reinterpret_cast<HMENU>(
                DefSubclassProc(hWnd, MN_GETHMENU, 0, 0)
                );

            // ── Шаг 1: TraverseLog ──
            if (owner && GetPropW(owner, L"COwnerDrawPopupMenu_This") != nullptr)
            {
                g_MenuContext.type = MenuType::TraverseLog;
                g_MenuContext.useDarkMode = Shared::ThemeHelper::IsSystemDarkMode();
                DebugLog(L"[WndCompat][Menu] MenuPolicy: TraverseLog menu hwnd=0x%p", hWnd);
            }
            else
            {
                // ── Шаг 2: разбор owner-draw пунктов ──
                bool hasOwnerDraw = false;
                bool hasImmersiveData = false;

                ContextMenuRenderingData* firstMenuDataForImmersive{
                    nullptr
                };

                Shared::MenuUtils::WalkMenuItems(hMenu,
                    [&](bool ownerDraw, ULONG_PTR itemData, UINT itemID) -> bool
                    {
                        if (ownerDraw)
                        {
                            hasOwnerDraw = true;

                            if (owner)
                            {
                                auto* menuData =
                                    reinterpret_cast<ContextMenuRenderingData*>(
                                        Shared::MenuUtils::GetContextMenuDataForItem(owner, itemData, itemID));

                                if (menuData)
                                {
                                    hasImmersiveData = true;
                                    firstMenuDataForImmersive = menuData;
                                    return false;
                                }
                            }
                        }

                        return true;
                    });

                if (hasOwnerDraw && hasImmersiveData)
                {
                    g_MenuContext.type = MenuType::Immersive;

                    if (firstMenuDataForImmersive)
                    {
                        g_MenuContext.useDarkMode =
                            IsDarkModeActiveForImmeriveMenu(
                                firstMenuDataForImmersive->useDarkTheme,
                                firstMenuDataForImmersive->useSystemPadding);
                    }
                    else
                    {
                        g_MenuContext.useDarkMode = Shared::ThemeHelper::IsSystemDarkMode();
                    }

                    DebugLog(L"[WndCompat][Menu] MenuPolicy: Immersive menu hwnd=0x%p", hWnd);
                }
                else if (hasOwnerDraw)
                {
                    g_MenuContext.type = MenuType::ThirdParty;
                    DebugLog(L"[WndCompat][Menu] MenuPolicy: ThirdParty menu — detaching hwnd=0x%p", hWnd);
                    WndCompat::Menu_Detach(hWnd);
                    return result;
                }
                else
                {
                    g_MenuContext.type = MenuType::ClassicOrNativeTheme;
                    g_MenuContext.useDarkMode = false;
                    g_MenuContext.useUxTheme = false;
                    DebugLog(L"[WndCompat][Menu] MenuPolicy: ClassicOrNativeTheme (probe) hwnd=0x%p", hWnd);
                }
            }

            if (g_MenuContext.type == MenuType::Immersive ||
                g_MenuContext.type == MenuType::TraverseLog)
            {
                EnsureMenuOwnerAttachedForImmersive(owner);
            }
            else if (g_MenuContext.type == MenuType::ClassicOrNativeTheme ||
                g_MenuContext.type == MenuType::NativeTheme ||
                g_MenuContext.type == MenuType::Classic)
            {
                EnsureGaRootAttachedForMenuOwner(owner);
            }

            g_MenuContext.noSystemDropShadow =
                Shared::RegHelper::Get<DWORD>({ L"Menu" }, L"NoSystemDropShadow", 0) != 0;

            g_MenuContext.immersiveStyle =
                Shared::RegHelper::Get<DWORD>({ L"Menu" }, L"EnableImmersiveStyle", 1) != 0;

            DebugLog(L"[WndCompat][Menu] Menu settings: type=%d dark=%d noShadow=%d immersive=%d",
                static_cast<int>(g_MenuContext.type),
                g_MenuContext.useDarkMode ? 1 : 0,
                g_MenuContext.noSystemDropShadow ? 1 : 0,
                g_MenuContext.immersiveStyle ? 1 : 0);

            if (provider)
            {
                const bool earlyHooks = (GetPropW(hWnd, kEarlyThemeHooksProp) != nullptr);
                const bool earlyMatchesFinalType =
                    earlyHooks &&
                    (g_MenuContext.type == MenuType::ClassicOrNativeTheme ||
                        g_MenuContext.type == MenuType::NativeTheme ||
                        g_MenuContext.type == MenuType::Classic);

                if (earlyMatchesFinalType)
                {
                    DebugLog(L"[WndCompat][Menu] OnAttachHooks: пропуск (ранний pre-MN уже включил theme hooks) hwnd=0x%p type=%d",
                        hWnd, static_cast<int>(g_MenuContext.type));
                }
                else
                {
                    if (earlyHooks)
                    {
                        // Ранний путь предполагал простое меню; финальная политика — Immersive/TraverseLog/ThirdParty.
                        WndCompat::Menu::MenuContext undoEarly{};
                        undoEarly.type = WndCompat::Menu::MenuType::ClassicOrNativeTheme;
                        undoEarly.useDarkMode = false;
                        undoEarly.useUxTheme = false;
                        if (provider->OnDetachHooks)
                            provider->OnDetachHooks(hWnd, undoEarly);
                        RemovePropW(hWnd, kEarlyThemeHooksProp);
                    }

                    if (provider->OnAttachHooks)
                        provider->OnAttachHooks(hWnd, g_MenuContext);
                }

                // BWM: для ClassicOrNativeTheme — PrintWindow → DTB/WM_UAHINITMENU
                if (g_MenuContext.type == MenuType::ClassicOrNativeTheme)
                {
                    g_MenuContext.useUxTheme = false;
                    // Весь PrintWindow под флагом сессии: внутри WM_PRINT может не идти через UAH-bracket.
                    if (provider && provider->OnUxHooksBegin)
                        provider->OnUxHooksBegin(hWnd, g_MenuContext);
#pragma warning(suppress : 6387)
                    (void)PrintWindow(hWnd, nullptr, 0);
                    if (provider && provider->OnUxHooksEnd)
                        provider->OnUxHooksEnd(hWnd, g_MenuContext);
                    if (provider->SyncMenuContextFromMenuHandler)
                        provider->SyncMenuContextFromMenuHandler(&g_MenuContext);

                    // WM_UAHINITMENU внутри PrintWindow может прийти до WM_UAHDRAWMENUITEM: GlassCore
                    // выставляет Classic, затем DTB на 0x0092 — коррекция через MenuEventProvider.
                    if (provider->FinalizeNativeThemeAfterPrintWindowProbe)
                        provider->FinalizeNativeThemeAfterPrintWindowProbe(&g_MenuContext);
                }
            }

            // BWM: ThirdParty — ранний детач. Classic не детачим здесь: иначе первый кадр #32768
            // рисуется без нашего WM_PAINT (нет bracket ETW/glow).
            if (g_MenuContext.type == MenuType::ThirdParty)
            {
                DebugLog(L"[WndCompat][Menu] MenuPolicy: ThirdParty — detaching hwnd=0x%p", hWnd);
                RemovePropW(hWnd, kSizeWindowOnceProp);
                WndCompat::Menu_Detach(hWnd);
                return result;
            }

            // BWM MenuNT: backdrop для NativeTheme, Classic и оставшегося ClassicOrNativeTheme (если PrintWindow не промотировал).
            if (g_MenuContext.type >= MenuType::NativeTheme ||
                g_MenuContext.type == MenuType::Classic ||
                g_MenuContext.type == MenuType::ClassicOrNativeTheme)
            {
                if (provider && provider->OnPopupSized)
                    provider->OnPopupSized(hWnd, g_MenuContext);
                if (provider && provider->SyncMenuContextFromMenuHandler)
                    provider->SyncMenuContextFromMenuHandler(&g_MenuContext);
            }

            // SWP_FRAMECHANGED — только если есть стрелки прокрутки
            MARGINS mr = Shared::MenuUtils::GetPopupMenuNonClientMargins(hWnd);
            constexpr int kStandardNCMargin = 2;
            if (mr.cyTopHeight != kStandardNCMargin)
            {
                DebugLog(L"[WndCompat][Menu] MN_SIZEWINDOW: scroll arrows detected, forcing SWP_FRAMECHANGED");
                SetWindowPos(
                    hWnd,
                    nullptr,
                    0, 0, 0, 0,
                    SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOMOVE | SWP_NOSIZE |
                    SWP_FRAMECHANGED | SWP_NOACTIVATE
                );
            }

            return result;
        }

        // -----------------------------------------------------------------------
        // UAH-сообщения — bracket uxtheme-хуков
        // -----------------------------------------------------------------------
        if (uMsg == WM_UAHNCPAINTMENUPOPUP ||
            uMsg == WM_UAHDRAWMENU ||
            uMsg == WM_UAHDRAWMENUITEM ||
            uMsg == WM_UAHMEASUREMENUITEM)
        {
            DebugLog(L"[WndCompat][Menu] Menu: WM_UAH message 0x%04x hwnd=0x%p", uMsg, hWnd);

            if (provider && provider->OnUxHooksBegin)
                provider->OnUxHooksBegin(hWnd, g_MenuContext);

            auto result = DefSubclassProc(hWnd, uMsg, wParam, lParam);

            if (provider && provider->OnUxHooksEnd)
                provider->OnUxHooksEnd(hWnd, g_MenuContext);

            return result;
        }

        if (uMsg == WM_UAHINITMENU)
        {
            DebugLog(L"[WndCompat][Menu] Menu: WM_UAHINITMENU hwnd=0x%p", hWnd);

            if (provider && provider->OnUxHooksBegin)
                provider->OnUxHooksBegin(hWnd, g_MenuContext);

            auto result = DefSubclassProc(hWnd, uMsg, wParam, lParam);

            if (provider && provider->OnUxHooksEnd)
                provider->OnUxHooksEnd(hWnd, g_MenuContext);

            if (provider && provider->ApplyUaInitMenuNativeOrClassic)
                provider->ApplyUaInitMenuNativeOrClassic(result, &g_MenuContext);

            return result;
        }

        // -----------------------------------------------------------------------
        // WM_NCDESTROY — вызываем OnDetachHooks для корректного декремента
        // g_themeRef (E8 patches, Detours). Без этого ref-leak приводит к тому,
        // что хуки остаются включёнными после закрытия меню.
        // -----------------------------------------------------------------------
        if (uMsg == WM_NCDESTROY)
        {
            DebugLog(L"[WndCompat][Menu] Menu: WM_NCDESTROY hwnd=0x%p", hWnd);

            RemovePropW(hWnd, kSizeWindowOnceProp);
            RemovePropW(hWnd, kEarlyThemeHooksProp);

            if (provider && provider->OnDetachHooks)
                provider->OnDetachHooks(hWnd, g_MenuContext);

            if (provider && provider->OnPopupDestroyed)
                provider->OnPopupDestroyed(hWnd, g_MenuContext);

            Policy::OnPopupDestroy(hWnd);

            RemoveWindowSubclass(hWnd, MenuSubclassProc, 1);
            return DefSubclassProc(hWnd, uMsg, wParam, lParam);
        }

        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }
}


namespace WndCompat
{
#include <commctrl.h>

    void Menu_Attach(HWND hWnd)
    {
        if (!IsWindow(hWnd))
            return;

        static bool s_hostApiRegistered = false;
        if (!s_hostApiRegistered)
        {
            Shared::WndCompatHost::RegisterMenuFlyoutBorderRefresh(Menu_FlyoutBorderRefresh);
            s_hostApiRegistered = true;
        }

        SetLastError(0);
        BOOL ok = SetWindowSubclass(hWnd, MenuSubclassProc, 1, 0);
        DWORD err = GetLastError();

        if (ok)
        {
            DebugLog(L"[WndCompat][Menu] Menu_Attach: subclassed hwnd=0x%p", hWnd);
        }
        else
        {
            DebugLog(L"[WndCompat][Menu] Menu_Attach: FAILED hwnd=0x%p GetLastError=%lu",
                hWnd, err);
        }
    }

    void Menu_AttachIfNeeded(HWND hWnd)
    {
        if (!IsWindow(hWnd) || WndCompat::IsShuttingDown())
            return;

        DWORD_PTR ref{};
        if (::GetWindowSubclass(hWnd, MenuSubclassProc, 1, &ref) != FALSE)
            return;

        Menu_Attach(hWnd);
    }

    void Menu_Detach(HWND hWnd)
    {
        RemovePropW(hWnd, kSizeWindowOnceProp);
        RemovePropW(hWnd, kEarlyThemeHooksProp);

        if (auto provider = Menu::GetMenuEventProvider())
        {
            if (provider->OnDetachHooks)
                provider->OnDetachHooks(hWnd, Menu::g_MenuContext);
            if (provider->OnPopupDestroyed)
                provider->OnPopupDestroyed(hWnd, Menu::g_MenuContext);
        }

        if (IsWindow(hWnd))
            RemoveWindowSubclass(hWnd, MenuSubclassProc, 1);

        DebugLog(L"[WndCompat][Menu] Menu_Detach: hwnd=0x%p", hWnd);
    }

    void Menu_FlyoutBorderRefresh(HWND hWnd) noexcept
    {
        if (!IsWindow(hWnd))
            return;

        if (Menu::g_MenuContext.type == Menu::MenuType::TraverseLog)
            PaintImmersiveMenuNonClient(hWnd, nullptr, L"flyoutRefresh");
    }
}