// ListBox / WinForms .LISTBOX — нативный: FillRect + ExtTextOut + DrawThemeBackground (comctl32);
// ExtTextOutW также на uxtheme.dll / main.cpl. Сессия: BeginPaint + GetDC + CreateCompatibleDC (g_lbSessionRef).
// IAT на comctl32: ResolveListBoxComctlHookModule (caller gdi32/uxtheme → база new/legacy).
// WinForms (BWM parity): comctl32 ExtTextOutW + TabbedTextOutW; forms.ni DrawTextExW detour + GetSysColorBrush;
// gdiplus.dll IAT PatBlt + GetStockObject (фон строк). Без OHK_DRAWTHEMEBG / forms.ni DTB detour.

#include "CorePch.h"
#include <Hooks/ListBox.h>
#include <Hooks/forms/FormsHooks.h>
#include <Hooks/ScrollBarDrawTheme.h>
#include <Hooks/ScrollBarTheme.h>
#include <Subsystems/RenderText.h>
#include <Helpers/ThemeHelper.h>
#include <Core/Intercept/HookDispatcher.h>
#include <Module/ModuleHandler.h>
#include <Subsystems/PaintDispatcher.h>
#include <GlassControlKind.h>
#include <wil.h>
#include <uxtheme.h>
#include <WinApiUtils.h>
#include <vector>
#include <cmath>

#include <DebugLog.h>

using namespace GlassCore::Hooks;
using namespace std::literals;

namespace GlassCore::ListBoxControl
{
    namespace
    {
        // Счётчик активных сессий ListBox (BeginPaint + успешные GetDC/Compat). Хуки при 0→1 / 1→0.
        // Для GetDC/CreateCompatibleDC: каждому onBegin соответствует маркер в стеке (1 = мы увеличили ref,
        // 0 = ранний выход без ref), чтобы Release/DeleteDC не снимал хуки от BeginPaint при «чужом» caller/hWnd.
        thread_local int                g_lbSessionRef      = 0;
        thread_local HMODULE            g_lbSessionMod      = nullptr;
        thread_local bool               g_lbSessionWinForms = false;
        thread_local std::vector<uint8_t> g_lbDcLikeArmStack;

        static std::atomic<bool> g_shutdown{ false };

        [[nodiscard]] static bool ListBoxHooksSessionActive() noexcept
        {
            return g_lbSessionRef > 0;
        }

        // PaintDispatcher host — только ListBox / WinForms_ListBox (не updown и прочие comctl32 DTB).
        [[nodiscard]] static bool IsListBoxPaintSessionHost() noexcept
        {
            HWND host = GlassCore::PaintDispatcher::GetPaintSessionHostHwnd();
            if (!host || !IsWindow(host))
                return false;
            using namespace GlassShared;
            const GlassControlKind k = GlassGetWindowKind(host);
            return k == GlassControlKind::ListBox || k == GlassControlKind::WinForms_ListBox;
        }

        static bool StrEq(LPCWSTR a, LPCWSTR b) noexcept
        {
            return a && b && _wcsicmp(a, b) == 0;
        }

        // SEH вынесен: __try нельзя в функции с C++-объектами (DrawThemeOrigCallDepth и т.д.).
        [[nodiscard]] static bool TryGetListBoxThemeClassSafe(
            HTHEME hTheme, WCHAR* themeClass, int cch, HRESULT* outHr) noexcept
        {
            __try
            {
                *outHr = Shared::ThemeHelper::GetThemeClass(hTheme, themeClass, cch);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        [[nodiscard]] static int GetScrollBandSizeForDpiCached(HDC hdc) noexcept
        {
            static thread_local UINT s_cachedDpi{ 0 };
            static thread_local int  s_cachedBand{ 0 };

            HWND hwnd = GlassCore::PaintDispatcher::GetPaintSessionHostHwnd();
            if (!hwnd || !IsWindow(hwnd))
                hwnd = WindowFromDC(hdc);

            UINT dpi = 96;
            if (hwnd && IsWindow(hwnd))
                dpi = GetDpiForWindow(hwnd);
            if (!dpi)
                dpi = 96;

            if (s_cachedDpi != dpi || s_cachedBand <= 0)
            {
                int band = 0;
                if (dpi != 96)
                {
                    const int v = GetSystemMetricsForDpi(SM_CXVSCROLL, dpi);
                    const int h = GetSystemMetricsForDpi(SM_CYHSCROLL, dpi);
                    band = std::max(v, h);
                }
                else
                {
                    band = std::max(GetSystemMetrics(SM_CXVSCROLL), GetSystemMetrics(SM_CYHSCROLL));
                }
                if (band <= 0)
                    band = std::max(GetSystemMetrics(SM_CXVSCROLL), GetSystemMetrics(SM_CYHSCROLL));

                s_cachedDpi = dpi;
                s_cachedBand = band;
            }

            return s_cachedBand;
        }

        BOOL WINAPI Overall_MyFillRect(HDC, const RECT*, HBRUSH);
        BOOL WINAPI Overall_MyExtTextOutW(HDC, int, int, UINT, const RECT*, LPCWSTR, UINT, const INT*);
        LONG WINAPI Overall_MyTabbedTextOutW(
            HDC, int, int, LPCWSTR, int, int, const INT*, int);
        int  WINAPI Overall_MyDrawTextExW(HDC, LPWSTR, int, LPRECT, UINT, LPDRAWTEXTPARAMS);
        HGDIOBJ WINAPI Overall_MyGetStockObject(int);
        BOOL WINAPI Overall_MyPatBlt(HDC, int, int, int, int, DWORD);
        HRESULT WINAPI Overall_MyDrawThemeBg(
            HTHEME, HDC, int, int, LPCRECT, LPCRECT);

        enum OverallHookIndex : size_t
        {
            OHK_FILLRECT = 0,
            OHK_EXTTEXTOUT,
            OHK_TABBEDTEXTOUT,
            OHK_DRAWTEXTEX,
            OHK_DRAWTHEMEBG,
            OHK_GETSTOCKOBJECT,
            OHK_PATBLT,
            OHK_COUNT
        };

        inline const HookDispatcherDependency<OHK_COUNT, 3> g_dep
        { {
            std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
                "FillRect", reinterpret_cast<PVOID>(Overall_MyFillRect) },
            std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv },
                "ExtTextOutW", reinterpret_cast<PVOID>(Overall_MyExtTextOutW) },
            std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
                "TabbedTextOutW", reinterpret_cast<PVOID>(Overall_MyTabbedTextOutW) },
            std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
                "DrawTextExW", reinterpret_cast<PVOID>(Overall_MyDrawTextExW) },
            std::tuple{ std::array{ "uxtheme.dll"sv, "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv, ""sv },
                "DrawThemeBackground", reinterpret_cast<PVOID>(Overall_MyDrawThemeBg) },
            std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv },
                "GetStockObject", reinterpret_cast<PVOID>(Overall_MyGetStockObject) },
            std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv },
                "PatBlt", reinterpret_cast<PVOID>(Overall_MyPatBlt) },
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

        // uxtheme.dll + main.cpl — IAT ExtTextOutW; только пока активна сессия ListBox.
        static void EnableListBoxAuxExtTextOutModules(bool enable) noexcept
        {
            if (HMODULE const ux = GetModuleHandleW(L"uxtheme.dll"))
                EnableHooksOnModule(ux, OHK_EXTTEXTOUT, enable);
            if (HMODULE const mc = GetModuleHandleW(L"main.cpl"))
                EnableHooksOnModule(mc, OHK_EXTTEXTOUT, enable);
        }

        // База IAT для ListBox — всегда образ comctl32 (см. PaintDispatcher: disable = тому же модулю, что enable).
        [[nodiscard]] static HMODULE ResolveListBoxComctlHookModule(HMODULE callerMod) noexcept
        {
            auto* g_new = GlassCore::ModuleHandler::g_comctl32New;
            auto* g_legacy = GlassCore::ModuleHandler::g_comctl32Legacy;
            if (callerMod == g_new || callerMod == g_legacy)
                return callerMod;
            return g_new ? g_new : g_legacy;
        }

        // gdiplus.dll — IAT PatBlt + GetStockObject (BWM PaintHandler .LISTBOX).
        static void EnableListBoxGdiplusHooks(bool enable) noexcept
        {
            if (HMODULE const gdiplus = GetModuleHandleW(L"gdiplus.dll"))
            {
                EnableHooksOnModule(gdiplus, OHK_GETSTOCKOBJECT, enable);
                EnableHooksOnModule(gdiplus, OHK_PATBLT, enable);
            }
        }

        static void EnableListBoxSessionHooks(HMODULE callerMod, bool winForms, bool enable) noexcept
        {
            if (!callerMod) return;
            if (winForms)
            {
                // BWM PaintHandler .LISTBOX (caller=comctl32): ExtTextOut + TabbedTextOut, gdiplus PatBlt/GetStockObject, DrawTextEx detour.
                EnableHooksOnModule(callerMod, OHK_EXTTEXTOUT, enable);
                EnableHooksOnModule(callerMod, OHK_TABBEDTEXTOUT, enable);
                EnableListBoxGdiplusHooks(enable);
                EnableListBoxAuxExtTextOutModules(enable);
                if (GetModuleHandleW(L"System.Windows.Forms.ni.dll"))
                    GlassCore::FormsHooks::DrawTextExAttach(enable);
            }
            else
            {
                EnableHooksOnModule(callerMod, OHK_FILLRECT, enable);
                EnableHooksOnModule(callerMod, OHK_EXTTEXTOUT, enable);
                EnableHooksOnModule(callerMod, OHK_DRAWTHEMEBG, enable);
                EnableListBoxAuxExtTextOutModules(enable);
            }
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

        BOOL WINAPI Overall_MyFillRect(HDC hdc, const RECT* lprc, HBRUSH hbr)
        {
            HMODULE callerMod = GetCallerModule(_ReturnAddress());
            auto orig = GetOverallOrigSafe<OHK_FILLRECT, decltype(&FillRect)>(callerMod);

            if (!orig)
                return FillRect(hdc, lprc, hbr);

            if (!ListBoxHooksSessionActive() || g_lbSessionWinForms)
                return orig(hdc, lprc, hbr);

            if (!lprc || !hbr)
                return orig(hdc, lprc, hbr);

            LOGBRUSH lb{};
            if (GetObject(hbr, sizeof(lb), &lb) && lb.lbStyle == BS_SOLID)
            {
                const int rcW = lprc->right - lprc->left;
                const int rcH = lprc->bottom - lprc->top;
                const int band = GetScrollBandSizeForDpiCached(hdc);

                if (lb.lbColor == GetSysColor(COLOR_SCROLLBAR)
                    && rcW > 0
                    && rcH > 0
                    && band > 0
                    && std::abs(rcW - rcH) <= 1
                    && std::abs(rcW - band) <= 1
                    && std::abs(rcH - band) <= 1)
                {
                    PatBlt(hdc, lprc->left, lprc->top, rcW, rcH, BLACKNESS);
                    return TRUE;
                }

                if (lb.lbColor == GetSysColor(COLOR_HIGHLIGHT))
                {
                    HBRUSH alphaBrush = Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(lb.lbColor, 255);
                    if (alphaBrush)
                    {
                        BOOL res = orig(hdc, lprc, alphaBrush);
                        DeleteObject(alphaBrush);
                        return res;
                    }
                }
                if (lb.lbColor == GetSysColor(COLOR_WINDOW))
                {
                    PatBlt(hdc,
                        lprc->left, lprc->top,
                        lprc->right - lprc->left,
                        lprc->bottom - lprc->top,
                        BLACKNESS);
                    return TRUE;
                }
                if (!lb.lbColor)
                    return orig(hdc, lprc, hbr);

                HBRUSH alphaBrush = Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(lb.lbColor, 255);
                if (alphaBrush)
                {
                    BOOL res = orig(hdc, lprc, alphaBrush);
                    DeleteObject(alphaBrush);
                    return res;
                }
            }
            return orig(hdc, lprc, hbr);
        }

        BOOL WINAPI Overall_MyExtTextOutW(
            HDC hdc, int x, int y, UINT options, const RECT* lprect,
            LPCWSTR lpString, UINT c, const INT* lpDx)
        {
            HMODULE callerMod = GetCallerModule(_ReturnAddress());
            auto orig = GetOverallOrigSafe<OHK_EXTTEXTOUT, decltype(&ExtTextOutW)>(callerMod);

            if (!orig)
                return ExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);

            if (!ListBoxHooksSessionActive())
                return orig(hdc, x, y, options, lprect, lpString, c, lpDx);

            if ((options & ETO_OPAQUE) && lprect)
            {
                PatBlt(hdc, lprect->left, lprect->top,
                    lprect->right - lprect->left, lprect->bottom - lprect->top, BLACKNESS);
                const COLORREF bk = GetBkColor(hdc);
                if (bk == GetSysColor(COLOR_HIGHLIGHT))
                {
                    wil::unique_hbrush hi{ Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(bk, 255) };
                    if (hi)
                        FillRect(hdc, lprect, hi.get());
                }
            }

            // BWM-паритет (WinHooks: ETO_OPAQUE → return TRUE без orig; ComboLBox: OPAQUE && !c → TRUE):
            // фон уже залит выше (BLACKNESS [+alpha HIGHLIGHT]) — вызов orig с ETO_OPAQUE перекрасил бы
            // rect дефолтной opaque-кистью поверх нашей (баг: на выделенной строке кисть без альфы).
            if ((options & ETO_OPAQUE) && lprect && !c)
                return TRUE;

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

        LONG WINAPI Overall_MyTabbedTextOutW(
            HDC hdc, int x, int y, LPCWSTR lpString, int chCount,
            int nTabPositions, const INT* lpnTabStopPositions, int nTabOrigin)
        {
            HMODULE callerMod = GetCallerModule(_ReturnAddress());
            auto orig = GetOverallOrigSafe<OHK_TABBEDTEXTOUT, decltype(&TabbedTextOutW)>(callerMod);

            if (!orig)
                return TabbedTextOutW(hdc, x, y, lpString, chCount, nTabPositions, lpnTabStopPositions, nTabOrigin);

            if (!ListBoxHooksSessionActive())
                return orig(hdc, x, y, lpString, chCount, nTabPositions, lpnTabStopPositions, nTabOrigin);

            if (nTabPositions == 0 && chCount > 0 && lpString)
            {
                RECT clip{};
                GetClipBox(hdc, &clip);
                clip.left = x;
                clip.top = y;
                int result = 0;
                if (!FAILED(GlassCore::Text::DrawTextWithAlpha(
                        hdc, lpString, chCount, &clip,
                        DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOCLIP, result)))
                    return static_cast<LONG>(result);
            }

            return orig(hdc, x, y, lpString, chCount, nTabPositions, lpnTabStopPositions, nTabOrigin);
        }

        int WINAPI Overall_MyDrawTextExW(
            HDC hdc, LPWSTR lpchText, int cchText,
            LPRECT lprc, UINT format, LPDRAWTEXTPARAMS lpdtp)
        {
            HMODULE callerMod = GetCallerModule(_ReturnAddress());
            auto orig = GetOverallOrigSafe<OHK_DRAWTEXTEX, decltype(&DrawTextExW)>(callerMod);

            if (!orig)
                return DrawTextExW(hdc, lpchText, cchText, lprc, format, lpdtp);

            if (!ListBoxHooksSessionActive() || !g_lbSessionWinForms)
                return orig(hdc, lpchText, cchText, lprc, format, lpdtp);

            if (format & DT_CALCRECT)
                return orig(hdc, lpchText, cchText, lprc, format, lpdtp);

            if (!lprc || IsRectEmpty(lprc))
                return orig(hdc, lpchText, cchText, lprc, format, lpdtp);

            int result = 0;
            if (!FAILED(GlassCore::Text::DrawTextWithAlpha(hdc, lpchText, cchText, lprc, format, result)))
                return result;

            return orig(hdc, lpchText, cchText, lprc, format, lpdtp);
        }

        [[nodiscard]] static bool IsWinFormsListBoxGdiplusHookActive() noexcept
        {
            return ListBoxHooksSessionActive()
                && g_lbSessionWinForms
                && IsListBoxPaintSessionHost();
        }

        HGDIOBJ WINAPI Overall_MyGetStockObject(int i)
        {
            HMODULE callerMod = GetCallerModule(_ReturnAddress());
            auto orig = GetOverallOrigSafe<OHK_GETSTOCKOBJECT, decltype(&GetStockObject)>(callerMod);

            if (!orig)
                return GetStockObject(i);

            if (IsWinFormsListBoxGdiplusHookActive())
            {
                if (i == WHITE_BRUSH || i == DC_BRUSH)
                    i = BLACK_BRUSH;
            }

            return orig(i);
        }

        BOOL WINAPI Overall_MyPatBlt(HDC hdc, int x, int y, int cx, int cy, DWORD rop)
        {
            HMODULE callerMod = GetCallerModule(_ReturnAddress());
            auto orig = GetOverallOrigSafe<OHK_PATBLT, decltype(&PatBlt)>(callerMod);

            if (!orig)
                return PatBlt(hdc, x, y, cx, cy, rop);

            if (!IsWinFormsListBoxGdiplusHookActive())
                return orig(hdc, x, y, cx, cy, rop);

            LOGBRUSH lb{};
            HGDIOBJ const hbrObj = GetCurrentObject(hdc, OBJ_BRUSH);
            if (!hbrObj || !GetObject(hbrObj, sizeof(lb), &lb))
                return orig(hdc, x, y, cx, cy, rop);

            const COLORREF rgb = lb.lbColor & 0xFFFFFF;
            if (!rgb
                || rgb == (GetSysColor(COLOR_WINDOW) & 0xFFFFFF)
                || rgb == (GetSysColor(COLOR_BTNFACE) & 0xFFFFFF))
            {
                return PatBlt(hdc, x, y, cx, cy, BLACKNESS);
            }

            wil::unique_hbrush alphaBrush{
                Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(lb.lbColor, 255) };
            if (!alphaBrush)
                return orig(hdc, x, y, cx, cy, rop);

            HGDIOBJ const old = SelectObject(hdc, alphaBrush.get());
            const BOOL res = orig(hdc, x, y, cx, cy, rop);
            SelectObject(hdc, old);
            return res;
        }

        // ── DrawThemeBackground ──────────────────────────────────────────────────
        // Тема "ScrollBar": как ListView — ScrollBarDrawTheme + GetComctlScrollBarDrawThemeOrig.
        // Тема "ListBox": фон клиента (part=0 state=0) — PatBlt(BLACKNESS) под «стекло»; WinForms — orig.
        HRESULT WINAPI Overall_MyDrawThemeBg(
            HTHEME hTheme,
            HDC     hdc,
            int     iPartId,
            int     iStateId,
            LPCRECT pRect,
            LPCRECT pClipRect)
        {
            HMODULE callerMod = GetCallerModule(_ReturnAddress());
            auto orig = GetOverallOrigSafe<OHK_DRAWTHEMEBG, decltype(&DrawThemeBackground)>(callerMod);

            if (!orig)
                return DrawThemeBackground(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

            if (g_shutdown.load(std::memory_order_acquire))
                return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

            if (!ListBoxHooksSessionActive() || !IsListBoxPaintSessionHost())
                return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

            if (!hTheme)
                return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

            WCHAR themeClass[MAX_PATH + 1]{};
            HRESULT hrThemeClass = E_FAIL;
            if (!TryGetListBoxThemeClassSafe(hTheme, themeClass, MAX_PATH, &hrThemeClass))
            {
                DebugLog(DBG_CRITICAL,
                    L"[GlassCore][ListBox] GetThemeClass AV hTheme=%p caller=%p",
                    hTheme,
                    callerMod);
                return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
            }
            if (FAILED(hrThemeClass))
                return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

            if (StrEq(themeClass, L"ScrollBar"))
            {
                auto sbOrig = GlassCore::ScrollBarTheme::GetComctlScrollBarDrawThemeOrig(callerMod);
                auto dtbOrig  = sbOrig ? sbOrig : orig;
                if (!dtbOrig)
                    return DrawThemeBackground(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

                GlassCore::ScrollBarDrawTheme::LogScrollBarDrawThemeCall(
                    L"[GlassCore][ListBox]",
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

            if (StrEq(themeClass, L"ListBox"))
            {
                if (pRect && !IsRectEmpty(pRect) && iPartId == 0 && iStateId == 0)
                {
                    const RECT* clipRect = pClipRect ? pClipRect : pRect;
                    PatBlt(hdc,
                        clipRect->left,
                        clipRect->top,
                        clipRect->right - clipRect->left,
                        clipRect->bottom - clipRect->top,
                        BLACKNESS);
                    return S_OK;
                }
            }

            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
        }

        static void OnBeginPaintGlobal(HWND hWnd, HMODULE callerMod)
        {
            using namespace GlassShared;
            if (g_shutdown.load(std::memory_order_acquire)) return;

            const GlassControlKind k = GlassGetWindowKind(hWnd);
            if (k != GlassControlKind::ListBox && k != GlassControlKind::WinForms_ListBox)
                return;

            HMODULE const hookMod = ResolveListBoxComctlHookModule(callerMod);
            if (!hookMod)
                return;

            GlassCore::ScrollBarTheme::EnsureComctlScrollBarIatIfComctlCaller(hWnd, hookMod);

            const bool winForms = (k == GlassControlKind::WinForms_ListBox);
            if (g_lbSessionRef == 0)
            {
                g_lbSessionMod = hookMod;
                g_lbSessionWinForms = winForms;
                EnableListBoxSessionHooks(hookMod, winForms, true);
            }
            g_lbSessionRef++;
        }

        static void OnEndPaintGlobal(HWND hWnd, HMODULE /*callerMod*/)
        {
            using namespace GlassShared;
            if (g_shutdown.load(std::memory_order_acquire)) return;
            const GlassControlKind k = GlassGetWindowKind(hWnd);
            if (k != GlassControlKind::ListBox && k != GlassControlKind::WinForms_ListBox)
                return;
            if (g_lbSessionRef == 0) return;

            g_lbSessionRef--;
            if (g_lbSessionRef == 0 && g_lbSessionMod)
            {
                EnableListBoxSessionHooks(g_lbSessionMod, g_lbSessionWinForms, false);
                g_lbSessionMod = nullptr;
            }
        }

        // GetDC / CreateCompatibleDC: как Edit (стек модулей) — включаем IAT на comctl32, не на caller gdi32/uxtheme.
        static void ListBoxOnDcLikeSessionBegin(HWND hWnd, HMODULE callerMod)
        {
            uint8_t marker = 0;
            auto pushArm = wil::scope_exit([&]() noexcept { g_lbDcLikeArmStack.push_back(marker); });

            using namespace GlassShared;
            if (g_shutdown.load(std::memory_order_acquire)) return;

            const GlassControlKind k = GlassGetWindowKind(hWnd);
            if (k != GlassControlKind::ListBox && k != GlassControlKind::WinForms_ListBox)
                return;

            HMODULE const hookMod = ResolveListBoxComctlHookModule(callerMod);
            if (!hookMod)
                return;

            GlassCore::ScrollBarTheme::EnsureComctlScrollBarIatIfComctlCaller(hWnd, hookMod);

            const bool winForms = (k == GlassControlKind::WinForms_ListBox);
            const int oldRef = g_lbSessionRef;
            marker = 1;
            g_lbSessionRef++;
            if (oldRef == 0)
            {
                g_lbSessionMod = hookMod;
                g_lbSessionWinForms = winForms;
                EnableListBoxSessionHooks(hookMod, winForms, true);
            }
        }

        static void ListBoxOnDcLikeSessionEnd(HWND /*hWnd*/, HMODULE /*callerMod*/)
        {
            // Пустой arm / ref<=0: silent (контракт PaintDispatcher; WARN заливал лог в PopupOnly).
            if (g_lbDcLikeArmStack.empty())
                return;
            const uint8_t armed = g_lbDcLikeArmStack.back();
            g_lbDcLikeArmStack.pop_back();
            if (!armed) return;

            if (g_lbSessionRef <= 0)
                return;
            g_lbSessionRef--;
            if (g_lbSessionRef == 0 && g_lbSessionMod)
            {
                EnableListBoxSessionHooks(g_lbSessionMod, g_lbSessionWinForms, false);
                g_lbSessionMod = nullptr;
            }
        }

        static void OnListBoxCompatBegin(HWND hWnd, HMODULE callerMod)
        {
            ListBoxOnDcLikeSessionBegin(hWnd, callerMod);
        }

        static void OnListBoxCompatEnd(HWND hWnd, HMODULE callerMod)
        {
            ListBoxOnDcLikeSessionEnd(hWnd, callerMod);
        }

        static void OnListBoxGetDCBegin(HWND hWnd, HMODULE callerMod)
        {
            ListBoxOnDcLikeSessionBegin(hWnd, callerMod);
        }

        static void OnListBoxGetDCEnd(HWND hWnd, HMODULE callerMod)
        {
            ListBoxOnDcLikeSessionEnd(hWnd, callerMod);
        }

        [[nodiscard]] static bool IsWinFormsListBoxPaintActive() noexcept
        {
            return ListBoxHooksSessionActive()
                && g_lbSessionWinForms
                && IsListBoxPaintSessionHost();
        }
    }

    void Prepare() {}

    // Предпрогрев IAT-кеша (только чтение: emplace + CacheHookData, без записей).
    // Холодный скан PE уходит из paint-кадра: в Startup (уже загруженные)
    // и в подписку ModuleHandler (поздняя подгрузка, под loader-lock безопасно).
    static void PrewarmModule(void* mod) noexcept
    {
        if (!mod) return;
        auto lock = g_overallLock.lock_exclusive();
        auto [it, ins] = g_overallMap.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(mod),
            std::forward_as_tuple(g_dep, mod));
        if (ins)
            it->second.CacheHookData();
    }

    // Только свои модули — чужие не кешируем (карта не раздувается).
    static void PrewarmForModule(HMODULE mod, const wchar_t* base) noexcept
    {
        if (!mod || !base) return;
        if (mod == GlassCore::ModuleHandler::g_entryPointModule)
        {
            PrewarmModule(mod);
            return;
        }
        if (_wcsicmp(base, L"comctl32.dll") != 0 && _wcsicmp(base, L"uxtheme.dll") != 0
            && _wcsicmp(base, L"main.cpl") != 0 && _wcsicmp(base, L"gdiplus.dll") != 0
            && _wcsicmp(base, L"System.Windows.Forms.ni.dll") != 0)
            return;
        PrewarmModule(mod);
    }

    void Startup()
    {
        GlassCore::PaintDispatcher::RegisterPaintProvider({ OnBeginPaintGlobal, OnEndPaintGlobal });
        GlassCore::PaintDispatcher::RegisterGetDCProvider({ OnListBoxGetDCBegin, OnListBoxGetDCEnd });
        GlassCore::PaintDispatcher::RegisterCompatDCProvider({ OnListBoxCompatBegin, OnListBoxCompatEnd });
        PrewarmModule(GlassCore::ModuleHandler::g_comctl32New);
        PrewarmModule(GlassCore::ModuleHandler::g_comctl32Legacy);
        if (HMODULE const ux = GetModuleHandleW(L"uxtheme.dll"))
            PrewarmModule(ux);
        if (HMODULE const mc = GetModuleHandleW(L"main.cpl"))
            PrewarmModule(mc);
        if (HMODULE const formsNi = GetModuleHandleW(L"System.Windows.Forms.ni.dll"))
            PrewarmModule(formsNi);
        if (HMODULE const gdiplus = GetModuleHandleW(L"gdiplus.dll"))
            PrewarmModule(gdiplus);
        GlassCore::ModuleHandler::RegisterModulePrewarmHandler(PrewarmForModule);
    }

    bool IsInsideWinFormsListBoxPaint() noexcept
    {
        return IsWinFormsListBoxPaintActive();
    }

    void Shutdown()
    {
        g_shutdown.store(true, std::memory_order_seq_cst);
        DisableAllOverallHooks();
        g_lbSessionRef = 0;
        g_lbSessionMod = nullptr;
        g_lbDcLikeArmStack.clear();
        g_shutdown.store(false, std::memory_order_seq_cst);
    }
}
