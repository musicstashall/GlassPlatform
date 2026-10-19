#include "CorePch.h"

#include <Hooks/UxThemeHooks.h>
#include <Core/Intercept/HookDispatcher.h>
#include <Core/Intercept/HookHelper.h>

#include <Subsystems/MenuRendering.h>
#include <Subsystems/MenuHandler.h>
#include <Subsystems/RenderText.h>

#include <Helpers/ThemeHelper.h>
#include <MenuUtils.h>
#include <WinApiUtils.h>
#include <Core/Intercept/DebugDetourHooks.h>
#include <Subsystems/GdiHookUtils.h>
#include <ColorUtils.h>
#include <vssym32.h>

using namespace std::literals;

namespace GlassCore::Menu
{
    bool IsInsideOwnerDraw() noexcept;
}

// private
static decltype(&::DrawThemeBackground) g_actualDrawThemeBackground{ nullptr };
static decltype(&::DrawThemeText)       g_actualDrawThemeText{ nullptr };
static decltype(&::DrawThemeTextEx)     g_actualDrawThemeTextEx{ nullptr };
static decltype(&::ExtTextOutW) g_actualExtTextOutW = ExtTextOutW;

namespace
{
    // Повторный резолв экспортов uxtheme (если Startup раньше не увидел модуль) — без изменения g_themeRef.
    void TryResolveUxThemeDrawExportsIfNeeded() noexcept
    {
        if (g_actualDrawThemeBackground && g_actualDrawThemeTextEx)
            return;

        HMODULE const hUxTheme = GetModuleHandleW(L"uxtheme.dll");
        if (!hUxTheme)
            return;

        if (!g_actualDrawThemeBackground)
        {
            g_actualDrawThemeBackground = reinterpret_cast<decltype(g_actualDrawThemeBackground)>(
                GetProcAddress(hUxTheme, "DrawThemeBackground"));
        }
        if (!g_actualDrawThemeTextEx)
        {
            g_actualDrawThemeTextEx = reinterpret_cast<decltype(g_actualDrawThemeTextEx)>(
                GetProcAddress(hUxTheme, "DrawThemeTextEx"));
        }
    }
}

namespace GlassCore::Hooks::UxThemeHooks
{
    static bool g_prepared = false;
    static bool g_started = false;

    // ------------------------------------------------------------
    // Forward declarations of detours
    // ------------------------------------------------------------

    HRESULT WINAPI MyDrawThemeBackground(
        HTHEME hTheme,
        HDC hdc,
        int iPartId,
        int iStateId,
        LPCRECT pRect,
        LPCRECT pClipRect);

    HRESULT WINAPI MyDrawThemeText(
        HTHEME hTheme,
        HDC hdc,
        int iPartId,
        int iStateId,
        LPCWSTR pszText,
        int cchText,
        DWORD dwTextFlags,
        DWORD dwTextFlags2,
        LPCRECT pRect);

    BOOL WINAPI MyStretchBlt(
        HDC hdcDest,
        int xDest,
        int yDest,
        int wDest,
        int hDest,
        HDC hdcSrc,
        int xSrc,
        int ySrc,
        int wSrc,
        int hSrc,
        DWORD rop);

    HRESULT WINAPI MyDwmSetWindowAttribute(
        HWND hwnd,
        DWORD dwAttribute,
        LPCVOID pvAttribute,
        DWORD cbAttribute);

    // *** НОВЫЙ ***
    BOOL WINAPI MyExtTextOutW(
        HDC hdc,
        int x,
        int y,
        UINT options,
        const RECT* lprect,
        LPCWSTR lpString,
        UINT c,
        const INT* lpDx);

    int WINAPI MyDrawTextExW(
        HDC hdc,
        LPWSTR lpchText,
        int cchText,
        LPRECT lprc,
        UINT format,
        LPDRAWTEXTPARAMS lpdtp);

    HRESULT WINAPI MyDrawThemeTextEx(
        HTHEME hTheme,
        HDC hdc,
        int iPartId,
        int iStateId,
        LPCWSTR pszText,
        int cchText,
        DWORD dwTextFlags,
        LPRECT pRect,
        const DTTOPTS* pOptions);

    // ------------------------------------------------------------
    // HookDispatcher dependency (ширина 4 кандидата импорта, как g_overallHookDep в ListViewHooks)
    //
    // Индексы UTHK_*:
    //   0 — DrawThemeBackground   (IAT uxtheme, детуры отдельно — g_actual*)
    //   1 — DrawThemeText
    //   2 — StretchBlt            (common, IAT uxtheme)
    //   3 — DwmSetWindowAttribute
    //   4 — DrawTextExW           (IAT uxtheme → user32; как ListView: при ink-glow, иначе DTT-Detour)
    // ExtTextOutW (Classic menu) — Detour на g_actualExtTextOutW, не IAT-таблица ниже.
    // ------------------------------------------------------------

    enum UxThemeHookIndex : size_t
    {
        UTHK_DRAWTHEMEBG = 0,
        UTHK_DRAWTHEMETEXT,
        UTHK_STRETCHBLT,
        UTHK_DWMSETWINDOWATTRIBUTE,
        UTHK_DRAWTEXTEXW,
        UTHK_COUNT
    };

    inline const HookDispatcherDependency<UTHK_COUNT, 4> g_hookDependency
    { {
        std::tuple{ std::array{ "uxtheme.dll"sv, "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv, ""sv, ""sv },
            "DrawThemeBackground", reinterpret_cast<void*>(MyDrawThemeBackground) },
        std::tuple{ std::array{ "uxtheme.dll"sv, "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv, ""sv, ""sv },
            "DrawThemeText", reinterpret_cast<void*>(MyDrawThemeText) },
        std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
            "StretchBlt", reinterpret_cast<void*>(MyStretchBlt) },
        std::tuple{ std::array{ "dwmapi.dll"sv, ""sv, ""sv, ""sv },
            "DwmSetWindowAttribute", reinterpret_cast<void*>(MyDwmSetWindowAttribute) },
        std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv, ""sv },
            "DrawTextExW", reinterpret_cast<void*>(MyDrawTextExW) },
    } };

    static HookDispatcher<UTHK_COUNT, 4> g_uxThemeDispatcher{ g_hookDependency, nullptr };

    static wil::srwlock g_lock;
    static size_t g_commonRef = 0; // StretchBlt + DwmSetWindowAttribute (IAT uxtheme)
    static size_t g_themeRef = 0;  // Detours DTB/DTT + IAT DrawTextExW (uxtheme) + ref ETW-Detour
    static size_t g_etwRef = 0;
    // Текст меню: IAT DrawTextExW → DrawTextWithAlpha (#126). Detours DrawThemeText не ставим:
    // DrawThemeTextEx рисует глиф послойно и затирает Glow.
    // DrawThemeTextEx-Detour: shell32 owner-draw → DrawThemeText, чтобы текст дошёл до DrawTextExW.
    static bool   s_enableThemeDttDetourArmed{ false };
    static bool   s_enableThemeDtexIatArmed{ false };
    static bool   s_enableThemeDttexDetourArmed{ false };
    thread_local HWND g_etwMenuHwnd = nullptr;
    // ------------------------------------------------------------
    // Helpers
    // ------------------------------------------------------------
    void SetCurrentMenuHwnd(HWND hWnd) noexcept
    {
        g_etwMenuHwnd = hWnd;
    }

    HWND GetCurrentMenuHwnd() noexcept
    {
        return g_etwMenuHwnd;
    }

    static bool EnsureTargetModule()
    {
        HMODULE target = GetModuleHandleW(L"uxtheme.dll");
        if (!target)
            return false;

        g_uxThemeDispatcher.moduleAddress = target;
        return true;
    }

    // ------------------------------------------------------------
    // Public API
    // ------------------------------------------------------------

    void Prepare()
    {
        if (g_prepared)
            return;

        g_prepared = true;
        DebugLog(L"[GlassCore][UxThemeHooks] Prepare done");
    }

    void Startup()
    {
        if (g_started)
            return;

        g_started = true;
        
        HMODULE hUxTheme = GetModuleHandleW(L"uxtheme.dll");
        if (hUxTheme)
        {
            g_actualDrawThemeBackground = reinterpret_cast<decltype(g_actualDrawThemeBackground)>(
                GetProcAddress(hUxTheme, "DrawThemeBackground"));
            /*g_actualDrawThemeText = reinterpret_cast<decltype(g_actualDrawThemeText)>(
                GetProcAddress(hUxTheme, "DrawThemeText"));*/
            g_actualDrawThemeTextEx = reinterpret_cast<decltype(g_actualDrawThemeTextEx)>(
                GetProcAddress(hUxTheme, "DrawThemeTextEx"));
        }

        // Резолвим ExtTextOutW — живёт в gdi32full.dll (если есть) или gdi32.dll
        {
            HMODULE hGdi = GetModuleHandleW(L"gdi32full.dll");
            if (!hGdi)
                hGdi = GetModuleHandleW(L"gdi32.dll");
            if (hGdi)
            {
                g_actualExtTextOutW = reinterpret_cast<decltype(g_actualExtTextOutW)>(
                    GetProcAddress(hGdi, "ExtTextOutW"));
            }
        }

        DebugLog(L"[GlassCore][UxThemeHooks] Startup: DTB=%p DTT=%p DTTEx=%p ETW=%p",
            g_actualDrawThemeBackground, g_actualDrawThemeText, g_actualDrawThemeTextEx, g_actualExtTextOutW);
    }

    void Shutdown()
    {
        if (!g_started)
            return;
        
        EnableThemeHooks(false);
        EnableHooks(false);

        g_started = false;
        DebugLog(L"[GlassCore][UxThemeHooks] Shutdown");
    }

    void EnableHooks(bool enable)
    {
        auto lock = g_lock.lock_exclusive();

        bool changed =
            (enable && g_commonRef == 0) ||
            (!enable && g_commonRef == 1);

        g_commonRef += enable ? 1 : static_cast<size_t>(-1);
        if (!changed)
            return;

        if (!EnsureTargetModule())
            return;

        g_uxThemeDispatcher.EnableHook(UTHK_STRETCHBLT, enable);
        g_uxThemeDispatcher.EnableHook(UTHK_DWMSETWINDOWATTRIBUTE, enable);

        DebugLog(L"[GlassCore][UxThemeHooks] EnableHooks: %d (ref=%zu)", enable, g_commonRef);
    }

    void EnableThemeHooks(bool enable)
    {
        auto lock = g_lock.lock_exclusive();

        if (enable)
            TryResolveUxThemeDrawExportsIfNeeded();

        if (enable &&
            (!g_actualDrawThemeBackground /*|| !g_actualDrawThemeText*/ || !g_actualDrawThemeTextEx))
        {
            DebugLog(DBG_WARNING,
                L"[GlassCore][UxThemeHooks] EnableThemeHooks(true): DrawThemeBackground=%p DrawThemeTextEx=%p — "
                L"пропуск без инкремента ref",
                reinterpret_cast<void*>(g_actualDrawThemeBackground),
                reinterpret_cast<void*>(g_actualDrawThemeTextEx));
            return;
        }

        // Защита от underflow: если g_themeRef уже 0, пропускаем decrement
        if (!enable && g_themeRef == 0)
        {
            DebugLog(DBG_WARNING,
                L"[GlassCore][UxThemeHooks] EnableThemeHooks(false): g_themeRef уже 0 — "
                L"пропуск декремента (несимметричный Detach или повторный вызов)");
            return;
        }

        bool changed =
            (enable && g_themeRef == 0) ||
            (!enable && g_themeRef == 1);

        g_themeRef += enable ? 1 : static_cast<size_t>(-1);
        if (!changed)
        {
            if (enable)
            {
                DebugLog(DBG_IMPORTANT,
                    L"[GlassCore][UxThemeHooks] EnableThemeHooks(true): вложенный владелец ref=%zu "
                    L"(детуры уже на месте)",
                    g_themeRef);
            }
            return;
        }

        Hooks::Detours::Write([&]
            {
                if (enable)
                {
                    Hooks::Detours::Attach(
                        reinterpret_cast<PVOID*>(&g_actualDrawThemeBackground),
                        MyDrawThemeBackground);
                    /*Hooks::Detours::Attach(
                        reinterpret_cast<PVOID*>(&g_actualDrawThemeText),
                        MyDrawThemeText);*/
                    Hooks::Detours::Attach(
                        reinterpret_cast<PVOID*>(&g_actualDrawThemeTextEx),
                        MyDrawThemeTextEx);
                }
                else
                {
                    /*Hooks::Detours::Detach(
                        reinterpret_cast<PVOID*>(&g_actualDrawThemeText),
                        MyDrawThemeText);*/
                    Hooks::Detours::Detach(
                        reinterpret_cast<PVOID*>(&g_actualDrawThemeTextEx),
                        MyDrawThemeTextEx);
                    Hooks::Detours::Detach(
                        reinterpret_cast<PVOID*>(&g_actualDrawThemeBackground),
                        MyDrawThemeBackground);
                    
                }
            });

        if (EnsureTargetModule())
        {
            g_uxThemeDispatcher.EnableHook(UTHK_DRAWTEXTEXW, enable);
        }
        

        DebugLog(L"[GlassCore][UxThemeHooks] EnableThemeHooks: %d (ref=%zu)", enable, g_themeRef);
    }

    void BeginMenuBarUxThemeDraw(HWND menuOwnerHwnd) noexcept
    {
        // Порядок как в BWM WinHandler: сначала флаг обработки, затем детуры.
        GlassCore::Menu::MenuHandler::SetUxHooksEnabled(true);
        EnableThemeHooks(true);

        // Classic (нет visual styles): user32 рисует текст через ExtTextOutW — как MN_SIZEWINDOW Classic.
        if (menuOwnerHwnd && !Shared::WinApiUtils::IsThemed(menuOwnerHwnd, L"Menu"))
        {
            SetCurrentMenuHwnd(menuOwnerHwnd);
            EnableExtTextOutHook(true);
        }
    }

    void EndMenuBarUxThemeDraw(HWND menuOwnerHwnd) noexcept
    {
        if (menuOwnerHwnd && !Shared::WinApiUtils::IsThemed(menuOwnerHwnd, L"Menu"))
        {
            EnableExtTextOutHook(false);
            SetCurrentMenuHwnd(nullptr);
        }

        EnableThemeHooks(false);
        GlassCore::Menu::MenuHandler::SetUxHooksEnabled(false);
    }

    void EnableExtTextOutHook(bool enable)
    {
        auto lock = g_lock.lock_exclusive();

        bool changed =
            (enable && g_etwRef == 0) ||
            (!enable && g_etwRef == 1);

        g_etwRef += enable ? 1 : static_cast<size_t>(-1);
        if (!changed)
            return;

        if (!g_actualExtTextOutW)
        {
            DebugLog(L"[GlassCore][UxThemeHooks] EnableExtTextOutHook: ETW address unknown, skipping");
            return;
        }

        DebugLog(L"[GlassCore][UxThemeHooks] EnableExtTextOutHook %d: ETW=%p (ref=%zu)",
            enable, g_actualExtTextOutW, g_etwRef);

        Hooks::Detours::Write([&]()
            {
                if (enable)
                {
                    Hooks::Detours::Attach(
                        reinterpret_cast<PVOID*>(&g_actualExtTextOutW),
                        MyExtTextOutW);
                }
                else
                {
                    Hooks::Detours::Detach(
                        reinterpret_cast<PVOID*>(&g_actualExtTextOutW),
                        MyExtTextOutW);
                }
            });
    }

    // ============================================================
    // DETOUR IMPLEMENTATIONS
    // ============================================================

    HRESULT WINAPI MyDrawThemeBackground(
        HTHEME hTheme,
        HDC hdc,
        int iPartId,
        int iStateId,
        LPCRECT pRect,
        LPCRECT pClipRect)
    {
        bool uxhooks = GlassCore::Menu::MenuHandler::IsUxHooksEnabled();
        DebugLog(L"[GlassCore][UxTheme] MyDrawThemeBackground called: partId=%d stateId=%d rect=[%d,%d,%d,%d",
            iPartId, iStateId,
			pRect ? pRect->left : -1, pRect ? pRect->top : -1, pRect ? pRect->right : -1, pRect ? pRect->bottom : -1);
        if (!uxhooks)
        {
            return g_actualDrawThemeBackground(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
        }
        WCHAR themeClassName[MAX_PATH + 1]{};
        HRESULT hrClass = Shared::ThemeHelper::GetThemeClass(hTheme, themeClassName, MAX_PATH);
        if (FAILED(hrClass) || _wcsicmp(themeClassName, L"Menu") != 0)
        {
            return g_actualDrawThemeBackground(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
        }
        if (!pRect || IsRectEmpty(pRect))
        {
            return g_actualDrawThemeBackground(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
        }
        RECT clipRect{};
        CopyRect(&clipRect, pRect);
        if (pClipRect)
            IntersectRect(&clipRect, &clipRect, pClipRect);
        // BWM MyDrawThemeBackground: меню-бар ...
        HMODULE callerModule = DetourGetContainingModule(_ReturnAddress());
        constexpr int kMenuBarBackground = 7;
        constexpr int kMenuBarItem = 8;
        if (callerModule == GetModuleHandleW(L"uxtheme.dll") &&
            (iPartId == kMenuBarBackground ||
                (iPartId == kMenuBarItem && (iStateId == 1 || iStateId == 3 || iStateId == 6))))
        {
            PatBlt(hdc, clipRect.left, clipRect.top,
                clipRect.right - clipRect.left, clipRect.bottom - clipRect.top, BLACKNESS);
            return S_OK;
        }
        COLORREF color{ DWMWA_COLOR_NONE }, expectedColor{ 0 };
        if (
            SUCCEEDED(GetThemeColor(hTheme, MENU_POPUPITEM, 0, TMT_TEXTCOLOR, &color)) &&
            SUCCEEDED(GetThemeColor(
                wil::unique_htheme{ OpenThemeData(nullptr, L"DarkMode::Menu") }.get(),
                MENU_POPUPITEM, 0, TMT_TEXTCOLOR, &expectedColor)) &&
            color == expectedColor
            )
        {
            GlassCore::Menu::MenuHandler::SetDarkMode(true);
        }
        else
        {
            GlassCore::Menu::MenuHandler::SetDarkMode(false);
        }
        GlassCore::Menu::MenuHandler::SetUseUxTheme(true);
        const auto& ctx = GlassCore::Menu::MenuHandler::GetContext();

        // Immersive стиль — корректировки и HOT highlight
        if (ctx.immersiveStyle)
        {
            constexpr int immersiveContextMenuSeparatorPadding = 8;
            RECT clipBoxRect{};
            GetClipBox(hdc, &clipBoxRect);

            if (iPartId == MENU_POPUPSEPARATOR)
                SetRect(const_cast<LPRECT>(pRect),
                    clipBoxRect.left + immersiveContextMenuSeparatorPadding, pRect->top,
                    clipBoxRect.right - immersiveContextMenuSeparatorPadding, pRect->bottom);

            if (iPartId == MENU_POPUPCHECK)
                OffsetRect(const_cast<LPRECT>(pRect),
                    clipBoxRect.left + immersiveContextMenuSeparatorPadding - pRect->left, 0);

            if (iPartId == MENU_POPUPCHECKBACKGROUND)
                return S_OK;

            // Паритет BWM/TF: ImmersiveStart HOT только без EnableCustomRendering;
            // иначе MenuRendering рисует hot_color из Menu\Hot\LightMode_Color / DarkMode_Color.
            if ((iPartId == MENU_POPUPITEM || iPartId == MENU_POPUPITEMFOCUSABLE) &&
                iStateId == MPI_HOT &&
                !ctx.customRendering.enable)
            {
                const wchar_t* themeClass = ctx.useDarkMode
                    ? L"DarkMode_ImmersiveStart::Menu"
                    : L"LightMode_ImmersiveStart::Menu";
                wil::unique_htheme immTheme{ OpenThemeData(nullptr, themeClass) };
                if (!immTheme)
                    return g_actualDrawThemeBackground(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

                DebugLog(L"[GlassCore][UxTheme] DTB: ImmersiveStart HOT partId=%d dark=%d", iPartId, ctx.useDarkMode);
                g_actualDrawThemeBackground(immTheme.get(), hdc, iPartId, iStateId, pRect, pClipRect);
                return S_OK;
            }
        }

        // Делегируем в MenuRendering
        if (GlassCore::Menu::Rendering::HandleDrawThemeBackground(
            hTheme, hdc, iPartId, iStateId, pRect, pClipRect, g_actualDrawThemeBackground))
        {
            return S_OK;
        }

        return g_actualDrawThemeBackground(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
    }

    // Не вешается: тело через DrawThemeTextEx затирает Glow. Текст идёт в MyDrawTextExW / MyExtTextOutW.
    HRESULT WINAPI MyDrawThemeText(
        HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
        LPCWSTR pszText, int cchText, DWORD dwTextFlags, DWORD dwTextFlags2,
        LPCRECT pRect)
    {
        HMODULE callerModule = DetourGetContainingModule(_ReturnAddress());
        HMODULE uxtheme = GetModuleHandleW(L"uxtheme.dll");

        if (callerModule != uxtheme)
        {
            return g_actualDrawThemeText(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, dwTextFlags2, pRect);
        }

        bool uxhooks = GlassCore::Menu::MenuHandler::IsUxHooksEnabled();
        if (!uxhooks)
        {
            return g_actualDrawThemeText(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, dwTextFlags2, pRect);
        }

        WCHAR themeClassName[MAX_PATH + 1]{};
        HRESULT hrClass = Shared::ThemeHelper::GetThemeClass(hTheme, themeClassName, MAX_PATH);
        if (FAILED(hrClass))
        {
            return g_actualDrawThemeText(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, dwTextFlags2, pRect);
        }

        if (_wcsicmp(themeClassName, L"Menu") != 0)
        {
            return g_actualDrawThemeText(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, dwTextFlags2, pRect);
        }

        if (!pRect || IsRectEmpty(pRect))
        {
            return g_actualDrawThemeText(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, dwTextFlags2, pRect);
        }

        thread_local bool inside = false;
        if (inside)
        {
            return g_actualDrawThemeText(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, dwTextFlags2, pRect);
        }

        inside = true;
        auto restore = wil::scope_exit([&] { inside = false; });
        DebugLog(L"[GlassCore][UxTheme] MyDrawThemeText pszText=%s", pszText);
        RECT rc = *pRect;
        DTTOPTS opts{ sizeof(DTTOPTS), DTT_COMPOSITED };
        return GlassCore::Text::DrawThemeContent(
            hdc, rc, nullptr, nullptr, 0,
            [&](HDC memDC, HPAINTBUFFER, RGBQUAD*, int)
            {
                DrawThemeTextEx(hTheme, memDC, iPartId, iStateId,
                    pszText, cchText, dwTextFlags, &rc, &opts);
            });
    }

    // DrawThemeTextEx: перенесено из ImmersiveHooks — shell32 + owner-draw, подмена на DrawThemeText.
    HRESULT WINAPI MyDrawThemeTextEx(
        HTHEME hTheme,
        HDC hdc,
        int iPartId,
        int iStateId,
        LPCWSTR pszText,
        int cchText,
        DWORD dwTextFlags,
        LPRECT pRect,
        const DTTOPTS* pOptions)
    {
        auto orig = g_actualDrawThemeTextEx;
        if (!orig)
        {
            return DrawThemeTextEx(
                hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions);
        }

        const HMODULE hShell32 = GetModuleHandleW(L"shell32.dll");
        if (!hShell32 || DetourGetContainingModule(_ReturnAddress()) != hShell32)
        {
            return orig(
                hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions);
        }

        bool uxhooks = GlassCore::Menu::MenuHandler::IsUxHooksEnabled();
        if (!uxhooks)
        {
            return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions);
        }

        WCHAR themeClassName[MAX_PATH + 1]{};
        HRESULT hrClass = Shared::ThemeHelper::GetThemeClass(hTheme, themeClassName, MAX_PATH);
        if (FAILED(hrClass))
        {
            return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions);
        }

        if (_wcsicmp(themeClassName, L"Menu") != 0)
        {
            return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions);
        }

        if (!GlassCore::Menu::IsInsideOwnerDraw())
        {
            return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions);
        }

        if (!pRect || IsRectEmpty(pRect))
        {
            return orig(
                hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions);
        }

        DTTOPTS defaultOpts{};
        defaultOpts.dwSize = sizeof(DTTOPTS);
        const DTTOPTS* effective = pOptions ? pOptions : &defaultOpts;
        DTTOPTS work = *effective;
        if (work.dwSize < sizeof(DTTOPTS))
            work.dwSize = sizeof(DTTOPTS);

        if (work.dwFlags & DTT_CALCRECT)
        {
            return orig(
                hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, &work);
        }

        const bool saved = SaveDC(hdc) != 0;
        if (work.dwFlags & DTT_TEXTCOLOR)
            SetTextColor(hdc, work.crText);
        if (work.dwFlags & DTT_COMPOSITED)
            SetBkMode(hdc, TRANSPARENT);

        const HRESULT hr = DrawThemeText(
            hTheme,
            hdc,
            iPartId,
            iStateId,
            pszText,
            cchText,
            dwTextFlags,
            0,
            pRect);
        if (saved)
            RestoreDC(hdc, -1);
        return hr;
    }

    // ============================================================
    // MyExtTextOutW — NT-меню: popup (MN_GETHMENU) и меню-бар (BWM MenuBar::WorkExtTextOutW).
    //
    // HWND: thread_local сессия (NCACTIVATE/UAH) или WindowFromDC(hdc) при отрисовке из WM_NCPAINT.
    // Меню-бар: GetMenuBarInfo + GetMenu + MenuItemFromPoint + thread_local olditem.
    // UxTheme + ETO_OPAQUE: PatBlt BLACKNESS (как BWM MenuBar ветка).
    //
    // Guard: реентерантность DrawTextWithAlpha → ExtTextOutW.
    // ============================================================

    BOOL WINAPI MyExtTextOutW(
        HDC hdc,
        int x,
        int y,
        UINT options,
        const RECT* lprect,
        LPCWSTR lpString,
        UINT c,
        const INT* lpDx)
    {
        HMODULE callerModule = DetourGetContainingModule(_ReturnAddress());
        HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
        HMODULE hUxTheme = GetModuleHandleW(L"uxtheme.dll");

        if (callerModule == hUxTheme && options == ETO_OPAQUE && lprect)
        {
            PatBlt(hdc, lprect->left, lprect->top,
                lprect->right - lprect->left, lprect->bottom - lprect->top, BLACKNESS);
            return TRUE;
        }

        if (callerModule != hUser32)
            return g_actualExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);

        if (options)
            return g_actualExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);

        thread_local bool inside = false;
        if (inside)
            return g_actualExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);

        // Сессия из WM_NCACTIVATE / UAH; иначе BWM: HWND из DC (WM_NCPAINT без UAH).
        HWND hWnd = Hooks::UxThemeHooks::GetCurrentMenuHwnd();
        if (!hWnd && hdc)
            hWnd = WindowFromDC(hdc);
        if (!hWnd)
            return g_actualExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);

        const bool retOnlyBackground =
            (c == 0 || (c == 1 && lpString && static_cast<int>(lpString[0]) == 0x20));

        inside = true;
        auto restoreGuard = wil::scope_exit([&] { inside = false; });

        // ── Меню-бар (окно-владелец): зеркало BWM MenuBar::WorkExtTextOutW ──
        MENUBARINFO mbi{ sizeof(mbi) };
        if (GetMenuBarInfo(hWnd, OBJID_MENU, 0, &mbi))
        {
            RECT client{};
            client.right = mbi.rcBar.right - mbi.rcBar.left;
            client.bottom = mbi.rcBar.bottom - mbi.rcBar.top;

            POINT ptScreen{};
            GetCursorPos(&ptScreen);
            HMENU menu = GetMenu(hWnd);
            if (!menu)
                return g_actualExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);

            int menuitem = MenuItemFromPoint(hWnd, menu, ptScreen);

            thread_local HWND s_menuBarEtioOwner{};
            thread_local int s_menuBarOldItem{ -1 };
            if (s_menuBarEtioOwner != hWnd)
            {
                s_menuBarEtioOwner = hWnd;
                s_menuBarOldItem = -1;
            }

            RECT rcItem{};
            COLORREF color = GetBkColor(hdc);

            if (menuitem > -1)
            {
                if (color == GetSysColor(COLOR_HIGHLIGHT))
                    GetMenuItemRect(hWnd, menu, menuitem, &rcItem);
                else if (s_menuBarOldItem >= 0)
                    GetMenuItemRect(hWnd, menu, s_menuBarOldItem, &rcItem);
                else
                    GetMenuItemRect(hWnd, menu, menuitem, &rcItem);
                client.right = rcItem.right - rcItem.left;
            }
            else if (s_menuBarOldItem > -1)
            {
                GetMenuItemRect(hWnd, menu, s_menuBarOldItem, &rcItem);
                client.right = rcItem.right - rcItem.left;
            }

            s_menuBarOldItem = menuitem;

            if (color == GetSysColor(COLOR_HIGHLIGHT))
            {
                wil::unique_hbrush brush{ Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(color, 255) };
                if (brush)
                    FillRect(hdc, &client, brush.get());
            }
            else
            {
                PatBlt(hdc, client.left, client.top,
                    client.right - client.left, client.bottom - client.top, BLACKNESS);
            }

            if (retOnlyBackground)
                return TRUE;

            if (c > 0 && !lpString)
                return g_actualExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);

            client.left = x;
            client.top = y;
            int drawResult = 0;
            return !FAILED(GlassCore::Text::DrawTextWithAlpha(
                hdc, lpString, static_cast<int>(c), &client, GetTextAlign(hdc), drawResult));
        }

        // ── Классическое popup-меню (#32768): MN_GETHMENU ──
        HMENU popupMenu{ reinterpret_cast<HMENU>(SendMessageW(hWnd, MN_GETHMENU, 0, 0)) };
        if (!popupMenu)
            return g_actualExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);

        POINT ptScreen{};
        GetCursorPos(&ptScreen);
        int menuitem = MenuItemFromPoint(hWnd, popupMenu, ptScreen);

        RECT rcItem{};
        COLORREF color = GetBkColor(hdc);

        if (menuitem > -1 && color == GetSysColor(COLOR_HIGHLIGHT))
            GetMenuItemRect(hWnd, popupMenu, menuitem, &rcItem);
        else
            GetMenuItemRect(hWnd, popupMenu, 0, &rcItem);

        RECT client{};
        CopyRect(&client, &rcItem);
        OffsetRect(&client, -client.left, -client.top);

        if (!color || color == GetSysColor(COLOR_WINDOW) || color == GetSysColor(COLOR_BTNFACE))
        {
            PatBlt(hdc, client.left, client.top,
                client.right - client.left, client.bottom - client.top, BLACKNESS);
        }
        else
        {
            wil::unique_hbrush brush{ Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(color, 255) };
            if (brush)
                FillRect(hdc, &client, brush.get());
        }

        if (retOnlyBackground)
            return TRUE;

        if (c > 0 && !lpString)
            return g_actualExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);

        client.left = x;
        client.top = y;
        int drawResult = 0;
        return !FAILED(GlassCore::Text::DrawTextWithAlpha(
            hdc, lpString, static_cast<int>(c), &client, GetTextAlign(hdc), drawResult));
    }

    BOOL WINAPI MyStretchBlt(
        HDC hdcDest,
        int xDest,
        int yDest,
        int wDest,
        int hDest,
        HDC hdcSrc,
        int xSrc,
        int ySrc,
        int wSrc,
        int hSrc,
        DWORD rop)
    {
        auto orig = g_uxThemeDispatcher.GetOrg<UTHK_STRETCHBLT, decltype(&MyStretchBlt)>();
        if (!orig)
        {
            return StretchBlt(
                hdcDest, xDest, yDest, wDest, hDest,
                hdcSrc, xSrc, ySrc, wSrc, hSrc, rop);
        }

        if (!GlassCore::Menu::MenuHandler::IsUxHooksEnabled()
            || !wSrc || !hSrc || !wDest || !hDest
            || DetourGetContainingModule(_ReturnAddress()) != GetModuleHandleW(L"uxtheme.dll"))
        {
            return orig(
                hdcDest, xDest, yDest, wDest, hDest,
                hdcSrc, xSrc, ySrc, wSrc, hSrc, rop);
        }

        auto hBitmap = reinterpret_cast<HBITMAP>(GetCurrentObject(hdcSrc, OBJ_BITMAP));
        GlassCore::GdiUtils::BitmapAlphaPrepareOptions opts{};
        const auto& removal = GlassCore::Menu::MenuHandler::GetContext().iconBackgroundColorRemoval;
        if (removal.enable)
        {
            opts.chromaKey = true;
            opts.chromaColor = Shared::ColorUtils::ARGBToCOLORREF(removal.colorTreatAsTransparent);
            opts.chromaThreshold = static_cast<BYTE>(removal.colorTreatAsTransparentThreshold);
        }

        wil::unique_hbitmap converted{};
        if (!GlassCore::GdiUtils::PrepareBitmapForAlphaBlend(hBitmap, converted, opts))
        {
            return orig(
                hdcDest, xDest, yDest, wDest, hDest,
                hdcSrc, xSrc, ySrc, wSrc, hSrc, rop);
        }

        const HGDIOBJ previous = SelectObject(hdcSrc, hBitmap);
        auto restoreBitmap = wil::scope_exit([&]
        {
            if (previous)
                SelectObject(hdcSrc, previous);
        });

        BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        if (!GdiAlphaBlend(
            hdcDest, xDest, yDest, wDest, hDest,
            hdcSrc, xSrc, ySrc, wSrc, hSrc, blend))
        {
            return orig(
                hdcDest, xDest, yDest, wDest, hDest,
                hdcSrc, xSrc, ySrc, wSrc, hSrc, rop);
        }

        return TRUE;
    }

    HRESULT WINAPI MyDwmSetWindowAttribute(
        HWND hwnd,
        DWORD dwAttribute,
        LPCVOID pvAttribute,
        DWORD cbAttribute)
    {
        const auto& border = GlassCore::Menu::MenuHandler::GetContext().border;
        COLORREF color{
            RGB(
                (border.color >> 16) & 0xFF,
                (border.color >> 8) & 0xFF,
                border.color & 0xFF) };

        if (GlassCore::Menu::MenuHandler::IsUxHooksEnabled() && Shared::MenuUtils::IsPopupMenu(hwnd))
        {
            if (dwAttribute == DWMWA_WINDOW_CORNER_PREFERENCE)
            {
                if (border.cornerType != DWMWCP_DEFAULT)
                    pvAttribute = &border.cornerType;
            }

            if (dwAttribute == DWMWA_BORDER_COLOR)
            {
                if (border.colorUseNone)
                {
                    color = DWMWA_COLOR_NONE;
                    pvAttribute = &color;
                }
                else if (!border.colorUseDefault)
                {
                    pvAttribute = &color;
                }
            }
        }

        return g_uxThemeDispatcher.GetOrg<UTHK_DWMSETWINDOWATTRIBUTE, decltype(&MyDwmSetWindowAttribute)>()(
            hwnd, dwAttribute, pvAttribute, cbAttribute);
    }

    int WINAPI MyDrawTextExW(
        HDC hdc,
        LPWSTR lpchText,
        int cchText,
        LPRECT lprc,
        UINT format,
        LPDRAWTEXTPARAMS lpdtp)
    {
        auto orig = g_uxThemeDispatcher.GetOrg<UTHK_DRAWTEXTEXW, decltype(&DrawTextExW)>();
        if (!orig)
            return DrawTextExW(hdc, lpchText, cchText, lprc, format, lpdtp);
        if (!GlassCore::Menu::MenuHandler::IsUxHooksEnabled())
            return orig(hdc, lpchText, cchText, lprc, format, lpdtp);
        if (lpdtp)
            return orig(hdc, lpchText, cchText, lprc, format, lpdtp);
        
        LOGFONTW lfMenu{};
        wil::unique_hfont hMenuFont;
        if (SUCCEEDED(GetThemeSysFont(static_cast<HTHEME>(nullptr), TMT_MENUFONT, &lfMenu)))
        {
            if (HFONT h = CreateFontIndirectW(&lfMenu))
                hMenuFont.reset(h);
        }

        HGDIOBJ const oldGdi = hMenuFont ? SelectObject(hdc, hMenuFont.get()) : nullptr;
        // Сначала вернуть прежний объект в DC, затем unique_hfont сделает DeleteObject (нельзя удалять шрифт, пока выбран в HDC).
        auto restoreGdi = wil::scope_exit([&] {
            if (oldGdi)
                SelectObject(hdc, oldGdi);
        });
        DebugLog(L"[GlassCore][UxThemeHooks] MyDrawTextExW lpchText=%s", lpchText);
        int result = 0;
        if (!FAILED(GlassCore::Text::DrawTextWithAlpha(
                hdc, lpchText, cchText, lprc, format, result)))
            return result;
        return orig(hdc, lpchText, cchText, lprc, format, lpdtp);
    }

}