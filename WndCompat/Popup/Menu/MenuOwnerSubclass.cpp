// WndCompat/Popup/Menu/MenuOwnerSubclass.cpp
#include "WndPch.h"
#include <WndCompat.h>
#include <InjectionMode.h>
#include <Tracker/WinEventHandler.h>
#include "MenuOwnerSubclass.h"
#include "MenuPolicy.h"
#include "MenuContext.h"
#include "MenuEventProvider.h"
#include <GlassBackdropMark.h>
#include <WinApiUtils.h>
#include <BitmapUtils.h>
#include <ColorUtils.h>
#include <wil.h>
#include <uah.h>

namespace WndCompat
{
    static std::vector<HWND> g_menuOwnerWindows;

    bool IsMenuOwnerAttached_Internal(HWND hOwner)
    {
        return std::find(g_menuOwnerWindows.begin(), g_menuOwnerWindows.end(), hOwner) != g_menuOwnerWindows.end();
    }

    static Menu::ContextMenuRenderingData* GetContextMenuDataForItem(HWND hWnd, ULONG_PTR itemData, UINT itemID)
    {
        if (!itemData) return nullptr;
        wchar_t propName[128];
        swprintf_s(propName, L"ImmersiveContextMenuArray_%lu-%u", static_cast<ULONG>(itemData), itemID);
        return reinterpret_cast<Menu::ContextMenuRenderingData*>(GetPropW(hWnd, propName));
    }

    struct BitmapRestore
    {
        HBITMAP* target;
        HBITMAP original;
        BitmapRestore(HBITMAP* t, HBITMAP o) : target(t), original(o) {}
        ~BitmapRestore() { if (target) *target = original; }
    };

    // =========================================================================
    // MenuOwnerSubclass — вызывается из RootSubclassProc (owner HWND):
    //   WM_DRAWITEM / WM_MEASUREITEM / WM_UAH* для меню-бара.
    // Два уровня сессий (как BWM WinHandler vs MenuSubclassProc):
    //   • OnMenuBarUxThemeDrawBegin/End — только WM_UAHDRAWMENU / WM_UAHDRAWMENUITEM
    //     (флаг MenuHandler + EnableThemeHooks + Classic ETW в GlassCore).
    //   • OnMenuBarClassicUahInitMenuBracket* — WM_UAHINITMENU: HWND+ETW на DefSubclass
    //     (неактивный NC: ExtTextOut с DC памяти — см. API Monitor).
    //   • OnUxHooksBegin/End — WM_UAHNCPAINTMENUPOPUP, WM_UAHMEASUREMENUITEM, WM_UAHINITMENU (внутри — после bracket),
    //     WM_MEASUREITEM(ODT_MENU): только SetUxHooksEnabled (аналог g_uxhooks в BWM).
    // Classic строка меню без UAH: BWM включает User32 ExtTextOut вокруг WM_NCACTIVATE /
    // WM_NCPAINT и удержание по HTMENU / WM_ENTERIDLE(MSGF_MENU), снятие WM_NCMOUSELEAVE —
    // см. MenuOwner_HandleClassicMenuBar* (колбеки GlassCore → UxThemeHooks).
    // Меню-бар (NC + WM_UAHDRAWMENU*) — Full + GlassIsBackdropAllowedForHwnd (NoBackdrop).
    // =========================================================================

    namespace
    {
        [[nodiscard]] bool IsMenuBarGlassCustomizationEnabled(HWND hOwner) noexcept
        {
            if (WndCompat::WinEventHook_GetMode() != Shared::InjectionMode::Full)
                return false;
            if (WndCompat::IsShuttingDown())
                return false;
            return GlassShared::GlassIsBackdropAllowedForHwnd(hOwner);
        }

        [[nodiscard]] bool IsClassicMenuBarExtTextOutTarget(HWND hWnd) noexcept
        {
            if (!IsMenuBarGlassCustomizationEnabled(hWnd))
                return false;
            if (!IsWindow(hWnd))
                return false;
            if (GetAncestor(hWnd, GA_ROOT) != hWnd)
                return false;
            if (!IsMenu(GetMenu(hWnd)))
                return false;
            return !Shared::WinApiUtils::IsThemed(hWnd, L"Menu");
        }
    }

    LRESULT MenuOwnerSubclass(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
    {
        DebugLog(L"[WndCompat][MenuOwner] Msg hwnd=0x%p uMsg=0x%X", hWnd, uMsg);

        switch (uMsg)
        {
        case WM_UAHDRAWMENUITEM:
        case WM_UAHDRAWMENU:
        {
            if (!IsMenuBarGlassCustomizationEnabled(hWnd))
                return DefSubclassProc(hWnd, uMsg, wParam, lParam);

            // BWM WinHandler по смыслу: Begin → DefSubclassProc → End.
            // wil::scope_exit: End вызывается при выходе из блока (после return DefSubclassProc).
            DebugLog(L"[WndCompat][MenuOwner] WM_UAH DRAW 0x%04x hwnd=0x%p wParam=0x%p lParam=0x%p",
                uMsg, hWnd, (void*)wParam, (void*)lParam);

            auto provider = WndCompat::Menu::GetMenuEventProvider();
            bool beginCalled = false;
            if (provider && provider->OnMenuBarUxThemeDrawBegin)
            {
                provider->OnMenuBarUxThemeDrawBegin(hWnd, WndCompat::Menu::g_MenuContext);
                beginCalled = true;
            }

            auto restoreMenuBarUx = wil::scope_exit([&] {
                if (beginCalled && provider && provider->OnMenuBarUxThemeDrawEnd)
                    provider->OnMenuBarUxThemeDrawEnd(hWnd, WndCompat::Menu::g_MenuContext);
                });

            return DefSubclassProc(hWnd, uMsg, wParam, lParam);
        }

        case WM_UAHNCPAINTMENUPOPUP:
        {
            // BWM MenuSubclassProc 642–648: только g_uxhooks — та же явная последовательность.
            DebugLog(L"[WndCompat][MenuOwner] WM_UAHNCPAINTMENUPOPUP hwnd=0x%p", hWnd);

            auto provider = WndCompat::Menu::GetMenuEventProvider();
            if (provider && provider->OnUxHooksBegin)
                provider->OnUxHooksBegin(hWnd, WndCompat::Menu::g_MenuContext);

            LRESULT result = DefSubclassProc(hWnd, uMsg, wParam, lParam);

            if (provider && provider->OnUxHooksEnd)
                provider->OnUxHooksEnd(hWnd, WndCompat::Menu::g_MenuContext);

            return result;
        }

        case WM_UAHINITMENU:
        {
            DebugLog(L"[WndCompat][MenuOwner] WM_UAHINITMENU hwnd=0x%p", hWnd);

            auto provider = WndCompat::Menu::GetMenuEventProvider();
            if (provider && provider->SyncMenuContextFromMenuHandler)
                provider->SyncMenuContextFromMenuHandler(&WndCompat::Menu::g_MenuContext);

            const bool classicBarEtw = IsClassicMenuBarExtTextOutTarget(hWnd);
            if (classicBarEtw && provider && provider->OnMenuBarClassicUahInitMenuBracketBegin)
                provider->OnMenuBarClassicUahInitMenuBracketBegin(hWnd);

            if (provider && provider->OnUxHooksBegin)
                provider->OnUxHooksBegin(hWnd, WndCompat::Menu::g_MenuContext);

            LRESULT result = DefSubclassProc(hWnd, uMsg, wParam, lParam);

            if (provider && provider->OnUxHooksEnd)
                provider->OnUxHooksEnd(hWnd, WndCompat::Menu::g_MenuContext);

            if (classicBarEtw && provider && provider->OnMenuBarClassicUahInitMenuBracketEnd)
                provider->OnMenuBarClassicUahInitMenuBracketEnd(hWnd);

            if (provider && provider->ApplyUaInitMenuNativeOrClassic)
                provider->ApplyUaInitMenuNativeOrClassic(result, &WndCompat::Menu::g_MenuContext);

            return result;
        }

        case WM_UAHMEASUREMENUITEM:
        {
            auto mis = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
            DebugLog(L"[WndCompat][MenuOwner] WM_UAHMEASUREMENUITEM hwnd=0x%p itemID=%u",
                hWnd, mis ? mis->itemID : 0u);

            auto provider = WndCompat::Menu::GetMenuEventProvider();
            if (provider && provider->OnUxHooksBegin)
                provider->OnUxHooksBegin(hWnd, WndCompat::Menu::g_MenuContext);

            if (mis && mis->CtlType == ODT_MENU && Menu::Policy::OnOwnerMeasureItem(hWnd, mis))
            {
                DebugLog(L"[WndCompat][MenuOwner] WM_UAHMEASUREMENUITEM handled by Policy hwnd=0x%p", hWnd);
                if (provider && provider->OnUxHooksEnd)
                    provider->OnUxHooksEnd(hWnd, WndCompat::Menu::g_MenuContext);
                return TRUE;
            }

            LRESULT result = DefSubclassProc(hWnd, uMsg, wParam, lParam);

            if (provider && provider->OnUxHooksEnd)
                provider->OnUxHooksEnd(hWnd, WndCompat::Menu::g_MenuContext);

            return result;
        }

        case WM_MEASUREITEM:
        {
            auto mis = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
            if (!mis || mis->CtlType != ODT_MENU)
                break;

            DebugLog(L"[WndCompat][MenuOwner] WM_MEASUREITEM hwnd=0x%p itemID=%u", hWnd, mis->itemID);

            auto provider = WndCompat::Menu::GetMenuEventProvider();
            if (provider && provider->OnUxHooksBegin)
                provider->OnUxHooksBegin(hWnd, WndCompat::Menu::g_MenuContext);

            if (Menu::Policy::OnOwnerMeasureItem(hWnd, mis))
            {
                DebugLog(L"[WndCompat][MenuOwner] WM_MEASUREITEM handled hwnd=0x%p", hWnd);
                if (provider && provider->OnUxHooksEnd)
                    provider->OnUxHooksEnd(hWnd, WndCompat::Menu::g_MenuContext);
                return TRUE;
            }

            LRESULT result = DefSubclassProc(hWnd, uMsg, wParam, lParam);

            if (provider && provider->OnUxHooksEnd)
                provider->OnUxHooksEnd(hWnd, WndCompat::Menu::g_MenuContext);

            return result;
        }

        case WM_DRAWITEM:
        {
            auto dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (!dis || dis->CtlType != ODT_MENU)
                break;

            DebugLog(L"[WndCompat][MenuOwner] WM_DRAWITEM hwnd=0x%p itemID=%u itemData=0x%p",
                hWnd, dis->itemID, (void*)dis->itemData);

            // Устанавливаем WndCompat-side guard (используется самим WndCompat для совместимости)
            Menu::g_drawItemStruct = dis;
            auto restoreDrawItemStruct = wil::scope_exit([&] {
                Menu::g_drawItemStruct = nullptr;
                });

            std::unique_ptr<BitmapRestore> restoreItem, restoreChecked, restoreUnchecked;

            if (Menu::g_MenuContext.type == Menu::MenuType::Immersive)
            {
                auto menuData = GetContextMenuDataForItem(hWnd, dis->itemData, dis->itemID);
                if (menuData)
                {
                    DebugLog(L"[WndCompat][MenuOwner] WM_DRAWITEM: ContextMenuData found hwnd=0x%p", hWnd);

                    auto provider = Menu::GetMenuEventProvider();

                    // Информационный колбек (bitmap-processing не делаем здесь, он ниже)
                    if (provider && provider->OnOwnerDrawItem)
                    {
                        DebugLog(L"[WndCompat][MenuOwner] Calling provider->OnOwnerDrawItem hwnd=0x%p", hWnd);
                        provider->OnOwnerDrawItem(hWnd, Menu::g_MenuContext, dis, menuData);
                    }

                    // ── Обработка иконок (bitmap processing) ─────────────────
                    wil::unique_hbitmap newItem, newChecked, newUnchecked;

                    auto processBitmap = [&](HBITMAP src) -> HBITMAP
                        {
                            if (!src || Shared::BitmapUtils::IsOemBitmap(src))
                                return nullptr;

                            HBITMAP converted = Shared::BitmapUtils::Promise32BPP(src);
                            if (!converted)
                                return nullptr;

                            if (Menu::g_MenuContext.iconBackgroundColorRemoval.enable)
                            {
                                const auto chromaColor = Shared::ColorUtils::ARGBToCOLORREF(
                                    Menu::g_MenuContext.iconBackgroundColorRemoval.colorTreatAsTransparent);
                                const auto chromaThreshold = static_cast<BYTE>(
                                    Menu::g_MenuContext.iconBackgroundColorRemoval.colorTreatAsTransparentThreshold);

                                std::vector<std::shared_ptr<Shared::BitmapUtils::SpriteEffect>> effects;
                                effects.push_back(std::make_shared<Shared::BitmapUtils::SpriteEffect>(
                                    chromaColor,
                                    chromaThreshold));

                                HBITMAP processed = Shared::BitmapUtils::ApplyEffects(converted, effects);
                                DeleteObject(converted);
                                return processed;
                            }

                            return converted;
                        };

                    newItem.reset(processBitmap(menuData->hbmpItem));

                    if (menuData->hbmpChecked == menuData->hbmpUnchecked)
                    {
                        if (!newItem)
                        {
                            newChecked.reset(processBitmap(menuData->hbmpChecked));
                            if (newChecked)
                            {
                                newUnchecked.reset(Shared::BitmapUtils::Promise32BPP(menuData->hbmpUnchecked));

                                if (Menu::g_MenuContext.iconBackgroundColorRemoval.enable)
                                {
                                    const auto chromaColor = Shared::ColorUtils::ARGBToCOLORREF(
                                        Menu::g_MenuContext.iconBackgroundColorRemoval.colorTreatAsTransparent);
                                    const auto chromaThreshold = static_cast<BYTE>(
                                        Menu::g_MenuContext.iconBackgroundColorRemoval.colorTreatAsTransparentThreshold);

                                    std::vector<std::shared_ptr<Shared::BitmapUtils::SpriteEffect>> effects;
                                    effects.push_back(std::make_shared<Shared::BitmapUtils::SpriteEffect>(
                                        chromaColor,
                                        chromaThreshold));

                                    HBITMAP processed = Shared::BitmapUtils::ApplyEffects(newUnchecked.get(), effects);
                                    newUnchecked.reset(processed);
                                }
                            }
                        }
                    }
                    else
                    {
                        newChecked.reset(processBitmap(menuData->hbmpChecked));
                        newUnchecked.reset(processBitmap(menuData->hbmpUnchecked));
                    }

                    if (newItem)
                    {
                        restoreItem = std::make_unique<BitmapRestore>(&menuData->hbmpItem, menuData->hbmpItem);
                        menuData->hbmpItem = newItem.get();
                    }
                    if (newChecked)
                    {
                        restoreChecked = std::make_unique<BitmapRestore>(&menuData->hbmpChecked, menuData->hbmpChecked);
                        menuData->hbmpChecked = newChecked.get();
                    }
                    if (newUnchecked && newUnchecked.get() != newChecked.get())
                    {
                        restoreUnchecked = std::make_unique<BitmapRestore>(&menuData->hbmpUnchecked, menuData->hbmpUnchecked);
                        menuData->hbmpUnchecked = newUnchecked.get();
                    }

                    // ── КЛЮЧЕВОЕ: Begin/End вокруг DefSubclassProc ────────────
                    // Активирует GlassCore-guard, чтобы ImmersiveHooks перехватывали
                    // DrawTextW и DrawThemeBackground внутри owner-draw WndProc.
                    // Begin/End живут в GlassCore.dll → thread_local виден им обоим,
                    // в отличие от g_drawItemStruct, который не пересекает DLL-границу.
                    if (provider && provider->OnOwnerDrawBegin)
                        provider->OnOwnerDrawBegin(hWnd, Menu::g_MenuContext, dis);

                    LRESULT result = DefSubclassProc(hWnd, uMsg, wParam, lParam);

                    if (provider && provider->OnOwnerDrawEnd)
                        provider->OnOwnerDrawEnd(hWnd, Menu::g_MenuContext);

                    // restore-объекты восстанавливают bitmap-указатели автоматически (RAII)
                    return result;
                }
            }

            // TraverseLog: нет ContextMenuRenderingData — explorerframe рисует сам,
            // но нам нужно активировать guard для TraverseLogHooks (DrawTextW, DrawThemeBackground).
            // BWM-паттерн: просто выставляем guard и зовём оригинальный WndProc.
            // Bitmap-processing здесь не нужен — TraverseLog-меню иконок не имеют.
            if (Menu::g_MenuContext.type == Menu::MenuType::TraverseLog)
            {
                auto provider = Menu::GetMenuEventProvider();

                if (provider && provider->OnOwnerDrawBegin)
                    provider->OnOwnerDrawBegin(hWnd, Menu::g_MenuContext, dis);

                LRESULT result = DefSubclassProc(hWnd, uMsg, wParam, lParam);

                if (provider && provider->OnOwnerDrawEnd)
                    provider->OnOwnerDrawEnd(hWnd, Menu::g_MenuContext);

                return result;
            }

            // Не Immersive или нет menuData — Policy и системный fallback
            if (Menu::Policy::OnOwnerDrawItem(hWnd, dis))
            {
                DebugLog(L"[WndCompat][MenuOwner] WM_DRAWITEM handled by Policy hwnd=0x%p", hWnd);
                return TRUE;
            }

            break;
        }

        }

        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }

    bool MenuOwner_HandleClassicMenuBarNcActivate(
        HWND hWnd, WPARAM wParam, LPARAM lParam, LRESULT& outResult) noexcept
    {
        if (!IsClassicMenuBarExtTextOutTarget(hWnd))
            return false;

        auto* p = Menu::GetMenuEventProvider();
        if (!p || !p->OnMenuBarClassicNcActivateBracketBegin || !p->OnMenuBarClassicNcActivateBracketEnd)
            return false;

        p->OnMenuBarClassicNcActivateBracketBegin(hWnd);
        outResult = DefSubclassProc(hWnd, WM_NCACTIVATE, wParam, lParam);
        p->OnMenuBarClassicNcActivateBracketEnd(hWnd);
        return true;
    }

    bool MenuOwner_HandleClassicMenuBarNcPaint(
        HWND hWnd, WPARAM wParam, LPARAM lParam, LRESULT& outResult) noexcept
    {
        if (!IsClassicMenuBarExtTextOutTarget(hWnd))
            return false;

        auto* p = Menu::GetMenuEventProvider();
        if (!p || !p->OnMenuBarClassicNcPaintBracketBegin || !p->OnMenuBarClassicNcPaintBracketEnd)
            return false;

        p->OnMenuBarClassicNcPaintBracketBegin(hWnd);
        outResult = DefSubclassProc(hWnd, WM_NCPAINT, wParam, lParam);
        p->OnMenuBarClassicNcPaintBracketEnd(hWnd);
        return true;
    }

    bool MenuOwner_HandleClassicMenuBarNcHitTest(
        HWND hWnd, WPARAM wParam, LPARAM lParam, LRESULT& outResult) noexcept
    {
        outResult = DefSubclassProc(hWnd, WM_NCHITTEST, wParam, lParam);
        if (!IsClassicMenuBarExtTextOutTarget(hWnd))
            return true;
        if (outResult != HTMENU)
            return true;

        auto* p = Menu::GetMenuEventProvider();
        if (p && p->OnMenuBarClassicEtwHoldEnsure)
            p->OnMenuBarClassicEtwHoldEnsure(hWnd);
        return true;
    }

    void MenuOwner_OnClassicMenuBarEnterIdle(HWND hWnd, WPARAM wParam, LPARAM lParam) noexcept
    {
        if (wParam != MSGF_MENU)
            return;
        if (!IsClassicMenuBarExtTextOutTarget(hWnd))
            return;

        auto* p = Menu::GetMenuEventProvider();
        if (p && p->OnMenuBarClassicEtwHoldEnsure)
            p->OnMenuBarClassicEtwHoldEnsure(hWnd);
    }

    void MenuOwner_OnClassicMenuBarNcMouseLeave(HWND hWnd) noexcept
    {
        auto* p = Menu::GetMenuEventProvider();
        if (p && p->OnMenuBarClassicEtwHoldRelease)
            p->OnMenuBarClassicEtwHoldRelease(hWnd);
    }

    void MenuOwner_ReleaseClassicMenuBarHold(HWND hWnd) noexcept
    {
        MenuOwner_OnClassicMenuBarNcMouseLeave(hWnd);
    }

}