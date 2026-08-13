// SysDateTimePick32 — BWM: тема → DrawThemeBackground + DrawThemeText; иначе DrawTextW; всегда ExtTextOutW.

#include "CorePch.h"
#include <Hooks/DateTimePicker.h>
#include <Hooks/forms/DateTimePickerFormsNiDtbAccess.h>
#include <Hooks/forms/FormsHooks.h>
#include <Subsystems/RenderText.h>
#include <Helpers/ThemeHelper.h>
#include <Helpers/WinApiUtils.h>
#include <Core/Intercept/HookDispatcher.h>
#include <Module/ModuleHandler.h>
#include <Subsystems/PaintDispatcher.h>
#include <GlassControlKind.h>
#include <DebugLog.h>
#include <wil.h>
#include <uxtheme.h>
#include <vector>

#include <detours.h>

using namespace GlassCore::Hooks;
using namespace std::literals;

namespace GlassCore::DateTimePickerControl
{
    namespace
    {
        enum class DtpKind { Themed, Unthemed };
        struct Session { HMODULE mod; DtpKind k; };
        thread_local std::vector<Session> g_stack;
        thread_local HWND g_dtpHwnd = nullptr;

        static std::atomic<bool> g_shutdown{ false };

        static bool StrEq(LPCWSTR a, LPCWSTR b) { return _wcsicmp(a, b) == 0; }

        [[nodiscard]] static bool IsDtpSessionKind(GlassShared::GlassControlKind k) noexcept
        {
            using GK = GlassShared::GlassControlKind;
            return k == GK::DateTimePicker || k == GK::WinForms_DateTimePicker;
        }

        [[nodiscard]] static bool DtpSessionActive() noexcept { return !g_stack.empty(); }

        [[nodiscard]] static HMODULE ResolveDefaultComctlModule() noexcept
        {
            auto* g_new = GlassCore::ModuleHandler::g_comctl32New;
            auto* g_legacy = GlassCore::ModuleHandler::g_comctl32Legacy;
            return g_new ? g_new : g_legacy;
        }

        [[nodiscard]] static bool IsSystemWindowsFormsNiModule(HMODULE mod) noexcept
        {
            if (!mod)
                return false;
            wchar_t path[MAX_PATH]{};
            if (GetModuleFileNameW(mod, path, _countof(path)) == 0)
                return false;
            const wchar_t* base = wcsrchr(path, L'\\');
            base = base ? base + 1 : path;
            return _wcsicmp(base, L"System.Windows.Forms.ni.dll") == 0;
        }

        // WinForms .SysDateTimePick32: BeginPaint caller часто forms.ni — IAT на comctl32.
        [[nodiscard]] static HMODULE ResolveDtpComctlHookModule(HMODULE callerMod) noexcept
        {
            auto* g_new = GlassCore::ModuleHandler::g_comctl32New;
            auto* g_legacy = GlassCore::ModuleHandler::g_comctl32Legacy;
            if (callerMod == g_new || callerMod == g_legacy)
                return callerMod;
            if (IsSystemWindowsFormsNiModule(callerMod))
                return g_new ? g_new : g_legacy;
            return nullptr;
        }

        [[nodiscard]] static HMODULE ResolveDtpSessionComctlModule(HMODULE callerMod) noexcept
        {
            if (HMODULE resolved = ResolveDtpComctlHookModule(callerMod))
                return resolved;
            return ResolveDefaultComctlModule();
        }

        int      WINAPI Overall_MyDrawThemeBg(HTHEME, HDC, int, int, LPCRECT, LPCRECT);
        HRESULT  WINAPI Overall_MyDrawThemeText(HTHEME, HDC, int, int, LPCWSTR, int, DWORD, DWORD, LPCRECT);
        HRESULT  WINAPI Overall_MyDrawThemeTextEx(HTHEME, HDC, int, int, LPCWSTR, int, DWORD, LPRECT, const DTTOPTS*);
        int      WINAPI Overall_MyDrawTextW(HDC, LPCWSTR, int, LPRECT, UINT);
        BOOL     WINAPI Overall_MyExtTextOutW(HDC, int, int, UINT, const RECT*, LPCWSTR, UINT, const INT*);

        enum OverallHookIndex : size_t
        {
            OHK_DRAWTHEMEBG = 0,
            OHK_DRAWTHEMETEXT,
            OHK_DRAWTHEMETEXTEX,
            OHK_DRAWTEXT,
            OHK_EXTTEXTOUT,
            OHK_COUNT
        };

        inline const HookDispatcherDependency<OHK_COUNT, 3> g_dep
        { {
            std::tuple{ std::array{ "uxtheme.dll"sv, "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv, ""sv },
                "DrawThemeBackground", reinterpret_cast<PVOID>(Overall_MyDrawThemeBg) },
            std::tuple{ std::array{ "uxtheme.dll"sv, "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv, ""sv },
                "DrawThemeText", reinterpret_cast<PVOID>(Overall_MyDrawThemeText) },
            std::tuple{ std::array{ "uxtheme.dll"sv, "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv, ""sv },
                "DrawThemeTextEx", reinterpret_cast<PVOID>(Overall_MyDrawThemeTextEx) },
            std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
                "DrawTextW", reinterpret_cast<PVOID>(Overall_MyDrawTextW) },
            std::tuple{ std::array{ "gdi32.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
                "ExtTextOutW", reinterpret_cast<PVOID>(Overall_MyExtTextOutW) },
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

        template <size_t index, typename T>
        static T GetOverallOrigSafe(HMODULE hookModule)
        {
            if (!hookModule)
                return nullptr;
            auto lock = g_overallLock.lock_shared();
            auto it = g_overallMap.find(static_cast<void*>(hookModule));
            if (it != g_overallMap.end())
                return it->second.template GetOrg<index, T>();
            return nullptr;
        }

        template <size_t index, typename T>
        static T GetSessionOrigSafe()
        {
            if (g_stack.empty())
                return nullptr;
            return GetOverallOrigSafe<index, T>(g_stack.back().mod);
        }

        [[nodiscard]] static HRESULT BlackDatePickerThemeBg(
            HDC hdc, LPCRECT pRect, LPCRECT pClipRect) noexcept
        {
            if (!pRect || IsRectEmpty(pRect))
                return S_OK;

            RECT clip = *pRect;
            if (pClipRect)
                IntersectRect(&clip, &clip, pClipRect);

            PatBlt(hdc,
                clip.left, clip.top,
                clip.right - clip.left,
                clip.bottom - clip.top,
                BLACKNESS);
            return S_OK;
        }

        [[nodiscard]] static bool IsDatePickerThemeClass(HTHEME hTheme) noexcept
        {
            WCHAR cls[256]{};
            return SUCCEEDED(Shared::ThemeHelper::GetThemeClass(hTheme, cls, 255))
                && StrEq(cls, L"DatePicker");
        }

        [[nodiscard]] static bool IsGrayColor(COLORREF color) noexcept
        {
            return GetRValue(color) == GetGValue(color) && GetRValue(color) == GetBValue(color);
        }

        // BWM WinHooks::MyExtTextOutW: ETO_OPAQUE → PatBlt(BLACKNESS) + FillRect alpha-кистью по GetBkColor.
        static void ApplyDtpOpaqueBackground(HDC hdc, const RECT* lprect) noexcept
        {
            if (!lprect || IsRectEmpty(lprect))
                return;

            PatBlt(hdc,
                lprect->left, lprect->top,
                lprect->right - lprect->left,
                lprect->bottom - lprect->top,
                BLACKNESS);

            const COLORREF color = GetBkColor(hdc);
            if (color == GetSysColor(COLOR_HIGHLIGHT)
                || color == GetSysColor(COLOR_BTNFACE)
                || color == GetSysColor(COLOR_BTNSHADOW)
                || !IsGrayColor(color))
            {
                wil::unique_hbrush brush{
                    Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(color, 255) };
                if (brush)
                    FillRect(hdc, lprect, brush.get());
            }
        }

        int WINAPI Overall_MyDrawThemeBg(
            HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
            LPCRECT pRect, LPCRECT pClipRect)
        {
            auto orig = GetSessionOrigSafe<OHK_DRAWTHEMEBG, decltype(&DrawThemeBackground)>();

            if (!orig)
                return DrawThemeBackground(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

            if (!DtpSessionActive())
                return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

            if (IsDatePickerThemeClass(hTheme) && (iPartId == 0 || iPartId == 2))
                return BlackDatePickerThemeBg(hdc, pRect, pClipRect);

            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
        }

        HRESULT WINAPI Overall_MyDrawThemeText(
            HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
            LPCWSTR pszText, int cchText, DWORD dwTextFlags, DWORD dwTextFlags2,
            LPCRECT pRect)
        {
            auto orig = GetSessionOrigSafe<OHK_DRAWTHEMETEXT, decltype(&DrawThemeText)>();

            if (!orig)
                return DrawThemeText(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, dwTextFlags2, pRect);

            if (!DtpSessionActive())
                return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, dwTextFlags2, pRect);

            if (!IsDatePickerThemeClass(hTheme))
                return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, dwTextFlags2, pRect);

            if (!pszText || cchText == 0 || !pRect || IsRectEmpty(pRect))
                return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, dwTextFlags2, pRect);

            DTTOPTS options{ sizeof(DTTOPTS), DTT_COMPOSITED };
            return GlassCore::Text::DrawThemeContent(
                hdc,
                *pRect,
                nullptr,
                nullptr,
                0,
                [&](HDC memoryDC, HPAINTBUFFER, RGBQUAD*, int)
                {
                    DrawThemeTextEx(
                        hTheme,
                        memoryDC,
                        iPartId,
                        iStateId,
                        pszText,
                        cchText,
                        dwTextFlags,
                        const_cast<LPRECT>(pRect),
                        &options);
                });
        }

        HRESULT WINAPI Overall_MyDrawThemeTextEx(
            HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
            LPCWSTR pszText, int cchText, DWORD dwTextFlags,
            LPRECT pRect, const DTTOPTS* pOptions)
        {
            auto orig = GetSessionOrigSafe<OHK_DRAWTHEMETEXTEX, decltype(&DrawThemeTextEx)>();

            if (!orig)
                return DrawThemeTextEx(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions);

            if (!DtpSessionActive())
                return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions);

            if (!IsDatePickerThemeClass(hTheme))
                return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions);

            if (!pRect || IsRectEmpty(pRect))
                return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions);

            const DTTOPTS* effectiveOpts = pOptions;
            DTTOPTS defaultOpts{ sizeof(DTTOPTS) };
            if (!effectiveOpts)
                effectiveOpts = &defaultOpts;

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
                hdc,
                *pRect,
                nullptr,
                nullptr,
                0,
                [&](HDC memoryDC, HPAINTBUFFER, RGBQUAD*, int)
                {
                    orig(hTheme, memoryDC, iPartId, iStateId,
                        pszText, cchText, dwTextFlags, pRect, &work);
                });
        }

        int WINAPI Overall_MyDrawTextW(
            HDC hdc, LPCWSTR lpchText, int cchText, LPRECT lprc, UINT format)
        {
            auto orig = GetSessionOrigSafe<OHK_DRAWTEXT, decltype(&DrawTextW)>();

            if (!orig)
                return DrawTextW(hdc, lpchText, cchText, lprc, format);

            if (!DtpSessionActive())
                return orig(hdc, lpchText, cchText, lprc, format);

            if (format & DT_CALCRECT)
                return orig(hdc, lpchText, cchText, lprc, format);

            if (!lprc || IsRectEmpty(lprc) || !lpchText || cchText == 0)
                return orig(hdc, lpchText, cchText, lprc, format);

            // BWM WinHooks::MyDrawTextW: OPAQUE — PatBlt + FillRect по GetBkColor (не WINDOW).
            if (GetBkMode(hdc) == OPAQUE)
            {
                PatBlt(hdc, lprc->left, lprc->top,
                    lprc->right - lprc->left, lprc->bottom - lprc->top, BLACKNESS);

                const COLORREF color = GetBkColor(hdc);
                if (color && color != GetSysColor(COLOR_WINDOW))
                {
                    wil::unique_hbrush brush{
                        Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(color, 255) };
                    if (brush)
                        FillRect(hdc, lprc, brush.get());
                }
            }
            else
            {
                PatBlt(hdc, lprc->left, lprc->top,
                    lprc->right - lprc->left, lprc->bottom - lprc->top, BLACKNESS);
            }

            int result = 0;
            if (!FAILED(GlassCore::Text::DrawTextWithAlpha(hdc, lpchText, cchText, lprc, format, result)))
                return result;
            return orig(hdc, lpchText, cchText, lprc, format);
        }

        BOOL WINAPI Overall_MyExtTextOutW(
            HDC hdc, int x, int y, UINT options, const RECT* lprect,
            LPCWSTR lpString, UINT c, const INT* lpDx)
        {
            auto orig = GetSessionOrigSafe<OHK_EXTTEXTOUT, decltype(&ExtTextOutW)>();

            if (!orig)
                return ExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);

            if (!DtpSessionActive())
                return orig(hdc, x, y, options, lprect, lpString, c, lpDx);

            if ((options & ETO_OPAQUE) && lprect)
            {
                ApplyDtpOpaqueBackground(hdc, lprect);

                const bool emptyOrSpace =
                    c == 0 || (c == 1 && lpString && lpString[0] == L' ');
                if (emptyOrSpace)
                    return TRUE;
            }

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

        static HMODULE GetCallerModule(void* ra)
        {
            HMODULE hMod = nullptr;
            GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(ra),
                &hMod);
            return hMod;
        }

        static void EnableDtpHooksForSession(
            GlassShared::GlassControlKind kind,
            HMODULE comctlMod,
            DtpKind sessionKind,
            bool enable) noexcept
        {
            if (!comctlMod)
                return;

            EnableHooksOnModule(comctlMod, OHK_EXTTEXTOUT, enable);
            if (sessionKind == DtpKind::Themed)
            {
                EnableHooksOnModule(comctlMod, OHK_DRAWTHEMEBG, enable);
                EnableHooksOnModule(comctlMod, OHK_DRAWTHEMETEXT, enable);
                EnableHooksOnModule(comctlMod, OHK_DRAWTHEMETEXTEX, enable);
            }
            else
                EnableHooksOnModule(comctlMod, OHK_DRAWTEXT, enable);

            if (kind == GlassShared::GlassControlKind::WinForms_DateTimePicker
                && GetModuleHandleW(L"System.Windows.Forms.ni.dll"))
            {
                GlassCore::FormsHooks::DrawThemeBackgroundAttach(enable);
            }
        }

        [[nodiscard]] bool FormsNiDtb_IsSessionActiveImpl() noexcept
        {
            return DtpSessionActive()
                && IsDtpSessionKind(GlassShared::GlassGetWindowKind(g_dtpHwnd))
                && GlassShared::GlassGetWindowKind(g_dtpHwnd)
                    == GlassShared::GlassControlKind::WinForms_DateTimePicker;
        }

        [[nodiscard]] HRESULT FormsNiDtb_TryDrawThemeBackgroundImpl(
            Shared::WinApiUtils::DrawThemeBackground_pfn orig,
            HTHEME hTheme,
            HDC hdc,
            int iPartId,
            int iStateId,
            LPCRECT pRect,
            LPCRECT pClipRect) noexcept
        {
            if (IsDatePickerThemeClass(hTheme) && (iPartId == 0 || iPartId == 2))
                return BlackDatePickerThemeBg(hdc, pRect, pClipRect);

            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
        }

        static void OnBeginPaintGlobal(HWND hWnd, HMODULE callerMod)
        {
            using namespace GlassShared;
            if (g_shutdown.load(std::memory_order_acquire))
                return;

            const GlassControlKind kind = GlassGetWindowKind(hWnd);
            if (!IsDtpSessionKind(kind))
                return;

            const HMODULE comctlMod = ResolveDtpSessionComctlModule(callerMod);
            if (!comctlMod)
                return;

            g_dtpHwnd = hWnd;
            const bool themed = GetWindowTheme(hWnd) != nullptr;
            const DtpKind sessionKind = themed ? DtpKind::Themed : DtpKind::Unthemed;
            g_stack.push_back({ comctlMod, sessionKind });

            EnableDtpHooksForSession(kind, comctlMod, sessionKind, true);

            DebugLog(DBG_NORMAL,
                L"[GlassCore][DateTimePicker] OnBeginPaint: hwnd=0x%p kind=%u themed=%d caller=%p comctl=%p",
                hWnd,
                static_cast<unsigned>(kind),
                themed ? 1 : 0,
                callerMod,
                comctlMod);
        }

        static void OnEndPaintGlobal(HWND hWnd, HMODULE /*callerMod*/)
        {
            using namespace GlassShared;
            const GlassControlKind kind = GlassGetWindowKind(hWnd);
            if (!IsDtpSessionKind(kind))
                return;
            if (g_stack.empty())
                return;

            const Session s = g_stack.back();
            g_stack.pop_back();

            EnableDtpHooksForSession(kind, s.mod, s.k, false);

            if (g_stack.empty())
                g_dtpHwnd = nullptr;
        }
    }

    namespace FormsNiDtbAccess
    {
        bool IsWinFormsDateTimePickerDrawThemeSessionActive() noexcept
        {
            return FormsNiDtb_IsSessionActiveImpl();
        }

        HRESULT TryDrawThemeBackground(
            Shared::WinApiUtils::DrawThemeBackground_pfn orig,
            HTHEME hTheme,
            HDC hdc,
            int iPartId,
            int iStateId,
            LPCRECT pRect,
            LPCRECT pClipRect) noexcept
        {
            return FormsNiDtb_TryDrawThemeBackgroundImpl(
                orig, hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
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
        g_stack.clear();
        g_dtpHwnd = nullptr;
        g_shutdown.store(false, std::memory_order_seq_cst);
    }
}
