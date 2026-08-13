// IAT-хуки comctl32 для тултипов (BWM TooltipHooks → GlassCore + IATRegistry + g_overallMap).

#include "CorePch.h"

#include <Hooks/TooltipHooks.h>
#include <Core/Intercept/HookDispatcher.h>
#include <Module/ModuleHandler.h>
#include <Subsystems/RenderText.h>
#include <Subsystems/TooltipContext.h>
#include <Helpers/ThemeHelper.h>
#include <wil.h>
#include <CommCtrl.h>
#include <uxtheme.h>
#include <unordered_map>
#include <string_view>

using namespace GlassCore::Hooks;
using namespace std::literals;

namespace GlassCore::TooltipHooks
{
    static std::atomic<bool> g_shutdown{ false };

    int WINAPI Overall_MyDrawTextW(HDC, LPCWSTR, int, LPRECT, UINT);
    HRESULT WINAPI Overall_MyDrawThemeBackground(HTHEME, HDC, int, int, LPCRECT, LPCRECT);
    HRESULT WINAPI Overall_MyGetThemeMargins(HTHEME, HDC, int, int, int, LPCRECT, MARGINS*);
    int WINAPI Overall_MyDrawTextExW(HDC, LPWSTR, int, LPRECT, UINT, LPDRAWTEXTPARAMS);

    enum OverallHookIndex : size_t
    {
        OHK_DRAWTEXTW = 0,
        OHK_DRAWTHEMEBG,
        OHK_GETTHEMEMARGINS,
        OHK_DRAWTEXTEX,
        OHK_COUNT
    };

    inline const HookDispatcherDependency<OHK_COUNT, 3> g_overallHookDep
    { {
        std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
            "DrawTextW", reinterpret_cast<PVOID>(Overall_MyDrawTextW) },
        std::tuple{ std::array{ "uxtheme.dll"sv, "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv, ""sv },
            "DrawThemeBackground", reinterpret_cast<PVOID>(Overall_MyDrawThemeBackground) },
        std::tuple{ std::array{ "uxtheme.dll"sv, "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv, ""sv },
            "GetThemeMargins", reinterpret_cast<PVOID>(Overall_MyGetThemeMargins) },
        std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
            "DrawTextExW", reinterpret_cast<PVOID>(Overall_MyDrawTextExW) },
    } };

    using OverallDispatcher = HookDispatcher<OHK_COUNT, 3>;

    static std::unordered_map<void*, OverallDispatcher> g_overallMap;
    static wil::srwlock g_overallLock;
    static size_t         g_tooltipComctlPaintRef = 0;

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

    // ── DrawTextW ────────────────────────────────────────────────────────────

    int WINAPI Overall_MyDrawTextW(
        HDC hdc, LPCWSTR lpchText, int cchText, LPRECT lprc, UINT format)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_DRAWTEXTW, decltype(&DrawTextW)>(callerMod);

        if (!orig)
            return DrawTextW(hdc, lpchText, cchText, lprc, format);

        if (g_shutdown.load(std::memory_order_acquire))
            return orig(hdc, lpchText, cchText, lprc, format);

        if ((format & DT_CALCRECT) || (format & DT_INTERNAL) || (format & DT_NOCLIP))
            return orig(hdc, lpchText, cchText, lprc, format);

        auto& ctx = Tooltip::Detail::Context();
        const COLORREF oldColor = SetTextColor(hdc, ctx.rendering.color);
        auto restoreColor = wil::scope_exit([&] { SetTextColor(hdc, oldColor); });

        int result = 0;
        if (FAILED(GlassCore::Text::DrawTextWithAlpha(hdc, lpchText, cchText, lprc, format, result)))
            return orig(hdc, lpchText, cchText, lprc, format);

        return result;
    }

    int WINAPI Overall_MyDrawTextExW(
        HDC hdc, LPWSTR lpchText, int cchText,
        LPRECT lprc, UINT format, LPDRAWTEXTPARAMS lpdtp)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_DRAWTEXTEX, decltype(&DrawTextExW)>(callerMod);
        if (!orig)
            return DrawTextExW(hdc, lpchText, cchText, lprc, format, lpdtp);
        if (g_shutdown.load(std::memory_order_acquire))
            return orig(hdc, lpchText, cchText, lprc, format, lpdtp);
        if (g_tooltipComctlPaintRef == 0)
            return orig(hdc, lpchText, cchText, lprc, format, lpdtp);
        if (lpdtp)
            return orig(hdc, lpchText, cchText, lprc, format, lpdtp);
        int result = 0;
        if (!FAILED(GlassCore::Text::DrawTextWithAlpha(hdc, lpchText, cchText, lprc, format, result)))
            return result;
        return orig(hdc, lpchText, cchText, lprc, format, lpdtp);
    }

    // ── DrawThemeBackground ─────────────────────────────────────────────────

    HRESULT WINAPI Overall_MyDrawThemeBackground(
        HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
        LPCRECT pRect, LPCRECT pClipRect)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_DRAWTHEMEBG, decltype(&DrawThemeBackground)>(callerMod);

        if (!orig)
            return DrawThemeBackground(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

        if (g_shutdown.load(std::memory_order_acquire))
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

        if (!pRect || IsRectEmpty(pRect))
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

        WCHAR themeClassName[MAX_PATH + 1]{};
        if (FAILED(Shared::ThemeHelper::GetThemeClass(hTheme, themeClassName, MAX_PATH)))
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

        if (_wcsicmp(themeClassName, L"Tooltip") != 0)
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

        if (iPartId != TTP_STANDARD)
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

        RECT paintRect = *pRect;
        if (pClipRect)
            IntersectRect(&paintRect, &paintRect, pClipRect);

        PatBlt(hdc, paintRect.left, paintRect.top,
            paintRect.right - paintRect.left, paintRect.bottom - paintRect.top, BLACKNESS);
        return S_OK;
    }

    // ── GetThemeMargins ─────────────────────────────────────────────────────

    HRESULT WINAPI Overall_MyGetThemeMargins(
        HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
        int iPropId, LPCRECT prc, MARGINS* pMargins)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_GETTHEMEMARGINS, decltype(&GetThemeMargins)>(callerMod);

        if (!orig)
            return GetThemeMargins(hTheme, hdc, iPartId, iStateId, iPropId, prc, pMargins);

        if (g_shutdown.load(std::memory_order_acquire))
            return orig(hTheme, hdc, iPartId, iStateId, iPropId, prc, pMargins);

        HRESULT hr = orig(hTheme, hdc, iPartId, iStateId, iPropId, prc, pMargins);

        if (!pMargins)
            return hr;

        auto lock = g_overallLock.lock_shared();
        auto it = g_overallMap.find(static_cast<void*>(callerMod));
        if (it == g_overallMap.end() || !it->second.IsHookEnabled(OHK_GETTHEMEMARGINS))
            return hr;

        auto& ctx = Tooltip::Detail::Context();
        if (ctx.noMarginsHandling)
            return hr;

        if (iStateId != 0 || iPropId != TMT_CONTENTMARGINS || prc != nullptr)
            return hr;

        WCHAR themeClassName[MAX_PATH + 1]{};
        if (FAILED(Shared::ThemeHelper::GetThemeClass(hTheme, themeClassName, MAX_PATH)))
            return hr;

        if (_wcsicmp(themeClassName, L"Tooltip") != 0)
            return hr;

        if (ctx.rendering.marginsType == 0)
        {
            pMargins->cxLeftWidth += static_cast<int>(ctx.rendering.margins.cxLeftWidth);
            pMargins->cxRightWidth += static_cast<int>(ctx.rendering.margins.cxRightWidth);
            pMargins->cyTopHeight += static_cast<int>(ctx.rendering.margins.cyTopHeight);
            pMargins->cyBottomHeight += static_cast<int>(ctx.rendering.margins.cyBottomHeight);
        }
        else if (ctx.rendering.marginsType == 1)
        {
            *pMargins = ctx.rendering.margins;
        }

        return hr;
    }

    // ── Public API ───────────────────────────────────────────────────────────

    void Prepare() {}

    void Startup()
    {
        g_shutdown.store(false, std::memory_order_release);

        void* mod = GlassCore::ModuleHandler::g_comctl32New;
        if (!mod)
            return;

        auto lock = g_overallLock.lock_exclusive();
        auto [it, inserted] = g_overallMap.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(mod),
            std::forward_as_tuple(g_overallHookDep, mod));
        if (inserted)
        {
            it->second.CacheHookData();
            DebugLog(DBG_NORMAL, L"[GlassCore][TooltipHooks] Startup: cached comctl32 0x%p", mod);
        }
    }

    void Shutdown()
    {
        g_shutdown.store(true, std::memory_order_seq_cst);
        DisableAllOverallHooks();
        g_tooltipComctlPaintRef = 0;
        // Оставляем g_shutdown == true: запрет повторной регистрации IAT до выгрузки DLL (иначе use-after-free).
    }

    void EnableComctlPaintHooks(bool enable)
    {
        if (g_shutdown.load(std::memory_order_acquire))
            return;

        HMODULE m = GlassCore::ModuleHandler::g_comctl32New;
        if (!m) return;

        if (enable)
        {
            if (g_tooltipComctlPaintRef++ == 0
                && GlassCore::Text::IsInkGlowEffectActive())
                EnableHooksOnModule(m, OHK_DRAWTEXTEX, true);
        }
        else
        {
            if (g_tooltipComctlPaintRef > 0)
                --g_tooltipComctlPaintRef;
            if (g_tooltipComctlPaintRef == 0)
                EnableHooksOnModule(m, OHK_DRAWTEXTEX, false);
        }

        EnableHooksOnModule(m, OHK_DRAWTEXTW, enable);
        EnableHooksOnModule(m, OHK_DRAWTHEMEBG, enable);
    }

    void EnableMarginHooks(bool enable)
    {
        if (g_shutdown.load(std::memory_order_acquire))
            return;

        HMODULE m = GlassCore::ModuleHandler::g_comctl32New;
        if (!m) return;

        auto lock = g_overallLock.lock_exclusive();
        auto it = g_overallMap.find(static_cast<void*>(m));
        if (it == g_overallMap.end())
        {
            auto [ins, ok] = g_overallMap.emplace(
                std::piecewise_construct,
                std::forward_as_tuple(static_cast<void*>(m)),
                std::forward_as_tuple(g_overallHookDep, static_cast<void*>(m)));
            it = ins;
            it->second.CacheHookData();
        }
        else
        {
            it->second.moduleAddress = m;
        }

        it->second.EnableHookNoRef(OHK_GETTHEMEMARGINS, enable);
    }

    bool IsComctl32Ready() noexcept
    {
        return GlassCore::ModuleHandler::g_comctl32New != nullptr;
    }

    void SyncTextGlowHooksFromRegistry() noexcept
    {
        if (g_shutdown.load(std::memory_order_acquire))
            return;

        HMODULE m = GlassCore::ModuleHandler::g_comctl32New;
        if (!m || g_tooltipComctlPaintRef <= 0)
            return;

        EnableHooksOnModule(m, OHK_DRAWTEXTEX, GlassCore::Text::IsInkGlowEffectActive());
    }
}
