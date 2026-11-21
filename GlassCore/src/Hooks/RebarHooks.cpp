// Хуки для отрисовки ReBarWindow32.
//
// Архитектура: прямое управление хуками из RebarSubclassProc (WndCompat).
//
// WM_PAINT в сабклассе вызывает OnPaintBegin/OnPaintEnd — они включают/
// выключают IAT DrawThemeBackground на comctl32; текст — StatusBar-паттерн:
// !glow → DrawThemeText (comctl32); glow → DrawTextExW (uxtheme).
// DTB «Rebar» RP_BAND/RP_BACKGROUND → PatBlt(BLACKNESS); gripper/chevron/splitter → orig.
//
// PaintDispatcher НЕ используется для bracket — GetPaintSessionHostHwnd() выставляется
// BeginPaint внутри DefSubclassProc.

#include "CorePch.h"

#include <Hooks/Rebar.h>
#include <Subsystems/RenderText.h>
#include <Subsystems/TextGlowClientRegistry.h>
#include <Subsystems/PaintDispatcher.h>
#include <Helpers/ThemeHelper.h>
#include <Core/Intercept/HookDispatcher.h>
#include <Module/ModuleHandler.h>
#include <GlassControlKind.h>
#include <GlassBackdropMark.h>
#include <Subsystems/RootSwcaGradient.h>
#include <wil.h>
#include <uxtheme.h>
#include <vector>

using namespace GlassCore::Hooks;
using namespace std::literals;

namespace GlassCore::RebarControl
{
    // =========================================================================
    //  LIFO paint-сессий (сабкласс Rebar, не PaintDispatcher).
    // =========================================================================

    enum class RebarTextHook
    {
        None,
        DrawThemeText,
        DrawTextExW,
    };

    struct RebarPaintSession
    {
        HMODULE       comctlMod{};
        RebarTextHook textHook{ RebarTextHook::None };
    };

    thread_local std::vector<RebarPaintSession> g_rebarSessionStack;

    static std::atomic<bool> g_shutdown{ false };

    // =========================================================================
    //  Helpers
    // =========================================================================

    static bool StrEq(LPCWSTR a, LPCWSTR b)
    {
        return _wcsicmp(a, b) == 0;
    }

    // vsstyle.h REBARPARTS — gripper/chevron/splitter не затираем PatBlt.
    [[nodiscard]] static bool IsRebarThemedChromePart(int iPartId) noexcept
    {
        switch (iPartId)
        {
        case 1: // RP_GRIPPER
        case 2: // RP_GRIPPERVERT
        case 4: // RP_CHEVRON
        case 5: // RP_CHEVRONVERT
        case 7: // RP_SPLITTER
        case 8: // RP_SPLITTERVERT
            return true;
        default:
            return false;
        }
    }

    [[nodiscard]] static bool IsRebarGlassTransparentFillPart(int iPartId) noexcept
    {
        switch (iPartId)
        {
        case 3: // RP_BAND
        case 6: // RP_BACKGROUND
            return true;
        default:
            return false;
        }
    }

    [[nodiscard]] static bool IsRebarGlassSessionAllowed(HWND hWnd) noexcept
    {
        if (!GlassCore::Root::IsFullInjectionMode())
            return false;
        if (!hWnd)
            hWnd = GlassCore::PaintDispatcher::GetPaintSessionHostHwnd();
        if (!hWnd)
            return false;
        if (GlassShared::GlassGetWindowKind(hWnd) != GlassShared::GlassControlKind::Rebar)
            return false;
        return GlassCore::TextGlow::IsInkGlowAllowedForWindow(hWnd);
    }

    [[nodiscard]] static bool IsRebarGlassSessionAllowedForHdc(HDC hdc) noexcept
    {
        HWND hWnd = GlassCore::PaintDispatcher::GetPaintSessionHostHwnd();
        if (!hWnd)
            hWnd = WindowFromDC(hdc);
        return IsRebarGlassSessionAllowed(hWnd);
    }

    static void EnsureRebarTextColorOnDc(HDC hdc) noexcept
    {
        if (!hdc)
            return;
        if ((GetTextColor(hdc) & 0xFFFFFF) == 0)
            SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
    }

    // =========================================================================
    //  OVERALL DISPATCHER MAP
    // =========================================================================

    int WINAPI Overall_MyDrawThemeBg(HTHEME, HDC, int, int, LPCRECT, LPCRECT);
    HRESULT WINAPI Overall_MyDrawThemeText(HTHEME, HDC, int, int, LPCWSTR, int, DWORD, DWORD, LPCRECT);
    int WINAPI Overall_MyDrawTextExW(HDC, LPWSTR, int, LPRECT, UINT, LPDRAWTEXTPARAMS);

    enum OverallHookIndex : size_t
    {
        OHK_DRAWTHEMEBG = 0,
        OHK_DRAWTHEMETEXT,
        OHK_DRAWTEXTEX,
        OHK_COUNT
    };

    inline const HookDispatcherDependency<OHK_COUNT, 3> g_overallHookDep
    { {
        // OHK_DRAWTHEMEBG
        std::tuple{ std::array{ "uxtheme.dll"sv, "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv, ""sv },
            "DrawThemeBackground", reinterpret_cast<PVOID>(Overall_MyDrawThemeBg) },
        // OHK_DRAWTHEMETEXT (!glow)
        std::tuple{ std::array{ "uxtheme.dll"sv, "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv, ""sv },
            "DrawThemeText", reinterpret_cast<PVOID>(Overall_MyDrawThemeText) },
        // OHK_DRAWTEXTEX (glow: uxtheme → user32!DrawTextExW)
        std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
            "DrawTextExW", reinterpret_cast<PVOID>(Overall_MyDrawTextExW) },
    } };

    using OverallDispatcher = HookDispatcher<OHK_COUNT, 3>;

    static std::unordered_map<void*, OverallDispatcher> g_overallMap;
    static wil::srwlock g_overallLock;

    static bool EnableHooksOnModule(void* baseAddress, size_t index, bool enable)
    {
        if (!baseAddress) return false;

        auto lock = g_overallLock.lock_exclusive();

        auto it = g_overallMap.find(baseAddress);
        if (it == g_overallMap.end())
        {
            auto [inserted, ok] = g_overallMap.emplace(
                std::piecewise_construct,
                std::forward_as_tuple(baseAddress),
                std::forward_as_tuple(g_overallHookDep, baseAddress));
            it = inserted;
            it->second.CacheHookData();
        }
        else
        {
            it->second.moduleAddress = baseAddress;
        }

        it->second.EnableHook(index, enable);
        return true;
    }

    static void DisableAllOverallHooks()
    {
        auto lock = g_overallLock.lock_exclusive();
        for (auto& [addr, dispatcher] : g_overallMap)
            dispatcher.DisableAllHooks();
        g_overallMap.clear();
    }

    static HMODULE GetCallerModule(void* returnAddress)
    {
        HMODULE hMod = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(returnAddress),
            &hMod);
        return hMod;
    }

    template <size_t index, typename T>
    static T GetOverallOrigSafe(HMODULE callerModule)
    {
        auto lock = g_overallLock.lock_shared();
        auto it = g_overallMap.find(static_cast<void*>(callerModule));
        if (it != g_overallMap.end())
            return it->second.template GetOrg<index, T>();
        return nullptr;
    }

    // =========================================================================
    //  Hook implementations
    // =========================================================================

    int WINAPI Overall_MyDrawThemeBg(
        HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
        LPCRECT pRect, LPCRECT pClipRect)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_DRAWTHEMEBG, decltype(&DrawThemeBackground)>(callerMod);

        if (!orig)
            return DrawThemeBackground(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

        if (g_rebarSessionStack.empty())
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

        if (!GlassCore::Root::IsFullInjectionMode())
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

        const HWND hWnd = WindowFromDC(hdc);
        if (hWnd && !GlassShared::GlassIsBackdropAllowedForHwnd(hWnd))
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

        WCHAR themeClass[MAX_PATH + 1]{};
        if (SUCCEEDED(Shared::ThemeHelper::GetThemeClass(hTheme, themeClass, MAX_PATH))
            && StrEq(themeClass, L"Rebar")
            && pRect && !IsRectEmpty(pRect))
        {
            if (IsRebarThemedChromePart(iPartId))
                return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

            if (IsRebarGlassTransparentFillPart(iPartId))
            {
                const RECT* clipRect = pClipRect ? pClipRect : pRect;
                PatBlt(hdc,
                    clipRect->left, clipRect->top,
                    clipRect->right  - clipRect->left,
                    clipRect->bottom - clipRect->top,
                    BLACKNESS);
                return S_OK;
            }
        }

        return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
    }

    HRESULT WINAPI Overall_MyDrawThemeText(
        HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
        LPCWSTR pszText, int cchText, DWORD dwTextFlags, DWORD dwTextFlags2,
        LPCRECT pRect)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_DRAWTHEMETEXT, decltype(&DrawThemeText)>(callerMod);

        if (!orig)
            return DrawThemeText(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, dwTextFlags2, pRect);

        if (g_rebarSessionStack.empty())
            return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, dwTextFlags2, pRect);

        if (!pRect || IsRectEmpty(pRect) || !pszText || cchText == 0)
            return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, dwTextFlags2, pRect);

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

    int WINAPI Overall_MyDrawTextExW(
        HDC hdc, LPWSTR lpchText, int cchText,
        LPRECT lprc, UINT format, LPDRAWTEXTPARAMS lpdtp)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_DRAWTEXTEX, decltype(&DrawTextExW)>(callerMod);

        if (!orig)
            return DrawTextExW(hdc, lpchText, cchText, lprc, format, lpdtp);

        if (g_rebarSessionStack.empty())
            return orig(hdc, lpchText, cchText, lprc, format, lpdtp);

        if (!IsRebarGlassSessionAllowedForHdc(hdc))
            return orig(hdc, lpchText, cchText, lprc, format, lpdtp);

        if (lpdtp)
            return orig(hdc, lpchText, cchText, lprc, format, lpdtp);

        EnsureRebarTextColorOnDc(hdc);
        int result = 0;
        if (!FAILED(GlassCore::Text::DrawTextWithAlpha(hdc, lpchText, cchText, lprc, format, result)))
            return result;
        return orig(hdc, lpchText, cchText, lprc, format, lpdtp);
    }

    // =========================================================================
    //  Публичные функции — прямое управление хуками из WndCompat
    // =========================================================================

    void OnPaintBegin(HWND hWnd)
    {
        if (g_shutdown.load(std::memory_order_acquire)) return;
        if (!GlassCore::Root::IsFullInjectionMode()) return;
        if (!hWnd || !IsWindow(hWnd)) return;

        GlassCore::TextGlow::EnsureTextGlowClientRegistered(hWnd);

        HMODULE callerMod = GlassCore::ModuleHandler::g_comctl32New;
        if (!callerMod)
            callerMod = GlassCore::ModuleHandler::g_comctl32Legacy;
        if (!callerMod) return;

        const bool glow = GlassCore::Text::IsInkGlowEffectActiveForPaintSession(hWnd);
        HMODULE hUx = GetModuleHandleW(L"uxtheme.dll");

        RebarPaintSession session{ callerMod, RebarTextHook::None };
        g_rebarSessionStack.push_back(session);

        EnableHooksOnModule(callerMod, OHK_DRAWTHEMEBG, true);

        if (!glow || !hUx)
        {
            EnableHooksOnModule(callerMod, OHK_DRAWTHEMETEXT, true);
            g_rebarSessionStack.back().textHook = RebarTextHook::DrawThemeText;
        }
        else
        {
            EnableHooksOnModule(hUx, OHK_DRAWTEXTEX, true);
            g_rebarSessionStack.back().textHook = RebarTextHook::DrawTextExW;
        }

        DebugLog(DBG_NORMAL, L"[GlassCore][Rebar] OnPaintBegin: hWnd=0x%p callerMod=0x%p textHook=%d depth=%zu",
            hWnd, callerMod, static_cast<int>(g_rebarSessionStack.back().textHook), g_rebarSessionStack.size());
    }

    void OnPaintEnd()
    {
        if (!GlassCore::Root::IsFullInjectionMode()) return;
        if (g_rebarSessionStack.empty()) return;

        const RebarPaintSession session = g_rebarSessionStack.back();
        g_rebarSessionStack.pop_back();

        EnableHooksOnModule(session.comctlMod, OHK_DRAWTHEMEBG, false);

        switch (session.textHook)
        {
        case RebarTextHook::DrawThemeText:
            EnableHooksOnModule(session.comctlMod, OHK_DRAWTHEMETEXT, false);
            break;
        case RebarTextHook::DrawTextExW:
            if (HMODULE hUx = GetModuleHandleW(L"uxtheme.dll"))
                EnableHooksOnModule(hUx, OHK_DRAWTEXTEX, false);
            break;
        case RebarTextHook::None:
            break;
        }

        DebugLog(DBG_NORMAL, L"[GlassCore][Rebar] OnPaintEnd: callerMod=0x%p depth=%zu",
            session.comctlMod, g_rebarSessionStack.size());
    }

    void Prepare()  {}

    void Startup()
    {
        auto cacheModule = [&](void* mod)
        {
            if (!mod) return;
            auto lock = g_overallLock.lock_exclusive();
            auto [it, inserted] = g_overallMap.emplace(
                std::piecewise_construct,
                std::forward_as_tuple(mod),
                std::forward_as_tuple(g_overallHookDep, mod));
            if (inserted)
            {
                it->second.CacheHookData();
                DebugLog(DBG_NORMAL, L"[GlassCore][Rebar] Startup: cached 0x%p", mod);
            }
        };
        cacheModule(GlassCore::ModuleHandler::g_comctl32New);
        cacheModule(GlassCore::ModuleHandler::g_comctl32Legacy);
        cacheModule(GlassCore::ModuleHandler::g_entryPointModule);
        cacheModule(GetModuleHandleW(L"uxtheme.dll"));
    }

    void Shutdown()
    {
        g_shutdown.store(true, std::memory_order_seq_cst);
        DisableAllOverallHooks();
        g_rebarSessionStack.clear();
        g_shutdown.store(false, std::memory_order_seq_cst);
    }
}
