// Хуки для отрисовки SysHeader32 (заголовки столбцов ListView).
//
// Архитектура (PaintDispatcher + per-module g_overallMap):
//
// Header рисует фон через DrawThemeBackground (uxtheme IAT) и текст через
// DrawThemeTextEx (themed) или ExtTextOutW/DrawTextW (unthemed).
// Хуки включаются на callerModule при каждом BeginPaint:
//   callerModule определяется через _ReturnAddress() в хуке PaintDispatcher.
//
// BWM-паттерн: PaintHandler.cpp lines 218-238.
//   Themed:   DrawThemeBackground + DrawThemeTextEx
//   Unthemed: ExtTextOutW + DrawTextW
//   callerModule == comctl32 → WinHooks (DrawThemeBg/TextEx)
//   callerModule == overall_comctl32 → OverallHooks (ExtTextOutW)

#include "CorePch.h"

#include <Hooks/Header.h>
#include <Subsystems/RenderText.h>
#include <Helpers/ThemeHelper.h>
#include <Core/Intercept/HookDispatcher.h>
#include <Module/ModuleHandler.h>
#include <Subsystems/PaintDispatcher.h>
#include <Subsystems/TextGlowClientRegistry.h>
#include <GlassControlKind.h>
#include <wil.h>
#include <CommCtrl.h>
#include <uxtheme.h>
#include <WinApiUtils.h>
#include <vector>

using namespace GlassCore::Hooks;
using namespace std::literals;

namespace GlassCore::HeaderControl
{
    // =========================================================================
    //  LIFO сессий paint: какой набор IAT включён (themed / unthemed / legacy).
    // =========================================================================

    enum class HeaderSessionKind { NewThemed, NewUnthemed, Legacy };
    struct HeaderPaintSession { HMODULE mod; HeaderSessionKind kind; };
    thread_local std::vector<HeaderPaintSession> g_headerStack;

    static std::atomic<bool> g_shutdown{ false };

    // =========================================================================
    //  Helpers
    // =========================================================================

    static bool StrEq(LPCWSTR a, LPCWSTR b)
    {
        return _wcsicmp(a, b) == 0;
    }

    // =========================================================================
    //  OVERALL DISPATCHER MAP
    // =========================================================================

    int      WINAPI Overall_MyDrawThemeBg(HTHEME, HDC, int, int, LPCRECT, LPCRECT);
    HRESULT  WINAPI Overall_MyDrawThemeTextEx(HTHEME, HDC, int, int, LPCWSTR, int, DWORD, LPRECT, const DTTOPTS*);
    BOOL     WINAPI Overall_MyExtTextOutW(HDC, int, int, UINT, const RECT*, LPCWSTR, UINT, const INT*);
    int      WINAPI Overall_MyDrawTextW(HDC, LPCWSTR, int, LPRECT, UINT);
    int      WINAPI Overall_MyDrawTextExW(HDC, LPWSTR, int, LPRECT, UINT, LPDRAWTEXTPARAMS);

    enum OverallHookIndex : size_t
    {
        OHK_DRAWTHEMEBG = 0,    // themed Header: фон
        OHK_DRAWTHEMETEXTEX,     // themed Header: текст
        OHK_EXTTEXTOUT,          // unthemed Header: текст + фон
        OHK_DRAWTEXT,            // unthemed Header: текст
        OHK_DRAWTEXTEX,
        OHK_COUNT
    };

    inline const HookDispatcherDependency<OHK_COUNT, 3> g_overallHookDep
    { {
        // OHK_DRAWTHEMEBG
        std::tuple{ std::array{ "uxtheme.dll"sv, "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv, ""sv },
            "DrawThemeBackground", reinterpret_cast<PVOID>(Overall_MyDrawThemeBg) },
        // OHK_DRAWTHEMETEXTEX
        std::tuple{ std::array{ "uxtheme.dll"sv, "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv, ""sv },
            "DrawThemeTextEx", reinterpret_cast<PVOID>(Overall_MyDrawThemeTextEx) },
        // OHK_EXTTEXTOUT
        std::tuple{ std::array{ "gdi32.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
            "ExtTextOutW", reinterpret_cast<PVOID>(Overall_MyExtTextOutW) },
        // OHK_DRAWTEXT
        std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
            "DrawTextW", reinterpret_cast<PVOID>(Overall_MyDrawTextW) },
        std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
            "DrawTextExW", reinterpret_cast<PVOID>(Overall_MyDrawTextExW) },
    } };

    using OverallDispatcher = HookDispatcher<OHK_COUNT, 3>;

    static std::unordered_map<void*, OverallDispatcher> g_overallMap;
    static wil::srwlock g_overallLock;

    // ── EnableHooksOnModule ──────────────────────────────────────────────────
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

    // ── DisableAllOverallHooks ───────────────────────────────────────────────
    static void DisableAllOverallHooks()
    {
        auto lock = g_overallLock.lock_exclusive();
        for (auto& [addr, dispatcher] : g_overallMap)
            dispatcher.DisableAllHooks();
        g_overallMap.clear();
    }

    // ── GetCallerModule ──────────────────────────────────────────────────────
    static HMODULE GetCallerModule(void* returnAddress)
    {
        HMODULE hMod = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(returnAddress),
            &hMod);
        return hMod;
    }

    // ── GetOverallOrigSafe ───────────────────────────────────────────────────
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

    // ── DrawThemeBackground ──────────────────────────────────────────────────
    // Тема "Header" (iPartId 0-1, iStateId 1 или 4): PatBlt(BLACKNESS) — прозрачный фон.
    // BWM pattern: WinHooks.cpp lines 1306-1312.
    int WINAPI Overall_MyDrawThemeBg(
        HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
        LPCRECT pRect, LPCRECT pClipRect)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_DRAWTHEMEBG, decltype(&DrawThemeBackground)>(callerMod);

        if (!orig)
            return DrawThemeBackground(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

        if (g_headerStack.empty())
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

        WCHAR themeClass[MAX_PATH + 1]{};

        if (SUCCEEDED(Shared::ThemeHelper::GetThemeClass(hTheme, themeClass, MAX_PATH)))
        {
            DebugLog(DBG_NORMAL, L"[GlassCore][Header] Overall_MyDrawThemeBg: caller=%p themeClass=\"%ls\" part=%d state=%d",
                callerMod, themeClass, iPartId, iStateId);

            if (StrEq(themeClass, L"Header")
                && (iPartId == 0 || iPartId == 1)
                && (iStateId == 1 || iStateId == 4)
                && pRect && !IsRectEmpty(pRect))
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

    // ── DrawThemeTextEx ──────────────────────────────────────────────────────
    // Тематизированный Header рисует текст через DrawThemeTextEx.
    // DTT_CALCRECT: прямой orig. Иначе InkGlow + DTT_COMPOSITED + всегда DrawThemeContent (как ListView).
    HRESULT WINAPI Overall_MyDrawThemeTextEx(
        HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
        LPCWSTR pszText, int cchText, DWORD dwTextFlags,
        LPRECT pRect, const DTTOPTS* pOptions)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_DRAWTHEMETEXTEX, decltype(&DrawThemeTextEx)>(callerMod);

        if (!orig)
            return DrawThemeTextEx(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions);

        if (g_headerStack.empty())
            return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions);

        if (!pRect || IsRectEmpty(pRect))
            return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions);

        const DTTOPTS* effectiveOpts = pOptions;
        DTTOPTS defaultOpts{ sizeof(DTTOPTS) };
        if (!effectiveOpts)
            effectiveOpts = &defaultOpts;

        DebugLog(DBG_NORMAL,
            L"[GlassCore][Header] DrawThemeTextEx: part=%d state=%d flags=0x%X "
            L"dttFlags=0x%X composited=%d calcrect=%d rect={%d,%d,%d,%d}",
            iPartId, iStateId, dwTextFlags,
            effectiveOpts->dwFlags,
            !!(effectiveOpts->dwFlags & DTT_COMPOSITED),
            !!(effectiveOpts->dwFlags & DTT_CALCRECT),
            pRect->left, pRect->top, pRect->right, pRect->bottom);

        if (effectiveOpts->dwFlags & DTT_CALCRECT)
        {
            DTTOPTS measure = *effectiveOpts;
            if (measure.dwSize < sizeof(DTTOPTS))
                measure.dwSize = sizeof(DTTOPTS);
            return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, &measure);
        }

        DTTOPTS work = *effectiveOpts;
        if (work.dwSize < sizeof(DTTOPTS))
            work.dwSize = sizeof(DTTOPTS);
        work.dwFlags |= DTT_COMPOSITED;

        return GlassCore::Text::DrawThemeContent(
            hdc, *pRect, nullptr, nullptr, 0,
            [&](HDC memoryDC, HPAINTBUFFER, RGBQUAD*, int)
            {
                orig(hTheme, memoryDC, iPartId, iStateId,
                     pszText, cchText, dwTextFlags, pRect, &work);
            });
    }

    // ── ExtTextOutW ──────────────────────────────────────────────────────────
    // Unthemed Header: ETO_OPAQUE → PatBlt(BLACKNESS).
    // BWM pattern: Header::WorkExtTextOutW (WinHooks.cpp 952-964).
    BOOL WINAPI Overall_MyExtTextOutW(
        HDC hdc, int x, int y, UINT options, const RECT* lprect,
        LPCWSTR lpString, UINT c, const INT* lpDx)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_EXTTEXTOUT, decltype(&ExtTextOutW)>(callerMod);

        if (!orig)
            return ExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);

        if (g_headerStack.empty())
            return orig(hdc, x, y, options, lprect, lpString, c, lpDx);

        // BWM Header::WorkExtTextOutW: ETO_OPAQUE без темы → PatBlt(BLACKNESS)
        if (options == ETO_OPAQUE && lprect)
        {
            HWND hWnd = WindowFromDC(hdc);
            if (hWnd && !GetWindowTheme(hWnd))
            {
                PatBlt(hdc, lprect->left, lprect->top,
                       lprect->right - lprect->left, lprect->bottom - lprect->top,
                       BLACKNESS);
                return TRUE;
            }
        }

        // Текст: рисуем через DrawTextWithAlpha для корректного alpha-рендеринга
        if (c && lpString && !(options & ETO_GLYPH_INDEX))
        {
            // Очищаем фон если нужно
            if ((options & ETO_OPAQUE) && lprect)
            {
                PatBlt(hdc, lprect->left, lprect->top,
                       lprect->right - lprect->left, lprect->bottom - lprect->top,
                       BLACKNESS);
            }

            RECT clip{};
            GetClipBox(hdc, &clip);
            clip.left = x;
            clip.top  = y;

            int result = 0;
            if (!FAILED(GlassCore::Text::DrawTextWithAlpha(
                    hdc, lpString, static_cast<int>(c), &clip,
                    DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOCLIP, result)))
                return TRUE;
        }

        return orig(hdc, x, y, options, lprect, lpString, c, lpDx);
    }

    // ── DrawTextW ────────────────────────────────────────────────────────────
    // Unthemed Header: текст через DrawTextWithAlpha.
    int WINAPI Overall_MyDrawTextW(
        HDC hdc, LPCWSTR lpchText, int cchText, LPRECT lprc, UINT format)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_DRAWTEXT, decltype(&DrawTextW)>(callerMod);

        if (!orig)
            return DrawTextW(hdc, lpchText, cchText, lprc, format);

        if (g_headerStack.empty())
            return orig(hdc, lpchText, cchText, lprc, format);

        if (format & DT_CALCRECT)
            return orig(hdc, lpchText, cchText, lprc, format);

        if (!lprc || IsRectEmpty(lprc) || !lpchText || cchText == 0)
            return orig(hdc, lpchText, cchText, lprc, format);

        if (GetBkMode(hdc) == OPAQUE)
        {
            PatBlt(hdc, lprc->left, lprc->top,
                   lprc->right - lprc->left, lprc->bottom - lprc->top, BLACKNESS);
        }

        int result = 0;
        if (!FAILED(GlassCore::Text::DrawTextWithAlpha(hdc, lpchText, cchText, lprc, format, result)))
            return result;

        return orig(hdc, lpchText, cchText, lprc, format);
    }

    int WINAPI Overall_MyDrawTextExW(
        HDC hdc, LPWSTR lpchText, int cchText,
        LPRECT lprc, UINT format, LPDRAWTEXTPARAMS lpdtp)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_DRAWTEXTEX, decltype(&DrawTextExW)>(callerMod);

        if (!orig)
            return DrawTextExW(hdc, lpchText, cchText, lprc, format, lpdtp);

        if (lpdtp)
            return orig(hdc, lpchText, cchText, lprc, format, lpdtp);
        
        int result = 0;
        if (!FAILED(GlassCore::Text::DrawTextWithAlpha(hdc, lpchText, cchText, lprc, format, result)))
            return result;
        return orig(hdc, lpchText, cchText, lprc, format, lpdtp);
    }

    // =========================================================================
    //  PaintDispatcher provider callbacks
    // =========================================================================

    static void OnBeginPaintGlobal(HWND hWnd, HMODULE callerMod)
    {
        using namespace GlassShared;
        if (GlassGetWindowKind(hWnd) != GlassControlKind::Header) return;
        if (g_shutdown.load(std::memory_order_acquire)) return;

        GlassCore::TextGlow::EnsureTextGlowClientRegistered(hWnd);

        auto* g_new    = GlassCore::ModuleHandler::g_comctl32New;
        auto* g_legacy = GlassCore::ModuleHandler::g_comctl32Legacy;

        HeaderSessionKind sk;
        if (callerMod == g_new)
            sk = Shared::WinApiUtils::IsThemed(hWnd, L"Header")
                ? HeaderSessionKind::NewThemed
                : HeaderSessionKind::NewUnthemed;
        else if (callerMod == g_legacy)
            sk = HeaderSessionKind::Legacy;
        else
            return;

        g_headerStack.push_back({ callerMod, sk });
        
        DebugLog(DBG_NORMAL, L"[GlassCore][Header] BeginPaint: callerMod=0x%p hWnd=0x%p depth=%zu",
            callerMod, hWnd, g_headerStack.size());

        // BWM PaintHandler.cpp:218-238 — разветвление по callerModule и IsTheme
        if (callerMod == g_new)
        {
            if (sk == HeaderSessionKind::NewThemed)
            {
                EnableHooksOnModule(g_new, OHK_DRAWTHEMEBG, true);
                const bool glow = GlassCore::Text::IsInkGlowEffectActiveForPaintSession(hWnd);
                HMODULE hUx = GetModuleHandleW(L"uxtheme.dll");

                if (!glow || (!hUx && glow))
                    EnableHooksOnModule(g_new, OHK_DRAWTHEMETEXTEX, true);
                else
                    EnableHooksOnModule(hUx, OHK_DRAWTEXTEX, true);
            }
            else
            {
                EnableHooksOnModule(g_new, OHK_EXTTEXTOUT, true);
                EnableHooksOnModule(g_new, OHK_DRAWTEXT, true);
            }
        }
        else
            EnableHooksOnModule(g_legacy, OHK_EXTTEXTOUT, true);
    }

    static void OnEndPaintGlobal(HWND hWnd, HMODULE /*callerMod*/)
    {
        using namespace GlassShared;
        if (GlassGetWindowKind(hWnd) != GlassControlKind::Header) return;
        if (g_headerStack.empty()) return;

        auto* g_new    = GlassCore::ModuleHandler::g_comctl32New;
        auto* g_legacy = GlassCore::ModuleHandler::g_comctl32Legacy;

        const HeaderPaintSession s = g_headerStack.back();
        g_headerStack.pop_back();

        if (s.mod == g_new)
        {
            if (s.kind == HeaderSessionKind::NewThemed)
            {
                EnableHooksOnModule(g_new, OHK_DRAWTHEMEBG, false);
                const bool glow = GlassCore::Text::IsInkGlowEffectActiveForPaintSession(hWnd);
                HMODULE hUx = GetModuleHandleW(L"uxtheme.dll");

                if (!glow || (!hUx && glow))
                    EnableHooksOnModule(g_new, OHK_DRAWTHEMETEXTEX, false);
                else
                    EnableHooksOnModule(hUx, OHK_DRAWTEXTEX, false);
            }
            else
            {
                EnableHooksOnModule(g_new, OHK_EXTTEXTOUT, false);
                EnableHooksOnModule(g_new, OHK_DRAWTEXT, false);
            }
        }
        else if (s.mod == g_legacy)
            EnableHooksOnModule(g_legacy, OHK_EXTTEXTOUT, false);

        DebugLog(DBG_NORMAL, L"[GlassCore][Header] EndPaint: hWnd=0x%p depth=%zu", hWnd, g_headerStack.size());
    }

    // ── Post BeginPaint: безусловное стирание фона ────────────────────────────
    // Аналог BWM erase=true для SysHeader32 (WndList::MyBeginPaint делает
    // PatBlt(rcPaint, BLACKNESS) после BeginPaint): чистим всю зону rcPaint,
    // включая пустую область справа от колонок, которую item-хуки не покрывают.
    [[nodiscard]] static bool OnHeaderAfterBeginPaint(
        HWND hWnd, HDC hdc, LPPAINTSTRUCT ps, HMODULE /*callerMod*/) noexcept
    {
        if (g_shutdown.load(std::memory_order_acquire))
            return false;

        using namespace GlassShared;
        if (GlassGetWindowKind(hWnd) != GlassControlKind::Header)
            return false;

        if (!hdc || !ps || IsRectEmpty(&ps->rcPaint))
            return false;

        PatBlt(hdc,
            ps->rcPaint.left, ps->rcPaint.top,
            ps->rcPaint.right - ps->rcPaint.left,
            ps->rcPaint.bottom - ps->rcPaint.top,
            BLACKNESS);
        return true;
    }

    // =========================================================================
    //  Lifecycle
    // =========================================================================

    void Prepare()  {}

    void Startup()
    {
        GlassCore::PaintDispatcher::RegisterPaintProvider({ OnBeginPaintGlobal, OnEndPaintGlobal });
        GlassCore::PaintDispatcher::RegisterPostPaintProvider({ OnHeaderAfterBeginPaint });

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
                DebugLog(DBG_NORMAL, L"[GlassCore][Header] Startup: cached 0x%p", mod);
            }
        };
        cacheModule(GlassCore::ModuleHandler::g_comctl32New);
        cacheModule(GlassCore::ModuleHandler::g_comctl32Legacy);
        cacheModule(GlassCore::ModuleHandler::g_entryPointModule);
    }

    void Shutdown()
    {
        g_shutdown.store(true, std::memory_order_seq_cst);
        DisableAllOverallHooks();
        g_headerStack.clear();
        g_shutdown.store(false, std::memory_order_seq_cst);
    }
}
