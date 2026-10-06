#include "CorePch.h"

#include <Hooks/DropDown.h>
#include <Hooks/ComboLBox.h>
#include <Hooks/ScrollBarDrawTheme.h>
#include <Hooks/ScrollBarTheme.h>
#include <Module/ModuleHandler.h>
#include <Subsystems/RenderText.h>
#include <Subsystems/MenuRendering.h>
#include <Helpers/ThemeHelper.h>
#include <Core/Intercept/HookDispatcher.h>
#include <wil.h>
#include <uxtheme.h>
#include <Subsystems/Settings.h>
#include <Subsystems/Animation.h>
#include <Subsystems/FlyoutFrame.h>
#include <DropDownContext.h>
#include <Core/Utils.h>
#include <MenuUtils.h>

#include <vector>

using namespace GlassCore::Hooks;
using namespace std::literals;

namespace GlassCore::DropDown
{
    // =========================================================================
    // Thread-local guards
    // =========================================================================

    thread_local bool             g_insidePaint = false;
    thread_local bool             g_insideDrawItem = false;
    thread_local const DRAWITEMSTRUCT* g_drawItemStruct = nullptr;

    bool IsInsidePaint()    noexcept { return g_insidePaint; }
    bool IsInsideDrawItem() noexcept { return g_insideDrawItem; }
    const DRAWITEMSTRUCT* GetDrawItemStruct() noexcept { return g_drawItemStruct; }

    // =========================================================================
    // Hook prototypes
    // =========================================================================

    int WINAPI MyDrawTextW(
        HDC hdc, LPCWSTR lpchText, int cchText, LPRECT lprc, UINT format);

    HRESULT WINAPI MyDrawThemeBackground(
        HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
        LPCRECT pRect, LPCRECT pClipRect);

    HRESULT WINAPI MyDrawThemeText(
        HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
        LPCWSTR pszText, int cchText, DWORD dwTextFlags,
        DWORD dwTextFlags2, LPCRECT pRect);
    // =========================================================================
    // Hook dependency table
    // =========================================================================

    enum HookIndex : size_t
    {
        HK_DRAWTEXT = 0,
        HK_DRAWTHEMEBACKGROUND,
        HK_DRAWTHEMETEXT
    };

    inline const HookDispatcherDependency<3, 3> g_hookDependency
    { {
            // 0: DrawTextW
            std::tuple
            {
                std::array<std::string_view, 3>
                {
                    "user32.dll"sv,
                    "ext-ms-win-ntuser-draw-l1-1-0.dll"sv,
                    "ext-ms-win-ntuser-misc-l1-1-0.dll"sv
                },
                "DrawTextW",
                reinterpret_cast<PVOID>(MyDrawTextW)
            },
        // 1: DrawThemeBackground
        std::tuple
        {
            std::array<std::string_view, 3>
            {
                "uxtheme.dll"sv,
                "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv,
                ""sv
            },
            "DrawThemeBackground",
            reinterpret_cast<PVOID>(MyDrawThemeBackground)
        },
        std::tuple
        {
            std::array<std::string_view, 3>
            {
                "uxtheme.dll"sv,
                "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv,
                ""sv
            },
            "DrawThemeText",
            reinterpret_cast<PVOID>(MyDrawThemeText)
        }
    } };

    static HookDispatcher<3, 3> g_dispatcher{ g_hookDependency, nullptr };
    static wil::srwlock          g_lock;
    static size_t                g_enableHooksRef = 0;

    // =========================================================================
    // Hook implementations
    // =========================================================================

    int WINAPI MyDrawTextW(
        HDC hdc, LPCWSTR lpchText, int cchText, LPRECT lprc, UINT format)
    {
        auto orig = g_dispatcher.GetOrg<HK_DRAWTEXT, decltype(&DrawTextW)>();
        if (!orig)
            return DrawTextW(hdc, lpchText, cchText, lprc, format);

        // DrawTextW перехватываем только внутри WM_DRAWITEM
        if (!IsInsideDrawItem())
            return orig(hdc, lpchText, cchText, lprc, format);

        // Исключаем расчётные и специальные вызовы
        if ((format & DT_CALCRECT) || (format & DT_INTERNAL) || (format & DT_NOCLIP))
            return orig(hdc, lpchText, cchText, lprc, format);

        int result = 0;
        if (FAILED(GlassCore::Text::DrawTextWithAlpha(hdc, lpchText, cchText, lprc, format, result)))
            result = orig(hdc, lpchText, cchText, lprc, format);

        return result;
    }

    HRESULT WINAPI MyDrawThemeBackground(
        HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
        LPCRECT pRect, LPCRECT pClipRect)
    {
        auto orig = g_dispatcher.GetOrg<HK_DRAWTHEMEBACKGROUND, decltype(&DrawThemeBackground)>();
        if (!orig)
            return DrawThemeBackground(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

        if (!IsInsidePaint() && !IsInsideDrawItem())
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

        // Внутри WM_DRAWITEM: тему "ListviewPopup" закрашиваем чёрным
        if (IsInsideDrawItem())
        {
            WCHAR themeClass[MAX_PATH + 1]{};
            if (SUCCEEDED(Shared::ThemeHelper::GetThemeClass(hTheme, themeClass, MAX_PATH))
                && !_wcsicmp(themeClass, L"ListviewPopup"))
            {
                RECT clipRect = *pRect;
                if (pClipRect)
                    IntersectRect(&clipRect, &clipRect, pClipRect);

                PatBlt(hdc,
                    clipRect.left, clipRect.top,
                    clipRect.right - clipRect.left,
                    clipRect.bottom - clipRect.top,
                    BLACKNESS);

                return S_OK;
            }
        }

        // Тема ScrollBar из explorerframe: тот же sbOrig на comctl32, что и в Overall (вариант B плана).
        {
            WCHAR themeClassSb[MAX_PATH + 1]{};
            if (SUCCEEDED(Shared::ThemeHelper::GetThemeClass(hTheme, themeClassSb, MAX_PATH))
                && !_wcsicmp(themeClassSb, L"ScrollBar"))
            {
                // ScrollBar-orig ищем на обоих экземплярах comctl32 (v6 + legacy):
                // GetModuleHandleW возвращает один, неизвестно какой, а полоса
                // дропдауна может рисоваться через legacy. Предпочтение — v6.
                HMODULE comctlMod = GlassCore::ModuleHandler::g_comctl32New
                    ? GlassCore::ModuleHandler::g_comctl32New
                    : GlassCore::ModuleHandler::g_comctl32Legacy;
                auto sbOrig = comctlMod
                    ? GlassCore::ScrollBarTheme::GetComctlScrollBarDrawThemeOrig(comctlMod)
                    : nullptr;
                if (!sbOrig)
                {
                    HMODULE legacyMod = GlassCore::ModuleHandler::g_comctl32Legacy;
                    if (legacyMod && legacyMod != comctlMod)
                        sbOrig = GlassCore::ScrollBarTheme::GetComctlScrollBarDrawThemeOrig(legacyMod);
                }
                auto dtbOrig = sbOrig ? sbOrig : orig;
                if (!dtbOrig)
                    return DrawThemeBackground(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

                GlassCore::ScrollBarDrawTheme::LogScrollBarDrawThemeCall(
                    L"[GlassCore][DropDown]",
                    iPartId,
                    iStateId);

                RECT r{};
                if (!GlassCore::ScrollBarDrawTheme::TryGetScrollBarDrawRect(pRect, pClipRect, r))
                    return S_OK;

                if (GlassCore::ScrollBarDrawTheme::IsScrollBarTrackBackgroundPart(iPartId))
                {
                    PatBlt(hdc, r.left, r.top, r.right - r.left, r.bottom - r.top, BLACKNESS);
                    return S_OK;
                }

                if (iPartId == GlassCore::ScrollBarDrawTheme::kScrollBarPartSizeBox)
                {
                    PatBlt(hdc, r.left, r.top, r.right - r.left, r.bottom - r.top, BLACKNESS);
                    return S_OK;
                }

                if (GlassCore::ScrollBarDrawTheme::IsScrollBarInteractivePart(iPartId)
                    && GlassCore::ScrollBarDrawTheme::TryDrawScrollBarInteractiveGlass(
                        dtbOrig, hTheme, hdc, iPartId, iStateId, r))
                    return S_OK;

                const GlassCore::ScrollBarTheme::DrawThemeOrigCallDepth depth;
                return dtbOrig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
            }
        }

        // Общая обработка через MenuRendering (custom D2D рендеринг пунктов)
        if (GlassCore::Menu::Rendering::HandleDrawThemeBackground(
            hTheme, hdc, iPartId, iStateId, pRect, pClipRect, orig))
        {
            return S_OK;
        }

        return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
    }

    HRESULT WINAPI MyDrawThemeText(
        HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
        LPCWSTR pszText, int cchText, DWORD dwTextFlags,
        DWORD dwTextFlags2, LPCRECT pRect)
    {
        auto orig = g_dispatcher.GetOrg<HK_DRAWTHEMETEXT, decltype(&DrawThemeText)>();
        if (!orig)
            orig = DrawThemeText;

        if (!IsInsidePaint() && !IsInsideDrawItem())
            return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, dwTextFlags2, pRect);

        if (!pRect || IsRectEmpty(pRect) || !pszText || cchText == 0)
            return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, dwTextFlags2, pRect);

        if (dwTextFlags & DT_CALCRECT)
            return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, dwTextFlags2, pRect);

        DebugLog(L"[GlassCore][DropDown] MyDrawThemeText");

        if (GlassCore::Text::IsInkGlowEffectActive())
        {
            int result = 0;
            RECT rc = *pRect;
            if (!FAILED(GlassCore::Text::DrawTextWithAlpha(
                    hdc, pszText, cchText, &rc, dwTextFlags, result)))
                return S_OK;
        }

        WCHAR themeClassName[MAX_PATH + 1]{};
        if (SUCCEEDED(Shared::ThemeHelper::GetThemeClass(hTheme, themeClassName, MAX_PATH)))
        {
            DTTOPTS options{ sizeof(DTTOPTS), DTT_COMPOSITED };
            return GlassCore::Text::DrawThemeContent(
                hdc, *pRect, nullptr, nullptr, 0,
                [&](HDC memoryDC, HPAINTBUFFER, RGBQUAD*, int)
                {
                    DrawThemeTextEx(hTheme, memoryDC, iPartId, iStateId,
                        pszText, cchText, dwTextFlags,
                        const_cast<LPRECT>(pRect), &options);
                });
        }

        return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, dwTextFlags2, pRect);
    }

    // =========================================================================
    // Public API (provider callbacks + lifecycle)
    // =========================================================================
    static std::atomic<bool> g_shutdown{ false };

    void OnPaintBegin(HWND /*hWnd*/)
    {
        if (g_shutdown.load(std::memory_order_acquire)) return;
        g_insidePaint = true;
        EnableHooks(true);
    }

    void OnPaintEnd(HWND /*hWnd*/)
    {
        g_insidePaint = false;
        EnableHooks(false);
    }

    namespace
    {
        [[nodiscard]] bool IsWindowClassName(HWND hwnd, LPCWSTR className) noexcept
        {
            if (!hwnd || !className)
                return false;
            wchar_t cls[64]{};
            return ::GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls))) > 0
                && _wcsicmp(cls, className) == 0;
        }

        [[nodiscard]] bool IsViewControlClass(HWND hwnd) noexcept
        {
            return IsWindowClassName(hwnd, L"ViewControlClass");
        }

        [[nodiscard]] bool IsComboLBoxClass(HWND hwnd) noexcept
        {
            return IsWindowClassName(hwnd, L"ComboLBox");
        }

        void CollectChildWindowsByClass(HWND root, LPCWSTR className, std::vector<HWND>& out) noexcept
        {
            if (!root || !className)
                return;

            struct EnumData
            {
                LPCWSTR className;
                std::vector<HWND>* out;
            } data{ className, &out };

            ::EnumChildWindows(root,
                [](HWND hwnd, LPARAM lParam) -> BOOL
                {
                    auto* ed = reinterpret_cast<EnumData*>(lParam);
                    wchar_t cls[64]{};
                    if (::GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls))) > 0
                        && _wcsicmp(cls, ed->className) == 0)
                    {
                        ed->out->push_back(hwnd);
                    }
                    return TRUE;
                },
                reinterpret_cast<LPARAM>(&data));
        }

        void RedrawFlyoutContentHwnd(HWND hwnd) noexcept
        {
            if (!hwnd || !::IsWindow(hwnd))
                return;

            OnPaintBegin(hwnd);
            (void)::RedrawWindow(
                hwnd,
                nullptr,
                nullptr,
                RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
            OnPaintEnd(hwnd);
        }

        void SyncComboLBoxClientPaint(HWND root) noexcept
        {
            GlassCore::ComboLBox::OnComboLBoxPrintClientBegin(root);

            HDC hdc = ::GetDC(root);
            if (hdc)
            {
                (void)::SendMessageW(
                    root,
                    WM_PRINTCLIENT,
                    reinterpret_cast<WPARAM>(hdc),
                    PRF_CLIENT | PRF_CHECKVISIBLE | PRF_CHILDREN);
                ::ReleaseDC(root, hdc);
            }

            GlassCore::ComboLBox::OnComboLBoxPrintClientEnd(root);
        }

        void SyncViewControlClientPaint(HWND root) noexcept
        {
            (void)::RedrawWindow(
                root,
                nullptr,
                nullptr,
                RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        }

        void RefreshFlyoutClientPaint(HWND root, const wchar_t* modeLabel) noexcept
        {
            if (!root || !::IsWindow(root))
                return;

            if (IsViewControlClass(root))
            {
                SyncViewControlClientPaint(root);
            }
            else if (IsComboLBoxClass(root))
            {
                SyncComboLBoxClientPaint(root);
            }
            else
            {
                std::vector<HWND> targets;
                CollectChildWindowsByClass(root, L"ListviewPopup", targets);
                if (targets.empty())
                    CollectChildWindowsByClass(root, L"SysListView32", targets);
                if (targets.empty())
                    targets.push_back(root);

                for (HWND const target : targets)
                    RedrawFlyoutContentHwnd(target);
            }

            (void)::DwmFlush();

            DebugLog(DBG_IMPORTANT,
                L"[GlassCore][DropDown] RefreshFlyoutClientAfterUncloak root=0x%p mode=%s",
                root,
                modeLabel);
        }
    } // namespace

    void RefreshFlyoutClientAfterUncloak(HWND root) noexcept
    {
        if (!root || !::IsWindow(root))
            return;

        if (IsViewControlClass(root))
            RefreshFlyoutClientPaint(root, L"ViewControl");
        else if (IsComboLBoxClass(root))
            RefreshFlyoutClientPaint(root, L"ComboLBox");
        else
            RefreshFlyoutClientPaint(root, L"DropDown");
    }

    void OnDrawItemBegin(HWND /*hWnd*/, const DRAWITEMSTRUCT* dis)
    {
        if (g_shutdown.load(std::memory_order_acquire)) return;
        g_insideDrawItem = true;
        g_drawItemStruct = dis;
        EnableHooks(true);
    }

    void OnDrawItemEnd(HWND /*hWnd*/)
    {
        g_insideDrawItem = false;
        g_drawItemStruct = nullptr;
        EnableHooks(false);
    }

    void OnToolbarPaintBegin(HWND /*hWnd*/)
    {
        g_insidePaint = true;
        auto lock = g_lock.lock_exclusive();
        // Явный v6 вместо неоднозначного GetModuleHandleW(L"comctl32.dll").
        // (moduleAddress здесь пока не используется — хуки ниже закомментированы.)
        g_dispatcher.moduleAddress = GlassCore::ModuleHandler::g_comctl32New
            ? GlassCore::ModuleHandler::g_comctl32New
            : GlassCore::ModuleHandler::g_comctl32Legacy;
        /*if (g_dispatcher.moduleAddress)
            g_dispatcher.EnableHook(HK_DRAWTHEMETEXT, true);*/
    }

    void OnToolbarPaintEnd(HWND /*hWnd*/)
    {
        g_insidePaint = false;
        auto lock = g_lock.lock_exclusive();
        g_dispatcher.moduleAddress = GlassCore::ModuleHandler::g_comctl32New
            ? GlassCore::ModuleHandler::g_comctl32New
            : GlassCore::ModuleHandler::g_comctl32Legacy;
        /*if (g_dispatcher.moduleAddress)
            g_dispatcher.EnableHook(HK_DRAWTHEMETEXT, false);*/
    }
    // ── Hook enable/disable ───────────────────────────────────────────────────

    void EnableHooks(bool enable)
    {
        auto lock = g_lock.lock_exclusive();

        g_dispatcher.moduleAddress = GetModuleHandleW(L"explorerframe.dll");
        if (!g_dispatcher.moduleAddress)
            return;

        if (enable)
            ++g_enableHooksRef;
        else if (g_enableHooksRef > 0)
            --g_enableHooksRef;

        g_dispatcher.EnableHook(HK_DRAWTEXT, enable);
        g_dispatcher.EnableHook(HK_DRAWTHEMEBACKGROUND, enable);
        g_dispatcher.EnableHook(HK_DRAWTHEMETEXT, enable);
    }

    void DisableHooks()
    {
        auto lock = g_lock.lock_exclusive();

        g_dispatcher.DisableAllHooks();
        g_dispatcher.moduleAddress = nullptr;
    }

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    void Prepare() {}

    void Startup()
    {
        // ComboLBox/ComboBox хуки перенесены в GlassCore::ComboLBox
    }

    void Shutdown()
    {
        g_shutdown.store(true, std::memory_order_seq_cst);
        for (int i = 0; i < 100 && (g_insidePaint || g_insideDrawItem); ++i)
            Sleep(1);
        DisableHooks();
        g_enableHooksRef = 0;
        g_insidePaint = false;
        g_insideDrawItem = false;
        g_shutdown.store(false, std::memory_order_seq_cst);
        DebugLog(L"[GlassCore][DropDownHooks] Shutdown");
    }

    namespace
    {
        thread_local HWND s_dropDownAppliedRoot{};
        thread_local DWORD s_dropDownAppliedTick{};
        thread_local bool s_applyingFlyoutAtTarget = false;

        [[nodiscard]] bool IsApplyingFlyoutAtTarget() noexcept
        {
            return s_applyingFlyoutAtTarget;
        }

        void LogHwndClass(HWND hwnd, const wchar_t* label) noexcept
        {
            if (!hwnd)
            {
                DebugLog(DBG_IMPORTANT, L"[GlassCore][DropDown] %s: (null)", label);
                return;
            }
            wchar_t cls[64]{};
            if (::GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls))) > 0)
            {
                DebugLog(DBG_IMPORTANT,
                    L"[GlassCore][DropDown] %s: hwnd=0x%p class=%s",
                    label,
                    hwnd,
                    cls);
            }
            else
            {
                DebugLog(DBG_IMPORTANT,
                    L"[GlassCore][DropDown] %s: hwnd=0x%p class=?",
                    label,
                    hwnd);
            }
        }

        void FillDropDownContext(WndCompat::DropDownContext& ctx) noexcept
        {
            ctx = {};
            ctx.useDarkMode = Shared::ThemeHelper::ResolveEffectiveDarkMode();
            GlassCore::Settings::QueryBackdropEffectContext(L"DropDown", ctx.useDarkMode, ctx.backdropEffect);
            GlassCore::Settings::QueryBorderContext(L"DropDown", ctx.useDarkMode, ctx.border);
            GlassCore::Settings::QueryFlyoutAnimationContext(L"DropDown", ctx.animation);
        }

        void ApplyDropDownFlyoutAtTarget(HWND target) noexcept
        {
            if (!target || !::IsWindow(target))
                return;

            if (s_applyingFlyoutAtTarget)
                return;

            s_applyingFlyoutAtTarget = true;
            auto clearApplying = wil::scope_exit([] { s_applyingFlyoutAtTarget = false; });

            auto& ctx = WndCompat::g_dropDownCtx;
            FillDropDownContext(ctx);

            const bool deferBorder = ctx.animation.enable;

            if (deferBorder)
            {
                GlassCore::FlyoutFrame::SetHostBorderDeferred(target, true);
                GlassCore::FlyoutFrame::SyncPrimeFlyoutNcBlack(target);
            }

            GlassCore::Settings::ApplyPopupSwcaEffect(
                target, ctx.useDarkMode, ctx.backdropEffect, ctx.border, false);

            if (!deferBorder)
            {
                GlassCore::FlyoutFrame::PaintHostBorder(
                    target, ctx.useDarkMode, ctx.border, ctx.backdropEffect.effectType);
            }

            LogHwndClass(target, L"ApplyDropDownFlyoutAtTarget");
            DebugLog(DBG_IMPORTANT,
                L"[GlassCore][DropDown] ApplyDropDownFlyoutAtTarget: hwnd=0x%p cornerType=%d effectType=%u anim=%d defer=%d",
                target,
                static_cast<int>(ctx.border.cornerType),
                ctx.backdropEffect.effectType,
                static_cast<int>(ctx.animation.enable),
                deferBorder ? 1 : 0);

            s_dropDownAppliedRoot = target;
            s_dropDownAppliedTick = ::GetTickCount();

            if (ctx.animation.enable)
            {
                const HRESULT hr = GlassCore::Animation::CreateDropDownPopupIn(
                    target,
                    static_cast<float>(ctx.animation.startRatio) / 100.f,
                    std::chrono::milliseconds{ ctx.animation.popInTime },
                    std::chrono::milliseconds{ ctx.animation.fadeInTime },
                    ctx.animation.popInStyle,
                    ctx.animation.immediateInterupting);
                DebugLog(L"[GlassCore][DropDown] CreateDropDownPopupIn hr=0x%08X hwnd=0x%p", hr, target);
            }
        }
    }

    // Паритет TF + ResolveDropDownEffectHwnd: shell DropDown, иначе GA_ROOT, иначе listview.
    void OnListviewPopupAttach(HWND hWndListview, HWND /*hWndShellUnused*/)
    {
        HWND const gaRoot = ::GetAncestor(hWndListview, GA_ROOT);
        HWND const target = Shared::MenuUtils::ResolveDropDownEffectHwnd(hWndListview);
        DebugLog(DBG_IMPORTANT,
            L"[GlassCore][DropDown] OnListviewPopupAttach: listview=0x%p gaRoot=0x%p target=0x%p",
            hWndListview,
            gaRoot,
            target);
        LogHwndClass(hWndListview, L"listview");
        LogHwndClass(gaRoot, L"gaRoot");
        LogHwndClass(target, L"target");
        ApplyDropDownFlyoutAtTarget(target);
    }

    void OnDropDownShow(HWND hWnd)
    {
        if (!hWnd || !::IsWindow(hWnd))
            return;

        HWND const gaRoot = ::GetAncestor(hWnd, GA_ROOT);
        HWND const target = Shared::MenuUtils::ResolveDropDownEffectHwnd(hWnd);
        const DWORD tick = ::GetTickCount();

        if (target && GlassCore::FlyoutFrame::IsHostBorderDeferred(target))
        {
            DebugLog(L"[GlassCore][DropDown] OnDropDownShow: skip (defer active) hwnd=0x%p target=0x%p",
                hWnd, target);
            return;
        }

        if (IsApplyingFlyoutAtTarget())
        {
            DebugLog(L"[GlassCore][DropDown] OnDropDownShow: skip (apply in progress) hwnd=0x%p", hWnd);
            return;
        }

        // WM_SHOWWINDOW после attach Listviewpopup — эффект уже на target (TF + resolve).
        if (target && target == s_dropDownAppliedRoot && (tick - s_dropDownAppliedTick) < 5000u)
        {
            DebugLog(L"[GlassCore][DropDown] OnDropDownShow: skip (already applied at attach) hwnd=0x%p target=0x%p",
                hWnd, target);
            return;
        }

        // WM_SHOWWINDOW + явный вызов из хоста (LogViewer) могут прийти подряд.
        static thread_local HWND s_lastHwnd{};
        static thread_local DWORD s_lastTick{};
        if (s_lastHwnd == hWnd && (tick - s_lastTick) < 200u)
        {
            DebugLog(L"[GlassCore][DropDown] OnDropDownShow: пропуск дубля за 200 ms hwnd=0x%p", hWnd);
            return;
        }
        s_lastHwnd = hWnd;
        s_lastTick = tick;

        DebugLog(L"[GlassCore][DropDown] OnDropDownShow: fallback apply hwnd=0x%p gaRoot=0x%p target=0x%p",
            hWnd, gaRoot, target);
        ApplyDropDownFlyoutAtTarget(target ? target : hWnd);
    }

    void OnViewControlAttach(HWND hWnd)
    {
        DebugLog(L"[GlassCore][DropDown] OnViewControlAttach: hwnd=0x%p", hWnd);
        ApplyDropDownFlyoutAtTarget(hWnd);
    }

    void PaintDeferredFlyoutBorderIfNeeded(HWND hWnd) noexcept
    {
        const auto& ctx = WndCompat::g_dropDownCtx;
        FlyoutFrame::PaintDeferredHostBorder(
            hWnd,
            ctx.useDarkMode,
            ctx.border,
            ctx.backdropEffect.effectType,
            FlyoutFrame::FlyoutHostBorderFlush::FrameOnly);
    }

    bool QueryDropDownContext(WndCompat::DropDownContext* out) noexcept
    {
        if (!out)
            return false;
        *out = WndCompat::g_dropDownCtx;
        return true;
    }

    void OnFlyoutAnimationFinished(HWND hWnd) noexcept
    {
        if (!hWnd || !::IsWindow(hWnd))
            return;

        PaintDeferredFlyoutBorderIfNeeded(hWnd);
        RefreshFlyoutClientAfterUncloak(hWnd);
        FlyoutFrame::ClearHostBorderDeferred(hWnd);
    }

    void ClearFlyoutBorderDeferred(HWND hWnd) noexcept
    {
        FlyoutFrame::ClearHostBorderDeferred(hWnd);
    }
}
