// Хуки для отрисовки Static-контролов.
//
// Static / .STATIC; USER32 без темы → ExtTextOut на gdi32full.

#include "CorePch.h"

#include <Hooks/Static.h>
#include <Hooks/forms/FormsHooks.h>
#include <Subsystems/RenderText.h>
#include <Core/Intercept/HookDispatcher.h>
#include <Module/ModuleHandler.h>
#include <Subsystems/PaintDispatcher.h>
#include <GlassControlKind.h>
#include <wil.h>
#include <WinApiUtils.h>
#include <string>
#include <vector>

using namespace GlassCore::Hooks;
using namespace std::literals;

namespace GlassCore::StaticControl
{
    // =========================================================================
    //  Сессия paint (Static / .STATIC)
    // =========================================================================

    enum class StaticPaintBranch : uint8_t
    {
        None,
        WinFormsDot,                 // .STATIC: DrawText на callerMod
        ThemedFull,                  // IsTheme(Static): DrawTextW / TextOut / ExtTextOut — glow через общие пути, без DTT/DTB
        ExtTextOutGdi32,             // иначе: gdi32full ExtTextOutW
    };

    struct StaticPaintSession
    {
        HMODULE           mod{};
        StaticPaintBranch branch{ StaticPaintBranch::None };
        bool              useSsSimpleTextOut{};
        bool              callerIsFormsNi{};
    };

    struct StaticPaintStackEntry
    {
        HWND              hWnd{};
        HMODULE           callerMod{};
        StaticPaintSession session{};
    };

    // Стек сессий: BeginPaint / GetDC / CreateCompatibleDC (как ButtonHooks).
    thread_local std::vector<StaticPaintStackEntry> g_staticSessionStack;

    static std::atomic<bool> g_shutdown{ false };

    [[nodiscard]] static bool IsStaticControlKind(GlassShared::GlassControlKind k) noexcept
    {
        using GK = GlassShared::GlassControlKind;
        return k == GK::Static || k == GK::WinForms_Static;
    }

    [[nodiscard]] static bool IsStaticPaintSessionActive() noexcept
    {
        return !g_staticSessionStack.empty();
    }

    [[nodiscard]] static const StaticPaintSession& ActiveStaticSession() noexcept
    {
        static const StaticPaintSession kEmpty{};
        return g_staticSessionStack.empty() ? kEmpty : g_staticSessionStack.back().session;
    }

    static bool StrEq(LPCWSTR a, LPCWSTR b)
    {
        return _wcsicmp(a, b) == 0;
    }

    static bool BaseNameEq(const std::wstring& base, LPCWSTR expect)
    {
        return _wcsicmp(base.c_str(), expect) == 0;
    }

    // =========================================================================
    //  OVERALL DISPATCHER MAP
    // =========================================================================

    int      WINAPI Overall_MyDrawTextW(HDC, LPCWSTR, int, LPRECT, UINT);
    int      WINAPI Overall_MyDrawTextExW(HDC, LPWSTR, int, LPRECT, UINT, LPDRAWTEXTPARAMS);
    BOOL     WINAPI Overall_MyTextOutW(HDC, int, int, LPCWSTR, int);
    BOOL     WINAPI Overall_MyExtTextOutW(HDC, int, int, UINT, const RECT*, LPCWSTR, UINT, const INT*);
    HBRUSH   WINAPI Overall_MyGetSysColorBrush(int);
    HBRUSH   WINAPI Overall_MyCreateSolidBrush(COLORREF);

    enum OverallHookIndex : size_t
    {
        OHK_DRAWTEXT = 0,
        OHK_DRAWTEXTEX,
        OHK_TEXTOUT,
        OHK_EXTTEXTOUT,
        OHK_GETSYSCOLORBR,
        OHK_CREATESOLIDBRUSH,
        OHK_COUNT
    };

    inline const HookDispatcherDependency<OHK_COUNT, 3> g_overallHookDep
    { {
        std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
            "DrawTextW", reinterpret_cast<PVOID>(Overall_MyDrawTextW) },
        std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
            "DrawTextExW", reinterpret_cast<PVOID>(Overall_MyDrawTextExW) },
        std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, ""sv },
            "TextOutW", reinterpret_cast<PVOID>(Overall_MyTextOutW) },
        std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv },
            "ExtTextOutW", reinterpret_cast<PVOID>(Overall_MyExtTextOutW) },
        std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
            "GetSysColorBrush", reinterpret_cast<PVOID>(Overall_MyGetSysColorBrush) },
        std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv },
            "CreateSolidBrush", reinterpret_cast<PVOID>(Overall_MyCreateSolidBrush) },
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



    [[nodiscard]] static void* ResolveGdiModuleForExtTextOut() noexcept
    {
        if (void* p = GetModuleHandleW(L"gdi32full.dll")) return p;
        return GetModuleHandleW(L"gdi32.dll");
    }

    [[nodiscard]] static bool IsSystemWindowsFormsNiModule(HMODULE mod) noexcept
    {
        if (!mod)
            return false;
        return BaseNameEq(Shared::WinApiUtils::GetModuleFileBaseName(mod), L"System.Windows.Forms.ni.dll");
    }

    // Themed static / WinForms .STATIC: IAT на comctl32 (не на caller BeginPaint/user32).
    [[nodiscard]] static HMODULE ResolveStaticComctlHookModule(HMODULE callerMod) noexcept
    {
        auto* g_new = GlassCore::ModuleHandler::g_comctl32New;
        auto* g_legacy = GlassCore::ModuleHandler::g_comctl32Legacy;
        if (callerMod == g_new || callerMod == g_legacy)
            return callerMod;
        return g_new ? g_new : g_legacy;
    }

    // Themed static: IAT на comctl32 для DrawTextW/TextOut (без перехвата uxtheme DTT/DTB — glow через DrawTextWithAlpha / цепочку текста).
    [[nodiscard]] static HMODULE ResolveStaticThemedHookModule(HMODULE callerMod) noexcept
    {
        return ResolveStaticComctlHookModule(callerMod);
    }

    [[nodiscard]] static StaticPaintSession ComputeStaticPaintSession(HWND hWnd, HMODULE callerMod)
    {
        StaticPaintSession s{};
        s.mod = callerMod;

        using namespace GlassShared;
        const GlassControlKind kind = GlassGetWindowKind(hWnd);

        WCHAR cls[MAX_PATH + 1]{};
        GetClassNameW(hWnd, cls, MAX_PATH);
        const UINT  style = static_cast<UINT>(GetWindowLongW(hWnd, GWL_STYLE));
        const bool  ssSimple = !!(style & SS_SIMPLE);

        if (StrEq(cls, L".STATIC") || kind == GlassControlKind::WinForms_Static)
        {
            s.branch = StaticPaintBranch::WinFormsDot;
            s.callerIsFormsNi = IsSystemWindowsFormsNiModule(callerMod);
            if (!s.callerIsFormsNi)
            {
                if (HMODULE const hookMod = ResolveStaticComctlHookModule(callerMod))
                    s.mod = hookMod;
            }
            return s;
        }

        const std::wstring base = Shared::WinApiUtils::GetModuleFileBaseName(callerMod);
        const bool winHasTheme = (GetWindowTheme(hWnd) != nullptr);
        const bool staticUxThemed = Shared::WinApiUtils::IsThemed(hWnd, L"Static")
            || (BaseNameEq(base, L"COMCTL32.dll") && winHasTheme);
        const bool nativeStaticClass = StrEq(cls, L"Static");

        if (nativeStaticClass && BaseNameEq(base, L"USER32.dll") && !winHasTheme)
        {
            s.branch = StaticPaintBranch::ExtTextOutGdi32;
            return s;
        }

        if (staticUxThemed)
        {
            s.branch = StaticPaintBranch::ThemedFull;
            s.useSsSimpleTextOut = nativeStaticClass && ssSimple;
            if (HMODULE const r = ResolveStaticThemedHookModule(callerMod))
                s.mod = r;
            return s;
        }

        s.branch = StaticPaintBranch::ExtTextOutGdi32;
        return s;
    }

    // =========================================================================
    //  ApplyStaticPaintSession — включить/выключить хуки по ветке сессии
    //
  //  WinFormsDot (.STATIC):
  //    caller=forms.ni: только DrawTextEx (глобальный Detours)
  //    caller=comctl32: DrawTextW (IAT) + DrawTextEx если forms.ni загружен
    //
    //  ThemedFull:
    //    callerMod: DrawTextW (IAT)
    //    callerMod: TextOutW (IAT) — только при SS_SIMPLE
    //    inetcpl.cpl: DrawTextW, GetSysColorBrush, CreateSolidBrush (IAT)
    //    System.Windows.Forms.ni.dll: DrawTextExW (глобальный Detours-хук)
    //    sysmon.ocx: CreateSolidBrush (IAT)
    //    g_entryPointModule: DrawTextW (IAT)
    //
    //  ExtTextOutGdi32 (USER32 без темы, прочее):
    //    gdi32full/gdi32: ExtTextOutW (IAT)
    // =========================================================================

    static void ApplyStaticPaintSession(const StaticPaintSession& s, bool enable)
    {
        void* const m = static_cast<void*>(s.mod);
        switch (s.branch)
        {
        case StaticPaintBranch::None:
            break;

        case StaticPaintBranch::WinFormsDot:
            // ── .STATIC (WinForms): без DTB — фон через FormsHooks + DrawText/DrawTextEx ──
            if (!s.callerIsFormsNi && m)
                EnableHooksOnModule(m, OHK_DRAWTEXT, enable);
            if (GetModuleHandleW(L"System.Windows.Forms.ni.dll"))
                GlassCore::FormsHooks::DrawTextExAttach(enable);
            break;

        case StaticPaintBranch::ThemedFull:
            // ── Static с темой: только DrawTextW/TextOut (без DTT/DTB; glow уже общими путями)
            if (m)
            {
                EnableHooksOnModule(m, OHK_DRAWTEXT, enable);
                if (s.useSsSimpleTextOut)
                    EnableHooksOnModule(m, OHK_TEXTOUT, enable);
            }
            // Дополнительные модули
            if (void* inetcpl = GetModuleHandleW(L"inetcpl.cpl"))
            {
                EnableHooksOnModule(inetcpl, OHK_DRAWTEXT, enable);
                EnableHooksOnModule(inetcpl, OHK_GETSYSCOLORBR, enable);
                EnableHooksOnModule(inetcpl, OHK_CREATESOLIDBRUSH, enable);
            }
            // DrawTextExW из System.Windows.Forms.ni.dll — глобальный Detours-хук
            if (GetModuleHandleW(L"System.Windows.Forms.ni.dll"))
                GlassCore::FormsHooks::DrawTextExAttach(enable);
            if (void* sysmon = GetModuleHandleW(L"sysmon.ocx"))
                EnableHooksOnModule(sysmon, OHK_CREATESOLIDBRUSH, enable);
            if (GlassCore::ModuleHandler::g_entryPointModule)
                EnableHooksOnModule(GlassCore::ModuleHandler::g_entryPointModule, OHK_DRAWTEXT, enable);
            break;

        case StaticPaintBranch::ExtTextOutGdi32:
            // ── USER32 без темы, прочее ───────────────────────────────────
            if (void* gdi = ResolveGdiModuleForExtTextOut())
                EnableHooksOnModule(gdi, OHK_EXTTEXTOUT, enable);
            break;
        }
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

    int WINAPI Overall_MyDrawTextW(
        HDC hdc, LPCWSTR lpchText, int cchText, LPRECT lprc, UINT format)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_DRAWTEXT, decltype(&DrawTextW)>(callerMod);

        if (!orig)
            return DrawTextW(hdc, lpchText, cchText, lprc, format);

        if (!IsStaticPaintSessionActive())
            return orig(hdc, lpchText, cchText, lprc, format);

        if (format & DT_CALCRECT)
            return orig(hdc, lpchText, cchText, lprc, format);

        if (!lprc || IsRectEmpty(lprc) || !lpchText || cchText == 0)
            return orig(hdc, lpchText, cchText, lprc, format);

        DebugLog(DBG_NORMAL, L"[GlassCore][Static] DrawTextW: flags=0x%x bkMode=%d caller=0x%p",
            format, GetBkMode(hdc), callerMod);

        // BWM OverallHooks::MyDrawTextW (WinFormsDot/comctl32): OPAQUE — PatBlt или FillRect только для HIGHLIGHT.
        // BWM WinHooks::MyDrawTextW (ThemedFull): PatBlt для enabled + опциональный FillRect по GetBkColor.
        if (GetBkMode(hdc) == OPAQUE)
        {
            const bool winFormsDot =
                IsStaticPaintSessionActive()
                && ActiveStaticSession().branch == StaticPaintBranch::WinFormsDot;

            if (winFormsDot)
            {
                const COLORREF color = GetBkColor(hdc);
                if (color == GetSysColor(COLOR_HIGHLIGHT))
                {
                    wil::unique_hbrush brush{ Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(color, 255) };
                    if (brush)
                        FillRect(hdc, lprc, brush.get());
                }
                else
                {
                    PatBlt(hdc, lprc->left, lprc->top,
                        lprc->right - lprc->left, lprc->bottom - lprc->top, BLACKNESS);
                }
            }
            else
            {
                HWND const hWnd = WindowFromDC(hdc);
                if (hWnd && IsWindowEnabled(hWnd))
                {
                    PatBlt(hdc, lprc->left, lprc->top,
                        lprc->right - lprc->left, lprc->bottom - lprc->top, BLACKNESS);
                }

                const COLORREF color = GetBkColor(hdc);
                bool skipOpaqueFill = !color || color == GetSysColor(COLOR_WINDOW);
                if (!skipOpaqueFill && color == GetSysColor(COLOR_BTNFACE) && hWnd)
                {
                    WCHAR cn[256]{};
                    if (GetClassNameW(hWnd, cn, static_cast<int>(std::size(cn))) > 0
                        && StrEq(cn, L"SysHeader32") && !GetWindowTheme(hWnd))
                        skipOpaqueFill = true;
                }
                if (!skipOpaqueFill)
                {
                    wil::unique_hbrush brush{ Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(color, 255) };
                    if (brush)
                        FillRect(hdc, lprc, brush.get());
                }
            }
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

        if (!IsStaticPaintSessionActive())
            return orig(hdc, lpchText, cchText, lprc, format, lpdtp);

        if (format & DT_CALCRECT)
            return orig(hdc, lpchText, cchText, lprc, format, lpdtp);

        if (!lprc || IsRectEmpty(lprc))
            return orig(hdc, lpchText, cchText, lprc, format, lpdtp);

        DebugLog(DBG_NORMAL, L"[GlassCore][Static] DrawTextExW: flags=0x%x caller=0x%p", format, callerMod);

        int result = 0;
        if (!FAILED(GlassCore::Text::DrawTextWithAlpha(hdc, lpchText, cchText, lprc, format, result)))
            return result;

        return orig(hdc, lpchText, cchText, lprc, format, lpdtp);
    }

    BOOL WINAPI Overall_MyTextOutW(
        HDC hdc, int x, int y, LPCWSTR lpString, int c)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_TEXTOUT, decltype(&TextOutW)>(callerMod);

        if (!orig)
            return TextOutW(hdc, x, y, lpString, c);

        if (!IsStaticPaintSessionActive())
            return orig(hdc, x, y, lpString, c);

        if (!lpString || c == 0)
            return orig(hdc, x, y, lpString, c);

        DebugLog(DBG_NORMAL, L"[GlassCore][Static] TextOutW: x=%d y=%d c=%d caller=0x%p", x, y, c, callerMod);

        SIZE sz{};
        if (!GetTextExtentPoint32W(hdc, lpString, c, &sz))
            return orig(hdc, x, y, lpString, c);

        RECT rc{ x, y, x + sz.cx, y + sz.cy };

        int result = 0;
        if (!FAILED(GlassCore::Text::DrawTextWithAlpha(
            hdc, lpString, c, &rc,
            DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOCLIP, result)))
            return TRUE;

        return orig(hdc, x, y, lpString, c);
    }

    HBRUSH WINAPI Overall_MyGetSysColorBrush(int nIndex)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_GETSYSCOLORBR, decltype(&GetSysColorBrush)>(callerMod);

        if (IsStaticPaintSessionActive())
        {
            if (nIndex == COLOR_BTNFACE || nIndex == COLOR_WINDOW)
                return static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        }

        if (!orig)
            return GetSysColorBrush(nIndex);
        return orig(nIndex);
    }

    HBRUSH WINAPI Overall_MyCreateSolidBrush(COLORREF color)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_CREATESOLIDBRUSH, decltype(&CreateSolidBrush)>(callerMod);

        if (!orig)
            return CreateSolidBrush(color);

        if (!IsStaticPaintSessionActive())
            return orig(color);

        if (color == 0)
            return orig(0);

        HBRUSH alphaBrush = Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(color, 255);
        return alphaBrush ? alphaBrush : orig(color);
    }

    BOOL WINAPI Overall_MyExtTextOutW(
        HDC hdc, int x, int y, UINT options, const RECT* lprect,
        LPCWSTR lpString, UINT c, const INT* lpDx)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_EXTTEXTOUT, decltype(&ExtTextOutW)>(callerMod);

        if (!orig)
            return ExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);

        if (!IsStaticPaintSessionActive())
            return orig(hdc, x, y, options, lprect, lpString, c, lpDx);

        if ((options & ETO_OPAQUE) && lprect)
        {
            PatBlt(hdc, lprect->left, lprect->top,
                lprect->right - lprect->left, lprect->bottom - lprect->top,
                BLACKNESS);

            if (!c) return TRUE;

            RECT rc{ x, y, lprect->right, lprect->bottom };
            DWORD flags = DT_EXPANDTABS | DT_NOPREFIX;
            if (options & ETO_RTLREADING) flags |= DT_RTLREADING;

            int result = 0;
            if (!FAILED(GlassCore::Text::DrawTextWithAlpha(hdc, lpString, c, &rc, flags, result)))
                return result;
        }

        return orig(hdc, x, y, options, lprect, lpString, c, lpDx);
    }

    // =========================================================================
    //  PaintDispatcher provider callbacks
    // =========================================================================

    static void PushStaticPaintSession(HWND hWnd, HMODULE callerMod, const wchar_t* reason)
    {
        using namespace GlassShared;
        if (g_shutdown.load(std::memory_order_acquire))
            return;
        if (!IsStaticControlKind(GlassGetWindowKind(hWnd)))
            return;

        const bool wasEmpty = g_staticSessionStack.empty();
        const StaticPaintSession session = ComputeStaticPaintSession(hWnd, callerMod);
        g_staticSessionStack.push_back({ hWnd, callerMod, session });
        if (wasEmpty)
            ApplyStaticPaintSession(session, true);

        DebugLog(DBG_NORMAL,
            L"[GlassCore][Static] %s: callerMod=0x%p hWnd=0x%p branch=%u depth=%zu",
            reason, callerMod, hWnd, static_cast<unsigned>(session.branch), g_staticSessionStack.size());
    }

    static void PopStaticPaintSession(HWND hWnd, const wchar_t* reason)
    {
        using namespace GlassShared;
        if (!IsStaticControlKind(GlassGetWindowKind(hWnd)))
            return;
        if (g_staticSessionStack.empty())
            return;

        const StaticPaintStackEntry entry = g_staticSessionStack.back();
        g_staticSessionStack.pop_back();
        if (g_staticSessionStack.empty())
            ApplyStaticPaintSession(entry.session, false);

        DebugLog(DBG_NORMAL,
            L"[GlassCore][Static] %s: hWnd=0x%p depth=%zu",
            reason, hWnd, g_staticSessionStack.size());
    }

    static void StaticBeginSession(HWND hWnd, HMODULE callerMod)
    {
        PushStaticPaintSession(hWnd, callerMod, L"BeginSession");
    }

    static void StaticEndSession(HWND hWnd, const wchar_t* reason, bool drainSameHwnd)
    {
        PopStaticPaintSession(hWnd, reason);
        if (!drainSameHwnd)
            return;

        while (!g_staticSessionStack.empty() && g_staticSessionStack.back().hWnd == hWnd)
            PopStaticPaintSession(hWnd, L"EndPaintDrain");
    }

    static void OnBeginPaintGlobal(HWND hWnd, HMODULE callerMod)
    {
        StaticBeginSession(hWnd, callerMod);
    }

    static void OnEndPaintGlobal(HWND hWnd, HMODULE /*callerMod*/)
    {
        StaticEndSession(hWnd, L"EndPaint", true);
    }

    static void StaticOnDcLikeSessionBegin(HWND hWnd, HMODULE callerMod)
    {
        PushStaticPaintSession(hWnd, callerMod, L"DcLike Begin");
    }

    static void StaticOnDcLikeSessionEnd(HWND hWnd, HMODULE /*callerMod*/)
    {
        // Как Button: kind-gate + silent empty внутри Pop (контракт PaintDispatcher —
        // early-return Begin ⇒ no-op End). WARN до kind заливал лог в PopupOnly.
        PopStaticPaintSession(hWnd, L"DcLike End");
    }

    static void OnStaticGetDCBegin(HWND hWnd, HMODULE callerMod)
    {
        StaticOnDcLikeSessionBegin(hWnd, callerMod);
    }

    static void OnStaticGetDCEnd(HWND hWnd, HMODULE callerMod)
    {
        StaticOnDcLikeSessionEnd(hWnd, callerMod);
    }

    static void OnStaticCompatBegin(HWND hWnd, HMODULE callerMod)
    {
        StaticOnDcLikeSessionBegin(hWnd, callerMod);
    }

    static void OnStaticCompatEnd(HWND hWnd, HMODULE callerMod)
    {
        StaticOnDcLikeSessionEnd(hWnd, callerMod);
    }

    void Prepare() {}

    void Startup()
    {
        GlassCore::PaintDispatcher::RegisterPaintProvider({ OnBeginPaintGlobal, OnEndPaintGlobal });
        GlassCore::PaintDispatcher::RegisterGetDCProvider({ OnStaticGetDCBegin, OnStaticGetDCEnd });
        GlassCore::PaintDispatcher::RegisterCompatDCProvider({ OnStaticCompatBegin, OnStaticCompatEnd });

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
                    DebugLog(DBG_NORMAL, L"[GlassCore][Static] Startup: cached 0x%p", mod);
                }
            };
        cacheModule(GlassCore::ModuleHandler::g_comctl32New);
        cacheModule(GlassCore::ModuleHandler::g_comctl32Legacy);
        cacheModule(GlassCore::ModuleHandler::g_entryPointModule);
        if (void* gdi = GetModuleHandleW(L"gdi32full.dll"))
            cacheModule(gdi);
        else if (void* gdi = GetModuleHandleW(L"gdi32.dll"))
            cacheModule(gdi);
        if (void* inetcpl = GetModuleHandleW(L"inetcpl.cpl"))
            cacheModule(inetcpl);
        if (void* sysmon = GetModuleHandleW(L"sysmon.ocx"))
            cacheModule(sysmon);
    }

    bool IsInsideStaticPaint() noexcept
    {
        return IsStaticPaintSessionActive();
    }

    bool IsInsideStaticPaint(HWND hWnd) noexcept
    {
        if (!hWnd)
            return false;
        for (const StaticPaintStackEntry& entry : g_staticSessionStack)
        {
            if (entry.hWnd == hWnd)
                return true;
        }
        return false;
    }

    void Shutdown()
    {
        g_shutdown.store(true, std::memory_order_seq_cst);

        DisableAllOverallHooks();

        while (!g_staticSessionStack.empty())
        {
            const StaticPaintStackEntry entry = g_staticSessionStack.back();
            g_staticSessionStack.pop_back();
            if (g_staticSessionStack.empty())
                ApplyStaticPaintSession(entry.session, false);
        }

        g_shutdown.store(false, std::memory_order_seq_cst);
    }
}