// Родительский ComboBox (класс ComboBox / .COMBOBOX) — отдельно от ComboLBox (popup) и ComboLBoxHooks.
// Сессия при ComboBox; тела по мотивам ComboLBoxHooks для comctl32.
// Тема Combobox: DrawThemeBackground → offscreen + GdiAlphaBlend (та же альфа, что SysTabControl32).
// Исключение: подстрок msctls_progress32→ComboBoxEx32→ComboBox (и ComboLBox) — без сессии / только orig в хуках.

#include "CorePch.h"
#include <Hooks/ComboBoxControl.h>
#include <Hooks/forms/ComboFormsNiDtbAccess.h>
#include <Hooks/forms/FormsHooks.h>
#include <Hooks/ScrollBarDrawTheme.h>
#include <Hooks/ScrollBarTheme.h>
#include <Subsystems/PaintDispatcher.h>
#include <Subsystems/RenderText.h>
#include <Helpers/ThemeHelper.h>
#include <Subsystems/ThemePaintRegistry.h>
#include <Core/Intercept/HookDispatcher.h>

#include <Module/ModuleHandler.h>
#include <GlassControlKind.h>
#include <wil.h>
#include <uxtheme.h>
#include <WinApiUtils.h>
#include <Hooks/ComboAddressPieSkip.h>
#include <DebugLog.h>

#include <optional>
#include <vssym32.h>

#include <detours.h>

using namespace GlassCore::Hooks;
using namespace std::literals;

namespace GlassCore::ComboBoxControl
{
    namespace
    {
        thread_local HMODULE g_activePaintMod = nullptr;
        thread_local HWND    g_comboHwnd = nullptr;
        static std::atomic<bool> g_shutdown{ false };

        // Дедуп: два идентичных DTB подряд (тот же part/state/rect) → skip, иначе второй проход ломает стекло.
        // Сужать rect part=5 нельзя: подложка дроп-зоны рисуется в полном CP_READONLY, part=6 лишь стрелка/рамка.
        struct ComboBoxDtbDedupKey
        {
            HWND   hWnd{};
            HTHEME hTheme{};
            int    partId{};
            int    stateId{};
            RECT   rect{};

            [[nodiscard]] bool operator==(const ComboBoxDtbDedupKey& o) const noexcept
            {
                return hWnd == o.hWnd && hTheme == o.hTheme && partId == o.partId && stateId == o.stateId
                    && EqualRect(&rect, &o.rect);
            }
        };

        // Только «поле» (не дроп): дубликат part=5 не трогаем для part=6 — иначе перезапись ключа и пропуск лишнего на кнопке.
        thread_local std::optional<ComboBoxDtbDedupKey> g_comboDtbDedupField;

        [[nodiscard]] static bool IsComboBoxDropButtonThemePart(int iPartId) noexcept
        {
            switch (iPartId)
            {
            case CP_DROPDOWNBUTTON:
            case CP_DROPDOWNBUTTONRIGHT:
            case CP_DROPDOWNBUTTONLEFT:
                return true;
            default:
                return false;
            }
        }

        static bool StrEq(LPCWSTR a, LPCWSTR b) { return _wcsicmp(a, b) == 0; }

        static bool IsComboBoxSessionKind(GlassShared::GlassControlKind k) noexcept
        {
            using GK = GlassShared::GlassControlKind;
            return k == GK::ComboBox || k == GK::WinForms_ComboBox;
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

        // WinForms .COMBOBOX: DTB/DTT рисует comctl32, не caller BeginPaint (forms.ni).
        [[nodiscard]] static HMODULE ResolveComboComctlHookModule(HMODULE callerMod) noexcept
        {
            auto* g_new = GlassCore::ModuleHandler::g_comctl32New;
            auto* g_legacy = GlassCore::ModuleHandler::g_comctl32Legacy;
            if (callerMod == g_new || callerMod == g_legacy)
                return callerMod;
            return g_new ? g_new : g_legacy;
        }

        static bool CbxSessionActive() noexcept { return g_activePaintMod != nullptr; }

        int      WINAPI Overall_MyDrawTextW(HDC, LPCWSTR, int, LPRECT, UINT);
        int      WINAPI Overall_MyDrawTextExW(HDC, LPWSTR, int, LPRECT, UINT, LPDRAWTEXTPARAMS);
        BOOL     WINAPI Overall_MyExtTextOutW(HDC, int, int, UINT, const RECT*, LPCWSTR, UINT, const INT*);
        COLORREF WINAPI Overall_MySetBkColor(HDC, COLORREF);
        HBRUSH   WINAPI Overall_MyCreateSolidBrush(COLORREF);
        HGDIOBJ  WINAPI Overall_MySelectObject(HDC, HGDIOBJ);
        HRESULT  WINAPI Overall_MyDrawThemeBg(HTHEME, HDC, int, int, LPCRECT, LPCRECT);
        HRESULT  WINAPI Overall_MyDrawThemeText(HTHEME, HDC, int, int, LPCWSTR, int, DWORD, DWORD, LPCRECT);
        HBRUSH   WINAPI Overall_MyGetSysColorBrush(int);

        enum OverallHookIndex : size_t
        {
            OHK_DRAWTEXT = 0,
            OHK_DRAWTEXTEX,
            OHK_EXTTEXTOUT,
            OHK_SETBKCOLOR,
            OHK_CREATESOLIDBRUSH,
            OHK_SELECTOBJECT,
            OHK_DRAWTHEMEBG,
            OHK_DRAWTHEMETEXT,
            OHK_GETSYSCOLORBRUSH,
            OHK_COUNT
        };

        inline const HookDispatcherDependency<OHK_COUNT, 3> g_dep
        { {
            std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
                "DrawTextW", reinterpret_cast<PVOID>(Overall_MyDrawTextW) },
            std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
                "DrawTextExW", reinterpret_cast<PVOID>(Overall_MyDrawTextExW) },
            std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv },
                "ExtTextOutW", reinterpret_cast<PVOID>(Overall_MyExtTextOutW) },
            std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv },
                "SetBkColor", reinterpret_cast<PVOID>(Overall_MySetBkColor) },
            std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv },
                "CreateSolidBrush", reinterpret_cast<PVOID>(Overall_MyCreateSolidBrush) },
            std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv },
                "SelectObject", reinterpret_cast<PVOID>(Overall_MySelectObject) },
            std::tuple{ std::array{ "uxtheme.dll"sv, "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv, ""sv },
                "DrawThemeBackground", reinterpret_cast<PVOID>(Overall_MyDrawThemeBg) },
            std::tuple{ std::array{ "uxtheme.dll"sv, "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv, ""sv },
                "DrawThemeText", reinterpret_cast<PVOID>(Overall_MyDrawThemeText) },
            std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
                "GetSysColorBrush", reinterpret_cast<PVOID>(Overall_MyGetSysColorBrush) },
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

        static void EnableComboBoxHooksOnComctlModule(HMODULE comctlMod, bool enable)
        {
            if (!comctlMod)
                return;
            EnableHooksOnModule(comctlMod, OHK_DRAWTEXTEX, enable);
            EnableHooksOnModule(comctlMod, OHK_EXTTEXTOUT, enable);
            EnableHooksOnModule(comctlMod, OHK_DRAWTHEMEBG, enable);
            EnableHooksOnModule(comctlMod, OHK_DRAWTHEMETEXT, enable);
            EnableHooksOnModule(comctlMod, OHK_SELECTOBJECT, enable);
            EnableHooksOnModule(comctlMod, OHK_SETBKCOLOR, enable);
            EnableHooksOnModule(comctlMod, OHK_CREATESOLIDBRUSH, enable);
            EnableHooksOnModule(comctlMod, OHK_GETSYSCOLORBRUSH, enable);
        }

        static void EnableComboBoxComdlgHooks(bool enable) noexcept
        {
            if (PVOID base = GetModuleHandleW(L"COMDLG32.dll"))
            {
                EnableHooksOnModule(base, OHK_EXTTEXTOUT, enable);
                EnableHooksOnModule(base, OHK_SETBKCOLOR, enable);
                EnableHooksOnModule(base, OHK_GETSYSCOLORBRUSH, enable);
                EnableHooksOnModule(base, OHK_DRAWTEXT, enable);
                EnableHooksOnModule(base, OHK_DRAWTEXTEX, enable);
                EnableHooksOnModule(base, OHK_CREATESOLIDBRUSH, enable);
            }
        }

        [[nodiscard]] static HRESULT InvokeComboDrawThemeBackground(
            Shared::WinApiUtils::DrawThemeBackground_pfn orig,
            HMODULE callerMod,
            HTHEME hTheme,
            HDC hdc,
            int iPartId,
            int iStateId,
            LPCRECT pRect,
            LPCRECT pClipRect) noexcept
        {
            if (!orig)
                return DrawThemeBackground(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

            if (!CbxSessionActive())
                return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

            if (!pRect || IsRectEmpty(pRect))
                return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

            if (ComboAddressPieSkip::IsAddressPieComboBoxEx32(g_comboHwnd))
                return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

            ComboBoxDtbDedupKey dedupKey{};
            dedupKey.hWnd    = g_comboHwnd;
            dedupKey.hTheme  = hTheme;
            dedupKey.partId  = iPartId;
            dedupKey.stateId = iStateId;
            dedupKey.rect    = *pRect;

            const bool dropPart = IsComboBoxDropButtonThemePart(iPartId);
            if (!dropPart && g_comboDtbDedupField && *g_comboDtbDedupField == dedupKey)
            {
                DebugLog(DBG_NORMAL,
                    L"[GlassCore][ComboBoxControl] InvokeComboDrawThemeBg: double-DTB dedup(field) → skip caller=%p "
                    L"part=%d state=%d rect=[%d,%d,%d,%d]",
                    callerMod,
                    iPartId,
                    iStateId,
                    pRect->left,
                    pRect->top,
                    pRect->right,
                    pRect->bottom);
                return S_OK;
            }

            const BYTE effAlpha = GlassCore::ThemePaint::GetEffectiveDrawThemeConstantAlphaForSession(
                GlassShared::GlassGetWindowKind(g_comboHwnd),
                g_comboHwnd);

            DebugLog(DBG_NORMAL,
                L"[GlassCore][ComboBoxControl] InvokeComboDrawThemeBg: caller=%p part=%d state=%d "
                L"rect=[%d,%d,%d,%d] effAlpha=%u",
                callerMod,
                iPartId,
                iStateId,
                pRect->left,
                pRect->top,
                pRect->right,
                pRect->bottom,
                static_cast<unsigned>(effAlpha));

            if (dropPart)
            {
                if (effAlpha == 0)
                {
                    RECT fill{};
                    if (pClipRect)
                    {
                        if (!IntersectRect(&fill, pRect, pClipRect))
                            return S_OK;
                    }
                    else
                        fill = *pRect;
                    PatBlt(hdc, fill.left, fill.top, fill.right - fill.left, fill.bottom - fill.top, BLACKNESS);
                    return S_OK;
                }
                DebugLog(DBG_NORMAL,
                    L"[GlassCore][ComboBoxControl] InvokeComboDrawThemeBg: drop part → orig (без DIB+AlphaBlend)");
                return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
            }

            const bool alphaOk = Shared::WinApiUtils::DrawThemeBackgroundWithConstantAlpha(
                orig,
                hTheme,
                hdc,
                iPartId,
                iStateId,
                pRect,
                pClipRect,
                effAlpha);

            g_comboDtbDedupField = dedupKey;

            if (alphaOk)
                return S_OK;
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
        }

        int WINAPI Overall_MyDrawTextW(
            HDC hdc, LPCWSTR lpchText, int cchText, LPRECT lprc, UINT format)
        {
            HMODULE callerMod = GetCallerModule(_ReturnAddress());
            auto orig = GetOverallOrigSafe<OHK_DRAWTEXT, decltype(&DrawTextW)>(callerMod);

            if (!orig)
                return DrawTextW(hdc, lpchText, cchText, lprc, format);

            if (!CbxSessionActive())
                return orig(hdc, lpchText, cchText, lprc, format);

            if (format & DT_CALCRECT)
                return orig(hdc, lpchText, cchText, lprc, format);

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

            if (!CbxSessionActive())
                return orig(hdc, lpchText, cchText, lprc, format, lpdtp);

            if (format & DT_CALCRECT)
                return orig(hdc, lpchText, cchText, lprc, format, lpdtp);

            int result = 0;
            if (!FAILED(GlassCore::Text::DrawTextWithAlpha(hdc, lpchText, cchText, lprc, format, result)))
                return result;

            return orig(hdc, lpchText, cchText, lprc, format, lpdtp);
        }

        BOOL WINAPI Overall_MyExtTextOutW(
            HDC hdc, int x, int y, UINT options, const RECT* lprect,
            LPCWSTR lpString, UINT c, const INT* lpDx)
        {
            HMODULE callerMod = GetCallerModule(_ReturnAddress());
            auto orig = GetOverallOrigSafe<OHK_EXTTEXTOUT, decltype(&ExtTextOutW)>(callerMod);

            if (!CbxSessionActive())
                return orig ? orig(hdc, x, y, options, lprect, lpString, c, lpDx)
                : ExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);

            if ((options & ETO_CLIPPED) && lprect)
            {
                RECT client{};
                if (g_comboHwnd)
                    GetClientRect(g_comboHwnd, &client);
                IntersectClipRect(hdc,
                    (std::max)(static_cast<int>(x), static_cast<int>(lprect->left)),
                    (std::max)(static_cast<int>(y), static_cast<int>(lprect->top)),
                    lprect->right,
                    client.bottom ? (std::min)(client.bottom, lprect->bottom) : lprect->bottom);
            }

            if ((options & ETO_OPAQUE) && lprect)
            {
                PatBlt(hdc, lprect->left, lprect->top,
                    lprect->right - lprect->left, lprect->bottom - lprect->top, BLACKNESS);

                const COLORREF bk = GetBkColor(hdc);
                if (bk == GetSysColor(COLOR_HIGHLIGHT))
                {
                    HBRUSH hi = Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(bk, 255);
                    if (hi)
                    {
                        FillRect(hdc, lprect, hi);
                        DeleteObject(hi);
                    }
                }
            }

            if (c && lpString && !(options & ETO_GLYPH_INDEX))
            {
                RECT textRect{};
                if (lprect)
                    textRect = *lprect;
                else
                    GetClipBox(hdc, &textRect);
                textRect.left = x;
                textRect.top = y;

                DWORD dwTextFlags = DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOCLIP | DT_NOPREFIX;
                if (options & ETO_RTLREADING)
                    dwTextFlags |= DT_RTLREADING;

                int result = 0;
                if (!FAILED(GlassCore::Text::DrawTextWithAlpha(
                        hdc, lpString, static_cast<int>(c), &textRect, dwTextFlags, result)))
                {
                    if (options & ETO_CLIPPED)
                        SelectClipRgn(hdc, nullptr);
                    return TRUE;
                }
            }

            if ((options & ETO_OPAQUE) && lprect && !c)
            {
                if (options & ETO_CLIPPED)
                    SelectClipRgn(hdc, nullptr);
                return TRUE;
            }

            UINT opts = options;
            if (opts & ETO_OPAQUE)
            {
                const COLORREF bk = GetBkColor(hdc);
                if (!bk
                    || bk == GetSysColor(COLOR_WINDOW)
                    || bk == GetSysColor(COLOR_BTNFACE)
                    || bk == 0xffffff)
                {
                    opts &= ~static_cast<UINT>(ETO_OPAQUE);
                }
            }

            const BOOL res = orig
                ? orig(hdc, x, y, opts, lprect, lpString, c, lpDx)
                : ExtTextOutW(hdc, x, y, opts, lprect, lpString, c, lpDx);
            if (options & ETO_CLIPPED)
                SelectClipRgn(hdc, nullptr);
            return res;
        }

        HBRUSH WINAPI Overall_MyGetSysColorBrush(int nIndex)
        {
            HMODULE callerMod = GetCallerModule(_ReturnAddress());
            auto orig = GetOverallOrigSafe<OHK_GETSYSCOLORBRUSH, decltype(&GetSysColorBrush)>(callerMod);

            if (CbxSessionActive() && nIndex == COLOR_WINDOW)
                return GetStockBrush(BLACK_BRUSH);

            return orig ? orig(nIndex) : GetSysColorBrush(nIndex);
        }

        COLORREF WINAPI Overall_MySetBkColor(HDC hdc, COLORREF color)
        {
            HMODULE callerMod = GetCallerModule(_ReturnAddress());
            auto orig = GetOverallOrigSafe<OHK_SETBKCOLOR, decltype(&SetBkColor)>(callerMod);

            if (!orig)
                return SetBkColor(hdc, color);

            if (CbxSessionActive())
            {
                if (color == GetSysColor(COLOR_WINDOW) || color == GetSysColor(COLOR_BTNFACE))
                    color = 0;
            }

            return orig(hdc, color);
        }

        HBRUSH WINAPI Overall_MyCreateSolidBrush(COLORREF color)
        {
            HMODULE callerMod = GetCallerModule(_ReturnAddress());
            auto orig = GetOverallOrigSafe<OHK_CREATESOLIDBRUSH, decltype(&CreateSolidBrush)>(callerMod);

            if (!orig)
                return CreateSolidBrush(color);

            if (CbxSessionActive())
            {
                if (color == GetSysColor(COLOR_HIGHLIGHT))
                    return Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(color, 255);

                if (color == GetSysColor(COLOR_WINDOW) || color == 0xffffff ||
                    color == GetSysColor(COLOR_BTNFACE))
                    color = 0;
            }

            return orig(color);
        }

        HGDIOBJ WINAPI Overall_MySelectObject(HDC hdc, HGDIOBJ h)
        {
            HMODULE callerMod = GetCallerModule(_ReturnAddress());
            auto orig = GetOverallOrigSafe<OHK_SELECTOBJECT, decltype(&SelectObject)>(callerMod);

            if (!orig)
                return SelectObject(hdc, h);

            if (CbxSessionActive())
            {
                LOGPEN pn{};
                if (GetObject(h, sizeof(pn), &pn))
                {
                    if (pn.lopnColor == GetSysColor(COLOR_WINDOW) ||
                        pn.lopnColor == GetSysColor(COLOR_BTNFACE))
                        return 0;
                }
            }

            return orig(hdc, h);
        }

        HRESULT WINAPI Overall_MyDrawThemeBg(
            HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
            LPCRECT pRect, LPCRECT pClipRect)
        {
            HMODULE callerMod = GetCallerModule(_ReturnAddress());
            auto orig = GetOverallOrigSafe<OHK_DRAWTHEMEBG, decltype(&DrawThemeBackground)>(callerMod);

            if (!orig)
                return DrawThemeBackground(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

            if (g_shutdown.load(std::memory_order_acquire))
                return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

            if (!CbxSessionActive())
                return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

            WCHAR themeClass[MAX_PATH + 1]{};
            const bool haveClass = SUCCEEDED(
                Shared::ThemeHelper::GetThemeClass(hTheme, themeClass, MAX_PATH));

            if (haveClass && StrEq(themeClass, L"ScrollBar"))
            {
                auto sbOrig = GlassCore::ScrollBarTheme::GetComctlScrollBarDrawThemeOrig(callerMod);
                auto dtbOrig = sbOrig ? sbOrig : orig;
                if (!dtbOrig)
                    return DrawThemeBackground(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

                GlassCore::ScrollBarDrawTheme::LogScrollBarDrawThemeCall(
                    L"[GlassCore][ComboBoxControl]",
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

            if (haveClass && StrEq(themeClass, L"Combobox"))
            {
                return InvokeComboDrawThemeBackground(
                    reinterpret_cast<Shared::WinApiUtils::DrawThemeBackground_pfn>(orig),
                    callerMod,
                    hTheme,
                    hdc,
                    iPartId,
                    iStateId,
                    pRect,
                    pClipRect);
            }

            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
        }

        HRESULT WINAPI Overall_MyDrawThemeText(
            HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
            LPCWSTR pszText, int cchText, DWORD dwTextFlags,
            DWORD dwTextFlags2, LPCRECT pRect)
        {
            HMODULE callerMod = GetCallerModule(_ReturnAddress());
            auto orig = GetOverallOrigSafe<OHK_DRAWTHEMETEXT, decltype(&DrawThemeText)>(callerMod);

            if (!orig)
                return DrawThemeText(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, dwTextFlags2, pRect);

            if (!CbxSessionActive())
                return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, dwTextFlags2, pRect);

            WCHAR themeClassName[MAX_PATH + 1]{};
            if (SUCCEEDED(Shared::ThemeHelper::GetThemeClass(hTheme, themeClassName, MAX_PATH)))
            {
                if (!IsRectEmpty(pRect) && StrEq(themeClassName, L"Combobox"))
                {
                    if (ComboAddressPieSkip::IsAddressPieComboBoxEx32(g_comboHwnd))
                        return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, dwTextFlags2, pRect);

                    RECT lpRect = *pRect;
                    DTTOPTS options{ sizeof(DTTOPTS), DTT_COMPOSITED };
                    return GlassCore::Text::DrawThemeContent(
                        hdc, *pRect, nullptr, nullptr, 0,
                        [&](HDC memoryDC, HPAINTBUFFER, RGBQUAD*, int)
                        {
                            DrawThemeTextEx(hTheme, memoryDC, iPartId, iStateId,
                                pszText, cchText, dwTextFlags, &lpRect, &options);
                        });
                }
            }

            return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, dwTextFlags2, pRect);
        }

        static void EnableComboBoxHooksForSession(
            GlassShared::GlassControlKind kind,
            HMODULE comctlMod,
            bool enable) noexcept;

        static void OnBeginPaintGlobal(HWND hWnd, HMODULE callerMod)
        {
            using namespace GlassShared;
            if (g_shutdown.load(std::memory_order_acquire))
                return;

            const GlassControlKind kind = GlassGetWindowKind(hWnd);
            if (!IsComboBoxSessionKind(kind) || g_activePaintMod)
                return;

            auto* g_new = GlassCore::ModuleHandler::g_comctl32New;
            auto* g_legacy = GlassCore::ModuleHandler::g_comctl32Legacy;
            if (callerMod != g_new && callerMod != g_legacy)
                return;

            const HMODULE comctlMod = ResolveComboComctlHookModule(callerMod);
            if (!comctlMod)
                return;

            GlassCore::ScrollBarTheme::EnsureComctlScrollBarIatIfComctlCaller(hWnd, callerMod);
            g_comboDtbDedupField.reset();
            g_activePaintMod = comctlMod;
            g_comboHwnd = hWnd;
            EnableComboBoxHooksForSession(kind, comctlMod, true);
        }

        static void OnEndPaintGlobal(HWND hWnd, HMODULE /*callerMod*/)
        {
            using namespace GlassShared;
            const GlassControlKind kind = GlassGetWindowKind(hWnd);
            if (!IsComboBoxSessionKind(kind))
                return;
            if (!g_activePaintMod)
                return;

            EnableComboBoxHooksForSession(kind, g_activePaintMod, false);
            g_comboDtbDedupField.reset();
            g_activePaintMod = nullptr;
            g_comboHwnd = nullptr;
        }

        static void EnableComboBoxHooksForSession(
            GlassShared::GlassControlKind kind,
            HMODULE comctlMod,
            bool enable) noexcept
        {
            EnableComboBoxHooksOnComctlModule(comctlMod, enable);
            EnableComboBoxComdlgHooks(enable);
            if (kind == GlassShared::GlassControlKind::WinForms_ComboBox
                && GetModuleHandleW(L"System.Windows.Forms.ni.dll"))
            {
                GlassCore::FormsHooks::DrawThemeBackgroundAttach(enable);
            }
        }

        [[nodiscard]] bool FormsNiDtb_IsSessionActiveImpl() noexcept
        {
            return CbxSessionActive()
                && GlassShared::GlassGetWindowKind(g_comboHwnd)
                    == GlassShared::GlassControlKind::WinForms_ComboBox;
        }

        [[nodiscard]] HRESULT FormsNiDtb_TryDrawThemeBackgroundImpl(
            Shared::WinApiUtils::DrawThemeBackground_pfn orig,
            HMODULE callerMod,
            HTHEME hTheme,
            HDC hdc,
            int iPartId,
            int iStateId,
            LPCRECT pRect,
            LPCRECT pClipRect) noexcept
        {
            return InvokeComboDrawThemeBackground(
                orig, callerMod, hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
        }
    }

    namespace FormsNiDtbAccess
    {
        bool IsWinFormsComboDrawThemeSessionActive() noexcept
        {
            return FormsNiDtb_IsSessionActiveImpl();
        }

        HRESULT TryDrawThemeBackground(
            Shared::WinApiUtils::DrawThemeBackground_pfn orig,
            HMODULE callerMod,
            HTHEME hTheme,
            HDC hdc,
            int iPartId,
            int iStateId,
            LPCRECT pRect,
            LPCRECT pClipRect) noexcept
        {
            return FormsNiDtb_TryDrawThemeBackgroundImpl(
                orig, callerMod, hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
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
        g_comboDtbDedupField.reset();
        g_activePaintMod = nullptr;
        g_comboHwnd = nullptr;
        g_shutdown.store(false, std::memory_order_seq_cst);
    }
}
