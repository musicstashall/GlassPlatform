// GlassCore/src/Subsystems/MenuEvents.cpp
#include "CorePch.h"
#include <Subsystems/MenuEvents.h>

#include <Subsystems/MenuHandler.h>
#include <Hooks/UxThemeHooks.h>
#include <Hooks/ImmersiveHooks.h>
#include <Hooks/TraverseLogHooks.h>

#include <Subsystems/EffectHelper.h>
#include <Subsystems/Animation.h>
#include <Subsystems/FlyoutFrame.h>
#include <Subsystems/Settings.h>
#include <Core/Utils.h>

#include <MenuUtils.h>
#include <WinApiUtils.h>

#include <mutex>
#include <unordered_set>
#include <vector>

using WndCompat::Menu::MenuContext;
using WndCompat::Menu::MenuType;

namespace GlassCore::Menu
{
    namespace
    {
        std::mutex g_classicMenuBarHoldMutex;
        std::unordered_set<HWND> g_classicMenuBarHoldHwnds;
        [[nodiscard]] bool ClassicMenuBarNcBracketApplies(HWND hWnd) noexcept
        {
            return hWnd && IsMenu(GetMenu(hWnd))
                && !Shared::WinApiUtils::IsThemed(hWnd, L"Menu");
        }

        void ClassicMenuBarExtTextOutBracketBegin(HWND hWnd)
        {
            if (!ClassicMenuBarNcBracketApplies(hWnd))
                return;
            Hooks::UxThemeHooks::SetCurrentMenuHwnd(hWnd);
            Hooks::UxThemeHooks::EnableExtTextOutHook(true);
        }

        // После −1 к g_etwRef: если HTMENU/MSGF_MENU hold ещё держит это окно — вернуть HWND сессии
        // (иначе ExtTextOut с DC памяти при неактивном NC не видит владельца через WindowFromDC).
        void ClassicMenuBarExtTextOutBracketEndRestoreHoldIfNeeded(HWND hWnd)
        {
            if (!ClassicMenuBarNcBracketApplies(hWnd))
                return;
            Hooks::UxThemeHooks::EnableExtTextOutHook(false);
            Hooks::UxThemeHooks::SetCurrentMenuHwnd(nullptr);
            std::lock_guard lock(g_classicMenuBarHoldMutex);
            if (hWnd && g_classicMenuBarHoldHwnds.find(hWnd) != g_classicMenuBarHoldHwnds.end())
                Hooks::UxThemeHooks::SetCurrentMenuHwnd(hWnd);
        }

        std::mutex g_nativeThemeBorderSetupMutex;
        std::unordered_set<HWND> g_nativeThemeBorderSetupDone;
        thread_local bool g_inNativeThemeNcPaint{ false };

        [[nodiscard]] WndCompat::Menu::BorderContext ToMenuBorder(
            const Settings::BorderContext& border) noexcept
        {
            WndCompat::Menu::BorderContext out{};
            out.colorUseNone = border.colorUseNone;
            out.colorUseDefault = border.colorUseDefault;
            out.color = border.color;
            out.cornerType = border.cornerType;
            return out;
        }

        void ClearNativeThemeBorderSetup(HWND hWnd) noexcept
        {
            if (!hWnd)
                return;
            std::lock_guard lock(g_nativeThemeBorderSetupMutex);
            g_nativeThemeBorderSetupDone.erase(hWnd);
        }
    }

    // -------------------------------------------------------------------------
    // Guard для ImmersiveHooks: живёт только внутри GlassCore.dll.
    // WndCompat не может разделять thread_local через DLL-границу.
    // WndCompat вызывает OnOwnerDrawBegin/End вокруг DefSubclassProc(WM_DRAWITEM),
    // ImmersiveHooks проверяет IsInsideOwnerDraw() вместо g_drawItemStruct.
    // -------------------------------------------------------------------------
    thread_local bool g_insideOwnerDraw{ false };

    bool IsInsideOwnerDraw() noexcept
    {
        return g_insideOwnerDraw;
    }

    // -------------------------------------------------------------------------

    void OnPopupCreated(HWND hWnd, const MenuContext& ctx)
    {
        UNREFERENCED_PARAMETER(hWnd);
        MenuHandler::SetContext(ctx);
    }

    void OnPopupShown(HWND hWnd, const MenuContext& ctx)
    {
        MenuHandler::SetContext(ctx);
        EffectHelper::TriggerWindowNCRendering(hWnd);
    }

    void OnPopupSized(HWND hWnd, const MenuContext& ctx)
    {
        // Не затираем актуальные type/useDarkMode из MenuHandler (DTB/WM_UAHINITMENU)
        // устаревшим снимком ctx из WndCompat.
        MenuContext merged = ctx;
        const auto& h = MenuHandler::GetContext();
        merged.type = h.type;
        merged.useDarkMode = h.useDarkMode;
        merged.useUxTheme = h.useUxTheme;

        Settings::WindowBackdropEffectContext backdrop{};
        Settings::BorderContext settingsBorder{};
        Settings::QueryBackdropEffectContext(L"Menu", merged.useDarkMode, backdrop);
        Settings::QueryBorderContext(L"Menu", merged.useDarkMode, settingsBorder);
        Settings::QueryMenuCustomRenderingContext(merged.useDarkMode, merged.customRendering);
        Settings::QueryMenuIconBackgroundColorRemovalContext(merged.iconBackgroundColorRemoval);

        DebugLog(DBG_IMPORTANT, L"[GlassCore][MenuEvents] OnPopupSized: customRendering.enable=%d separator_disabled=%d separator_width=%u",
            merged.customRendering.enable, merged.customRendering.separator_disabled, merged.customRendering.separator_width);

        // SetContext вызываем ПОСЛЕ заполнения всех полей merged (включая customRendering)
        MenuHandler::SetContext(merged);
        MenuHandler::SetBorderContext(settingsBorder);

        // Паритет TranslucentFlyouts MenuHandler: ApplyEffect на real до cloak; NC refresh при ET 6–8 на show.
        GlassCore::Settings::FlyoutAnimationContext animation{};
        GlassCore::Settings::QueryFlyoutAnimationContext(L"Menu", animation);

        Settings::ApplyPopupSwcaEffect(
            hWnd, merged.useDarkMode, backdrop, settingsBorder, false);

        if (animation.enable)
        {
            SetFlyoutBorderDeferred(hWnd, true);
        }
        else
        {
            OnPaintFlyoutHostBorder(
                hWnd,
                WndCompat::Menu::FlyoutBorderPass::SetupAndPaint,
                nullptr);
        }

        if (animation.enable)
        {
            // MN_SIZEWINDOW приходит ДО SetWindowPos(SWP_SHOWWINDOW).
            // PopupIn прикрепляет subclass и cloak в ctor; WM_WINDOWPOSCHANGED+SWP_SHOWWINDOW → Start() (как TF).
            (void)GlassCore::Animation::CreateMenuPopupIn(
                hWnd,
                static_cast<float>(animation.startRatio) / 100.f,
                std::chrono::milliseconds{ animation.popInTime },
                std::chrono::milliseconds{ animation.fadeInTime },
                animation.popInStyle
            );
        }
    }

    void EnsureNativeThemeFlyoutBorder(HWND hWnd, bool forceRefresh) noexcept
    {
        if (!hWnd || !IsWindow(hWnd))
            return;

        bool alreadyDone = false;
        {
            std::lock_guard lock(g_nativeThemeBorderSetupMutex);
            alreadyDone = g_nativeThemeBorderSetupDone.find(hWnd) != g_nativeThemeBorderSetupDone.end();
            if (alreadyDone && !forceRefresh)
                return;
        }

        const bool dark = MenuHandler::GetContext().useDarkMode;
        Settings::BorderContext border{};
        Settings::QueryBorderContext(L"Menu", dark, border);
        const auto menuBorder = ToMenuBorder(border);

        Shared::MenuUtils::ApplyMenuDwmBorderColorFromTheme(hWnd, dark, menuBorder);

        if (!alreadyDone)
        {
            std::lock_guard lock(g_nativeThemeBorderSetupMutex);
            g_nativeThemeBorderSetupDone.insert(hWnd);
        }

        DebugLog(DBG_IMPORTANT,
            forceRefresh
                ? L"[GlassCore][MenuEvents] EnsureNativeThemeFlyoutBorder: refresh DWM hwnd=0x%p corner=%d"
                : L"[GlassCore][MenuEvents] EnsureNativeThemeFlyoutBorder: hwnd=0x%p corner=%d",
            hWnd,
            static_cast<int>(border.cornerType));
    }

    void SetFlyoutBorderDeferred(HWND hWnd, bool deferred) noexcept
    {
        FlyoutFrame::SetHostBorderDeferred(hWnd, deferred);
    }

    bool IsFlyoutBorderDeferred(HWND hWnd) noexcept
    {
        return FlyoutFrame::IsHostBorderDeferred(hWnd);
    }

    void ClearFlyoutBorderDeferred(HWND hWnd) noexcept
    {
        FlyoutFrame::ClearHostBorderDeferred(hWnd);
    }

    void PaintDeferredFlyoutBorderIfNeeded(HWND hWnd) noexcept
    {
        if (!hWnd || !IsWindow(hWnd) || !FlyoutFrame::IsHostBorderDeferred(hWnd))
            return;

        FlyoutFrame::ClearHostBorderDeferred(hWnd);

        OnPaintFlyoutHostBorder(
            hWnd,
            WndCompat::Menu::FlyoutBorderPass::SetupAndPaint,
            nullptr);

        DebugLog(DBG_IMPORTANT,
            L"[GlassCore][MenuEvents] PaintDeferredFlyoutBorderIfNeeded hwnd=0x%p",
            hWnd);
    }

    void OnPaintFlyoutHostBorder(
        HWND hWnd,
        WndCompat::Menu::FlyoutBorderPass pass,
        HRGN clipRgn) noexcept
    {
        if (!hWnd || !IsWindow(hWnd))
            return;

        if (IsFlyoutBorderDeferred(hWnd))
            return;

        const bool dark = MenuHandler::GetContext().useDarkMode;
        Settings::BorderContext border{};
        Settings::WindowBackdropEffectContext backdrop{};
        Settings::QueryBorderContext(L"Menu", dark, border);
        Settings::QueryBackdropEffectContext(L"Menu", dark, backdrop);

        if (MenuHandler::GetContext().type == MenuType::NativeTheme)
        {
            const auto menuBorder = ToMenuBorder(border);

            if (pass == WndCompat::Menu::FlyoutBorderPass::SetupAndPaint ||
                pass == WndCompat::Menu::FlyoutBorderPass::PostUncloakSetupAndPaint)
            {
                const bool forceDwm =
                    pass == WndCompat::Menu::FlyoutBorderPass::PostUncloakSetupAndPaint;
                EnsureNativeThemeFlyoutBorder(hWnd, forceDwm);
                (void)Shared::MenuUtils::PaintNativeThemePopupNcBorder(hWnd, menuBorder, nullptr);

                if (forceDwm)
                {
                    (void)RedrawWindow(
                        hWnd,
                        nullptr,
                        nullptr,
                        RDW_FRAME | RDW_NOCHILDREN);
                }

                DebugLog(DBG_IMPORTANT,
                    forceDwm
                        ? L"[GlassCore][MenuEvents] OnPaintFlyoutHostBorder: NativeTheme post-uncloak hwnd=0x%p"
                        : L"[GlassCore][MenuEvents] OnPaintFlyoutHostBorder: NativeTheme setup+paint hwnd=0x%p",
                    hWnd);
                return;
            }

            if (g_inNativeThemeNcPaint)
                return;

            g_inNativeThemeNcPaint = true;
            auto resetGuard = wil::scope_exit([] { g_inNativeThemeNcPaint = false; });

            (void)Shared::MenuUtils::PaintNativeThemePopupNcBorder(hWnd, menuBorder, clipRgn);
            return;
        }

        if (pass == WndCompat::Menu::FlyoutBorderPass::NcPaintOnly)
            return;

        FlyoutFrame::PaintHostBorder(
            hWnd,
            dark,
            border,
            backdrop.effectType,
            FlyoutFrame::FlyoutHostBorderFlush::FrameOnly);
    }

    void OnPopupDestroyed(HWND hWnd, const WndCompat::Menu::MenuContext& ctx)
    {
        // При уничтожении меню backdrop не отключается — окно само уничтожается.
        // DropEffect здесь не нужен (вызывал паразитический SetWindowBackdrop eff=0).
        UNREFERENCED_PARAMETER(ctx);
        ClearNativeThemeBorderSetup(hWnd);
        ClearFlyoutBorderDeferred(hWnd);
    }

    // -----------------------------------------------------------------------
    // Owner-draw / input
    // -----------------------------------------------------------------------

    void OnOwnerDrawItem(
        HWND hWnd,
        const MenuContext& ctx,
        const DRAWITEMSTRUCT* dis,
        const WndCompat::Menu::ContextMenuRenderingData* data)
    {
        UNREFERENCED_PARAMETER(hWnd);
        UNREFERENCED_PARAMETER(ctx);
        UNREFERENCED_PARAMETER(dis);
        UNREFERENCED_PARAMETER(data);
        // Bitmap-processing выполняется в WndCompat::MenuOwnerSubclass.
        // Вся остальная отрисовка (текст, фон, подсветка) происходит позже,
        // внутри DefSubclassProc, перехватывается ImmersiveHooks через guard.
    }

    // ── Immersive draw guard ─────────────────────────────────────────────────

    void OnOwnerDrawBegin(HWND hWnd, const MenuContext& ctx, const DRAWITEMSTRUCT* dis)
    {
        UNREFERENCED_PARAMETER(hWnd);
        UNREFERENCED_PARAMETER(dis);
        g_insideOwnerDraw = true;
        if (ctx.type == MenuType::Immersive || ctx.type == MenuType::TraverseLog)
            MenuHandler::SetUxHooksEnabled(true);
    }

    void OnOwnerDrawEnd(HWND hWnd, const MenuContext& ctx)
    {
        UNREFERENCED_PARAMETER(hWnd);
        if (ctx.type == MenuType::Immersive || ctx.type == MenuType::TraverseLog)
            MenuHandler::SetUxHooksEnabled(false);
        g_insideOwnerDraw = false;
    }

    void OnButtonUp(HWND hWnd, const MenuContext& ctx, const MENUBARINFO& mbi)
    {
        UNREFERENCED_PARAMETER(ctx);
        (void)GlassCore::Animation::CreateMenuFadeOut(
            hWnd, mbi, GlassCore::Animation::StandardFadeoutDuration);
    }

    // -----------------------------------------------------------------------
    // Управление хуками
    // -----------------------------------------------------------------------

    static PVOID GetImmersiveTargetModule()
    {
        HMODULE frame = GetModuleHandleW(L"explorerframe.dll");
        if (frame) return frame;
        return GetModuleHandleW(L"shell32.dll");
    }

    void OnUxHooksBegin(HWND /*hWnd*/, const MenuContext& /*ctx*/)
    {
        MenuHandler::SetUxHooksEnabled(true);
    }

    void OnUxHooksEnd(HWND /*hWnd*/, const MenuContext& /*ctx*/)
    {
        MenuHandler::SetUxHooksEnabled(false);
    }

    void OnMenuBarUxThemeDrawBegin(HWND hWnd, const MenuContext& /*ctx*/)
    {
        Hooks::UxThemeHooks::BeginMenuBarUxThemeDraw(hWnd);
    }

    void OnMenuBarUxThemeDrawEnd(HWND hWnd, const MenuContext& /*ctx*/)
    {
        Hooks::UxThemeHooks::EndMenuBarUxThemeDraw(hWnd);
    }

    void OnMenuBarClassicNcActivateBracketBegin(HWND hWnd)
    {
        ClassicMenuBarExtTextOutBracketBegin(hWnd);
    }

    void OnMenuBarClassicNcActivateBracketEnd(HWND hWnd)
    {
        ClassicMenuBarExtTextOutBracketEndRestoreHoldIfNeeded(hWnd);
    }

    void OnMenuBarClassicUahInitMenuBracketBegin(HWND hWnd)
    {
        ClassicMenuBarExtTextOutBracketBegin(hWnd);
    }

    void OnMenuBarClassicUahInitMenuBracketEnd(HWND hWnd)
    {
        ClassicMenuBarExtTextOutBracketEndRestoreHoldIfNeeded(hWnd);
    }

    void OnMenuBarClassicNcPaintBracketBegin(HWND hWnd)
    {
        if (!ClassicMenuBarNcBracketApplies(hWnd))
            return;
        Hooks::UxThemeHooks::EnableExtTextOutHook(true);
    }

    void OnMenuBarClassicNcPaintBracketEnd(HWND hWnd)
    {
        if (!ClassicMenuBarNcBracketApplies(hWnd))
            return;
        Hooks::UxThemeHooks::EnableExtTextOutHook(false);
    }

    void OnMenuBarClassicEtwHoldEnsure(HWND hWnd)
    {
        if (!hWnd || Shared::WinApiUtils::IsThemed(hWnd, L"Menu"))
            return;
        std::lock_guard lock(g_classicMenuBarHoldMutex);
        const auto inserted = g_classicMenuBarHoldHwnds.insert(hWnd).second;
        if (inserted)
            Hooks::UxThemeHooks::EnableExtTextOutHook(true);
    }

    void OnMenuBarClassicEtwHoldRelease(HWND hWnd)
    {
        if (!hWnd)
            return;
        std::lock_guard lock(g_classicMenuBarHoldMutex);
        const auto erased = g_classicMenuBarHoldHwnds.erase(hWnd);
        if (erased)
            Hooks::UxThemeHooks::EnableExtTextOutHook(false);
    }

    void OnClassicMenuPopupPaintExtTextOutBegin(HWND hWnd)
    {
        if (!hWnd)
            return;
        Hooks::UxThemeHooks::SetCurrentMenuHwnd(hWnd);
        Hooks::UxThemeHooks::EnableExtTextOutHook(true);
    }

    void OnClassicMenuPopupPaintExtTextOutEnd(HWND /*hWnd*/)
    {
        Hooks::UxThemeHooks::EnableExtTextOutHook(false);
        Hooks::UxThemeHooks::SetCurrentMenuHwnd(nullptr);
    }

    void ShutdownClassicMenuBarNcExtTextOut() noexcept
    {
        std::vector<HWND> toRelease;
        {
            std::lock_guard lock(g_classicMenuBarHoldMutex);
            toRelease.assign(g_classicMenuBarHoldHwnds.begin(), g_classicMenuBarHoldHwnds.end());
            g_classicMenuBarHoldHwnds.clear();
        }
        for (size_t i = 0; i < toRelease.size(); ++i)
            Hooks::UxThemeHooks::EnableExtTextOutHook(false);
    }

    void SyncMenuContextFromMenuHandler(WndCompat::Menu::MenuContext* ctx) noexcept
    {
        if (!ctx)
            return;
        const auto& h = MenuHandler::GetContext();
        ctx->useDarkMode = h.useDarkMode;
        ctx->useUxTheme = h.useUxTheme;
        ctx->border = h.border;
    }

    void FinalizeNativeThemeAfterPrintWindowProbe(WndCompat::Menu::MenuContext* ctx) noexcept
    {
        if (!ctx)
            return;
        if (ctx->type != MenuType::Classic)
            return;
        const auto& mh = MenuHandler::GetContext();
        if (!mh.useUxTheme)
            return;
        DebugLog(DBG_WARNING,
            L"[GlassCore][Menu] PrintWindow: ранний WM_UAHINITMENU дал Classic, "
            L"DTB подтвердил тему — NativeTheme");
        ctx->type = MenuType::NativeTheme;
        ctx->useUxTheme = true;
        ctx->useDarkMode = mh.useDarkMode;
        MenuHandler::SetContext(*ctx);
        SyncMenuContextFromMenuHandler(ctx);
    }

    void ApplyUaInitMenuNativeOrClassic(LRESULT defSubclassResult, WndCompat::Menu::MenuContext* ctx) noexcept
    {
        if (!ctx)
            return;

        using WndCompat::Menu::MenuType;

        // DTB-хуки ставят useUxTheme в MenuHandler (GlassCore-сторона), а не в ctx (WndCompat g_MenuContext).
        // При PrintWindow-пробе ctx->useUxTheme сброшен в false — читаем актуальное значение из MenuHandler.
        const bool uxThemeDetected = MenuHandler::GetContext().useUxTheme;

        if (defSubclassResult && uxThemeDetected)
        {
            ctx->type = MenuType::NativeTheme;
            ctx->useUxTheme = true;
        }
        else
        {
            ctx->type = MenuType::Classic;
            ctx->useDarkMode = false;
            ctx->useUxTheme = false;
            MenuHandler::SetDarkMode(false);
        }

        auto merged = MenuHandler::GetContext();
        merged.type = ctx->type;
        merged.useDarkMode = ctx->useDarkMode;
        merged.useUxTheme = ctx->useUxTheme;
        MenuHandler::SetContext(merged);
    }

    void GlassCore::Menu::OnAttachHooks(HWND hWnd, const WndCompat::Menu::MenuContext& ctx)
    {
        using WndCompat::Menu::MenuType;

        MenuHandler::SetContext(ctx);

        switch (ctx.type)
        {
        case MenuType::NativeTheme:
        case MenuType::ClassicOrNativeTheme:
            Hooks::UxThemeHooks::EnableThemeHooks(true);
            break;

        case MenuType::Immersive:
            Hooks::Immersive::EnableHooks(GetImmersiveTargetModule(), true);
            Hooks::UxThemeHooks::EnableThemeHooks(true);
            break;

        case MenuType::TraverseLog:
            Hooks::TraverseLog::EnableHooks(true);
            Hooks::UxThemeHooks::EnableThemeHooks(true);
            break;

        default:
            break;
        }
    }

    void GlassCore::Menu::OnDetachHooks(HWND /*hWnd*/, const WndCompat::Menu::MenuContext& ctx)
    {
        using WndCompat::Menu::MenuType;

        // Не сбрасываем SetUxHooksEnabled глобально: закрытие #32768 не должно ломать
        // следующий DTB/UAH меню-бара владельца (см. BWM: флаг только вокруг DefSubclass).

        // Сбрасываем guard на случай досрочного детача в середине draw
        g_insideOwnerDraw = false;

        switch (ctx.type)
        {
        case MenuType::NativeTheme:
        case MenuType::ClassicOrNativeTheme:
        case MenuType::Classic:
            // MN_SIZEWINDOW: OnAttach(ClassicOrNativeTheme) включает theme hooks; после PrintWindow
            // тип может стать Classic — всё равно нужна симметричная отписка (без трогания ETW бара).
            Hooks::UxThemeHooks::EnableThemeHooks(false);
            break;

        case MenuType::Immersive:
            Hooks::UxThemeHooks::EnableThemeHooks(false);
            Hooks::Immersive::EnableHooks(GetImmersiveTargetModule(), false);
            break;

        case MenuType::TraverseLog:
            Hooks::UxThemeHooks::EnableThemeHooks(false);
            Hooks::TraverseLog::EnableHooks(false);
            break;

        default:
            break;
        }
    }
}