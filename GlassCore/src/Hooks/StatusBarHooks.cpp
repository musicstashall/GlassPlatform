// msctls_statusbar32 — BWM: тема → DrawThemeText + DrawThemeBackground; иначе ExtTextOut.

#include "CorePch.h"
#include <Hooks/StatusBar.h>
#include <Subsystems/RenderText.h>
#include <Helpers/ThemeHelper.h>
#include <Core/Intercept/HookDispatcher.h>
#include <Module/ModuleHandler.h>
#include <Subsystems/PaintDispatcher.h>
#include <GlassControlKind.h>
#include <wil.h>
#include <uxtheme.h>
#include <vector>

using namespace GlassCore::Hooks;
using namespace std::literals;

namespace GlassCore::StatusBarControl
{
    namespace
    {
        enum class SbKind { Themed, Unthemed };
        struct Session { HMODULE mod; SbKind k; };
        thread_local std::vector<Session> g_stack;
        thread_local int     g_statusThemedPaintDepth = 0;

        static std::atomic<bool> g_shutdown{ false };

        static bool StrEq(LPCWSTR a, LPCWSTR b) { return _wcsicmp(a, b) == 0; }

        int      WINAPI Overall_MyDrawThemeBg(HTHEME, HDC, int, int, LPCRECT, LPCRECT);
        HRESULT  WINAPI Overall_MyDrawThemeText(HTHEME, HDC, int, int, LPCWSTR, int, DWORD, DWORD, LPCRECT);
        BOOL     WINAPI Overall_MyExtTextOutW(HDC, int, int, UINT, const RECT*, LPCWSTR, UINT, const INT*);
        int      WINAPI Overall_MyDrawTextExW(HDC, LPWSTR, int, LPRECT, UINT, LPDRAWTEXTPARAMS);

        enum OverallHookIndex : size_t
        {
            OHK_DRAWTHEMEBG = 0,
            OHK_DRAWTHEMETEXT,
            OHK_EXTTEXTOUT,
            OHK_DRAWTEXTEX,
            OHK_COUNT
        };

        inline const HookDispatcherDependency<OHK_COUNT, 3> g_dep
        { {
            std::tuple{ std::array{ "uxtheme.dll"sv, "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv, ""sv },
                "DrawThemeBackground", reinterpret_cast<PVOID>(Overall_MyDrawThemeBg) },
            std::tuple{ std::array{ "uxtheme.dll"sv, "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv, ""sv },
                "DrawThemeText", reinterpret_cast<PVOID>(Overall_MyDrawThemeText) },
            std::tuple{ std::array{ "gdi32.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
                "ExtTextOutW", reinterpret_cast<PVOID>(Overall_MyExtTextOutW) },
            std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
                "DrawTextExW", reinterpret_cast<PVOID>(Overall_MyDrawTextExW) },
        } };

        using OverallDispatcher = HookDispatcher<OHK_COUNT, 3>;
        static std::unordered_map<void*, OverallDispatcher> g_overallMap;
        static wil::srwlock                                                g_overallLock;

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
                    std::forward_as_tuple(g_dep, baseAddress));
                it = inserted;
                it->second.CacheHookData();
            }
            else
                it->second.moduleAddress = baseAddress;
            it->second.EnableHook(index, enable);
            return true;
        }

        static void DisableAllOverallHooks()
        {
            auto lock = g_overallLock.lock_exclusive();
            for (auto& [a, d] : g_overallMap)
                d.DisableAllHooks();
            g_overallMap.clear();
        }

        static HMODULE GetCallerModule(void* ra)
        {
            HMODULE hMod = nullptr;
            GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(ra),
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

        int WINAPI Overall_MyDrawThemeBg(
            HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
            LPCRECT pRect, LPCRECT pClipRect)
        {
            HMODULE callerMod = GetCallerModule(_ReturnAddress());
            auto orig = GetOverallOrigSafe<OHK_DRAWTHEMEBG, decltype(&DrawThemeBackground)>(callerMod);

            if (!orig)
                return DrawThemeBackground(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

            if (g_stack.empty())
                return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

            WCHAR cls[256]{};
            if (SUCCEEDED(Shared::ThemeHelper::GetThemeClass(hTheme, cls, 255))
                && StrEq(cls, L"Status")
                && pRect && !IsRectEmpty(pRect))
            {
                const RECT* cr = pClipRect ? pClipRect : pRect;
                PatBlt(hdc, cr->left, cr->top, cr->right - cr->left, cr->bottom - cr->top, BLACKNESS);
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

            if (g_stack.empty())
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

        BOOL WINAPI Overall_MyExtTextOutW(
            HDC hdc, int x, int y, UINT options, const RECT* lprect,
            LPCWSTR lpString, UINT c, const INT* lpDx)
        {
            HMODULE callerMod = GetCallerModule(_ReturnAddress());
            auto orig = GetOverallOrigSafe<OHK_EXTTEXTOUT, decltype(&ExtTextOutW)>(callerMod);

            if (!orig)
                return ExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);

            if (g_stack.empty())
                return orig(hdc, x, y, options, lprect, lpString, c, lpDx);

            if ((options & ETO_OPAQUE) && lprect)
                PatBlt(hdc, lprect->left, lprect->top,
                    lprect->right - lprect->left, lprect->bottom - lprect->top, BLACKNESS);

            if (c && lpString && !(options & ETO_GLYPH_INDEX))
            {
                RECT clip{};
                GetClipBox(hdc, &clip);
                clip.left = x;
                clip.top = y;
                int result = 0;
                if (!FAILED(GlassCore::Text::DrawTextWithAlpha(
                        hdc, lpString, static_cast<int>(c), &clip,
                        DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOCLIP, result)))
                    return TRUE;
            }
            return orig(hdc, x, y, options, lprect, lpString, c, lpDx);
        }

        int WINAPI Overall_MyDrawTextExW(
            HDC hdc, LPWSTR lpchText, int cchText,
            LPRECT lprc, UINT format, LPDRAWTEXTPARAMS lpdtp)
        {
            HMODULE callerMod = GetCallerModule(_ReturnAddress());
            auto orig = GetOverallOrigSafe<OHK_DRAWTEXTEX, decltype(&DrawTextExW)>(callerMod);

            if (!orig)
                return DrawTextExW(hdc, lpchText, cchText, lprc, format, lpdtp);

            if (g_statusThemedPaintDepth <= 0)
                return orig(hdc, lpchText, cchText, lprc, format, lpdtp);

            if (lpdtp)
                return orig(hdc, lpchText, cchText, lprc, format, lpdtp);
            int result = 0;
            if (!FAILED(GlassCore::Text::DrawTextWithAlpha(hdc, lpchText, cchText, lprc, format, result)))
                return result;
            return orig(hdc, lpchText, cchText, lprc, format, lpdtp);
        }

        static void OnBeginPaintGlobal(HWND hWnd, HMODULE callerMod)
        {
            using namespace GlassShared;
            if (g_shutdown.load(std::memory_order_acquire)) return;
            if (GlassGetWindowKind(hWnd) != GlassControlKind::StatusBar)
                return;

            auto* g_new = GlassCore::ModuleHandler::g_comctl32New;
            auto* g_legacy = GlassCore::ModuleHandler::g_comctl32Legacy;
            if (callerMod != g_new && callerMod != g_legacy)
                return;

            const bool themed = GetWindowTheme(hWnd) != nullptr;
            g_stack.push_back({ callerMod, themed ? SbKind::Themed : SbKind::Unthemed });
            if (themed)
            {
                ++g_statusThemedPaintDepth;
                
                EnableHooksOnModule(callerMod, OHK_DRAWTHEMEBG, true);
                const bool glow = GlassCore::Text::IsInkGlowEffectActiveForPaintSession(hWnd);
                HMODULE hUx = GetModuleHandleW(L"uxtheme.dll");

                if (!glow || (!hUx && glow))
                {
                    EnableHooksOnModule(callerMod, OHK_DRAWTHEMETEXT, true);
                }
                else
                    EnableHooksOnModule(hUx, OHK_DRAWTEXTEX, true);
            }
            else
                EnableHooksOnModule(callerMod, OHK_EXTTEXTOUT, true);
        }

        static void OnEndPaintGlobal(HWND hWnd, HMODULE /*callerMod*/)
        {
            using namespace GlassShared;
            if (GlassGetWindowKind(hWnd) != GlassControlKind::StatusBar)
                return;
            if (g_stack.empty()) return;

            const Session s = g_stack.back();
            
            g_stack.pop_back();

            if (s.k == SbKind::Themed)
            {
                EnableHooksOnModule(s.mod, OHK_DRAWTHEMEBG, false);
                const bool glow = GlassCore::Text::IsInkGlowEffectActiveForPaintSession(hWnd);
                HMODULE hUx = GetModuleHandleW(L"uxtheme.dll");

                if (!glow || (!hUx && glow))
                {
                    EnableHooksOnModule(s.mod, OHK_DRAWTHEMETEXT, false);
                }
                else
                    EnableHooksOnModule(hUx, OHK_DRAWTEXTEX, false);
            }
            else
                EnableHooksOnModule(s.mod, OHK_EXTTEXTOUT, false);
        }
    }

    void Prepare() {}

    void Startup()
    {
        GlassCore::PaintDispatcher::RegisterPaintProvider({ OnBeginPaintGlobal, OnEndPaintGlobal });
        auto cache = [&](void* mod)
            {
                if (!mod) return;
                auto lock = g_overallLock.lock_exclusive();
                auto [it, ins] = g_overallMap.emplace(
                    std::piecewise_construct,
                    std::forward_as_tuple(mod),
                    std::forward_as_tuple(g_dep, mod));
                if (ins)
                    it->second.CacheHookData();
            };
        cache(GlassCore::ModuleHandler::g_comctl32New);
        cache(GlassCore::ModuleHandler::g_comctl32Legacy);
    }

    void Shutdown()
    {
        g_shutdown.store(true, std::memory_order_seq_cst);
        DisableAllOverallHooks();
        g_statusThemedPaintDepth = 0;
        g_stack.clear();
        g_shutdown.store(false, std::memory_order_seq_cst);
    }
}
