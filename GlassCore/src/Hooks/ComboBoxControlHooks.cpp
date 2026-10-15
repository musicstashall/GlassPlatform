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
#include <Subsystems/GdiHookUtils.h>
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
        thread_local HMODULE g_activeHotMod = nullptr;
        thread_local HWND    g_comboHwnd = nullptr;
        // BWM Paint/ComboBox (caller == comctl32 v6): EXTTEXTOUT точки входа
        // включён только в paint-сессии; флаг — для парного снятия в EndPaint.
        thread_local bool   g_entryPointExtTextOutOn = false;
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

        // BWM ComboBoxHooks::WorkExtTextOutW: caller MFC42u.dll с inset-rect — заливка alpha-кистью.
        [[nodiscard]] static bool IsMFC42uModule(HMODULE mod) noexcept
        {
            if (!mod)
                return false;
            wchar_t path[MAX_PATH]{};
            if (GetModuleFileNameW(mod, path, _countof(path)) == 0)
                return false;
            const wchar_t* base = wcsrchr(path, L'\\');
            base = base ? base + 1 : path;
            return _wcsicmp(base, L"MFC42u.dll") == 0;
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

        static bool CbxSessionActive() noexcept { return g_activePaintMod != nullptr || g_activeHotMod != nullptr; }

        // Имя caller-модуля без пути — узкая проверка своих сценариев (паритет ListView/ComboLBox).
        [[nodiscard]] static bool CallerBaseIs(HMODULE caller, LPCWSTR base) noexcept
        {
            if (!caller || !base)
                return false;
            wchar_t path[MAX_PATH]{};
            if (GetModuleFileNameW(caller, path, _countof(path)) == 0)
                return false;
            const wchar_t* p = wcsrchr(path, L'\\');
            const wchar_t* name = p ? p + 1 : path;
            return _wcsicmp(name, base) == 0;
        }

        // Штрихи sysmon на родительском ComboBox — opaque (паритет BWM GDI+ Color(255,...)).
        constexpr BYTE kSysmonComboBoxStrokeAlpha = 255;

        // sysmon рисует в memory-DC (WindowFromDC=NULL) — HWND берём из сессии PaintDispatcher.
        // Возвращает HWND только для ComboBox-сессии, иначе nullptr → caller уходит в orig.
        [[nodiscard]] static HWND HwndForComboBoxDraw(HDC hdc) noexcept
        {
            if (!hdc)
                return nullptr;
            HWND w = WindowFromDC(hdc);
            if (w)
                return IsComboBoxSessionKind(GlassShared::GlassGetWindowKind(w)) ? w : nullptr;
            w = GlassCore::PaintDispatcher::GetPaintSessionHostHwnd();
            if (w && IsWindow(w) && IsComboBoxSessionKind(GlassShared::GlassGetWindowKind(w)))
                return w;
            if (g_comboHwnd && IsWindow(g_comboHwnd))
                return g_comboHwnd;
            return nullptr;
        }

        int      WINAPI Overall_MyDrawTextW(HDC, LPCWSTR, int, LPRECT, UINT);
        int      WINAPI Overall_MyDrawTextExW(HDC, LPWSTR, int, LPRECT, UINT, LPDRAWTEXTPARAMS);
        BOOL     WINAPI Overall_MyExtTextOutW(HDC, int, int, UINT, const RECT*, LPCWSTR, UINT, const INT*);
        COLORREF WINAPI Overall_MySetBkColor(HDC, COLORREF);
        HBRUSH   WINAPI Overall_MyCreateSolidBrush(COLORREF);
        HGDIOBJ  WINAPI Overall_MyGetStockObject(int);
        BOOL     WINAPI Overall_MyLineTo(HDC, int, int);
        HGDIOBJ  WINAPI Overall_MySelectObject(HDC, HGDIOBJ);
        HRESULT  WINAPI Overall_MyDrawThemeBg(HTHEME, HDC, int, int, LPCRECT, LPCRECT);
        HRESULT  WINAPI Overall_MyDrawThemeText(HTHEME, HDC, int, int, LPCWSTR, int, DWORD, DWORD, LPCRECT);
        HBRUSH   WINAPI Overall_MyGetSysColorBrush(int);
        BOOL     WINAPI Overall_MyTextOutW(HDC, int, int, LPCWSTR, int);

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
            OHK_TEXTOUT,
            OHK_GETSTOCKOBJECT,
            OHK_LINETO,
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
            std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv },
                "TextOutW", reinterpret_cast<PVOID>(Overall_MyTextOutW) },
            std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv },
                "GetStockObject", reinterpret_cast<PVOID>(Overall_MyGetStockObject) },
            std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv },
                "LineTo", reinterpret_cast<PVOID>(Overall_MyLineTo) },
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
                EnableHooksOnModule(base, OHK_TEXTOUT, enable);
            }
        }

        // BWM PaintHandler (ctrl-ветка ComboBox): IAT ExtTextOutW на MFC42u.dll.
        // Без этого слота вызовы из MFC42u (диалоги mmc, diskmgmt) идут мимо сессии.
        static void EnableComboBoxMfcHooks(bool enable) noexcept
        {
            if (PVOID base = GetModuleHandleW(L"MFC42u.dll"))
                EnableHooksOnModule(base, OHK_EXTTEXTOUT, enable);
        }

        // BWM PaintHandler (paint-сессия, ветка ComboBox, caller == comctl32 v6):
        // IAT ExtTextOutW на точке входа — cleanmgr.exe и аналоги рисуют текст
        // поля комбобокса своим кодом, без этого слота вызовы идут мимо сессии.
        // Только paint-канал, только OHK_EXTTEXTOUT (scope задачи).
        static void EnableComboBoxEntryPointHooks(bool enable) noexcept
        {
            if (void* ep = GlassCore::ModuleHandler::g_entryPointModule)
                EnableHooksOnModule(ep, OHK_EXTTEXTOUT, enable);
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

        // Паритет BWM ComboBoxHooks::WorkExtTextOutW: фон — PatBlt(BLACKNESS) + FillRect
        // alpha-кистью при MFC42u-inset либо цветном BkColor; текст — DrawTextWithAlpha по
        // client-rect с флагами GetTextAlign; ранний выход и strip ETO_OPAQUE — дословно BWM.
        // Отличия от BWM (архитектура PaintDispatcher): gate — CbxSessionActive вместо
        // WndList::IsDataAttach, HWND — сессионный g_comboHwnd вместо LWnd(hdc),
        // orig — per-module слот (прямой вызов при отсутствии), гард lpString.
        BOOL WINAPI Overall_MyExtTextOutW(
            HDC hdc, int x, int y, UINT options, const RECT* lprect,
            LPCWSTR lpString, UINT c, const INT* lpDx)
        {
            HMODULE callerMod = GetCallerModule(_ReturnAddress());
            auto orig = GetOverallOrigSafe<OHK_EXTTEXTOUT, decltype(&ExtTextOutW)>(callerMod);
            auto callOrig = [&](UINT opts) -> BOOL
            {
                return orig ? orig(hdc, x, y, opts, lprect, lpString, c, lpDx)
                    : ExtTextOutW(hdc, x, y, opts, lprect, lpString, c, lpDx);
            };

            if (!CbxSessionActive())
                return callOrig(options);

            RECT client{};
            if (g_comboHwnd)
                GetClientRect(g_comboHwnd, &client);

            if ((options & (ETO_OPAQUE | ETO_CLIPPED)) && lprect)
            {
                if (options & ETO_CLIPPED)
                {
                    IntersectClipRect(hdc,
                        (std::max)(static_cast<int>(x), static_cast<int>(lprect->left)),
                        (std::max)(static_cast<int>(y), static_cast<int>(lprect->top)),
                        lprect->right,
                        (std::min)(client.bottom, lprect->bottom));
                }

                PatBlt(hdc, lprect->left, lprect->top,
                    lprect->right - lprect->left, lprect->bottom - lprect->top, BLACKNESS);

                if (options & ETO_OPAQUE)
                {
                    const COLORREF bk = GetBkColor(hdc);
                    // Дословно BWM ComboBoxHooks::WorkExtTextOutW — без лишних гардов.
                    const bool mfcInset = IsMFC42uModule(callerMod)
                        && client.left != lprect->left && client.right != lprect->right;
                    const bool coloredBk = bk
                        && bk != GetSysColor(COLOR_WINDOW)
                        && bk != 0xffffff;
                    if (mfcInset || coloredBk)
                    {
                        HBRUSH hi = Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(bk, 255);
                        if (hi)
                        {
                            FillRect(hdc, lprect, hi);
                            DeleteObject(hi);
                        }
                    }
                }
            }

            if (options == ETO_GLYPH_INDEX || options == ETO_IGNORELANGUAGE
                || (options == ETO_OPAQUE && !c) || !c)
            {
                UINT stripped = options;
                if (stripped != ETO_OPAQUE)
                    stripped -= (stripped & ETO_OPAQUE);
                if (stripped == ETO_OPAQUE && !c)
                    return TRUE;
                return callOrig(stripped);
            }

            RECT textRect = client;
            textRect.left = x;
            textRect.top = y;

            int result = 0;
            const bool ok = lpString && !FAILED(GlassCore::Text::DrawTextWithAlpha(
                hdc, lpString, static_cast<int>(c), &textRect, GetTextAlign(hdc), result));
            if (options & ETO_CLIPPED)
                SelectClipRgn(hdc, nullptr);
            if (ok)
                return TRUE;
            return callOrig(options);
        }

        // BWM OverallHooks::MyTextOutW: слот TEXTOUT ставится на COMDLG32 для комбобокса
        // (текст неактивного поля). Фон — HIGHLIGHT→alpha, иначе BLACKNESS; текст всегда
        // через DrawTextWithAlpha по clipbox-rect с флагами GetTextAlign.
        BOOL WINAPI Overall_MyTextOutW(HDC hdc, int x, int y, LPCWSTR lpString, int c)
        {
            HMODULE callerMod = GetCallerModule(_ReturnAddress());
            auto orig = GetOverallOrigSafe<OHK_TEXTOUT, decltype(&TextOutW)>(callerMod);

            if (!CbxSessionActive())
                return orig ? orig(hdc, x, y, lpString, c)
                : TextOutW(hdc, x, y, lpString, c);


            if (GlassShared::GlassGetWindowKind(g_comboHwnd) == GlassShared::GlassControlKind::ComboBox)
            {
                const COLORREF color = GetBkColor(hdc);
                RECT rc{};
                GetClipBox(hdc, &rc);
                if (color == GetSysColor(COLOR_HIGHLIGHT))
                {
                    HBRUSH brush = Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(color, 255);
                    if (brush)
                    {
                        FillRect(hdc, &rc, brush);
                        DeleteObject(brush);
                    }
                }
                else
                {
                    PatBlt(hdc, rc.left, rc.top,
                        rc.right - rc.left, rc.bottom - rc.top, BLACKNESS);
                }
            }

            RECT rc{};
            GetClipBox(hdc, &rc);
            rc.left = x;
            rc.top = y;

            int result = 0;
            const bool ok = lpString && !FAILED(GlassCore::Text::DrawTextWithAlpha(
                hdc, lpString, c, &rc, GetTextAlign(hdc), result));
            if (ok)
                return TRUE;
            return orig ? orig(hdc, x, y, lpString, c)
                : TextOutW(hdc, x, y, lpString, c);
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

            // Лог 2026-10-06: BWM не аттачит SetBkColor на comctl для родительского
            // комбобокса (только ветка ComboLBox цепляет DRAWTEXTEX/EXTTEXTOUT/SELECTOBJECT),
            // ремап WINDOW/BTNFACE→0 отравлял shared-DC (bk=0: чёрные грани и blackout
            // поля при фокус-перерисовках). Passthrough, как в BWM.
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

                // Выбранная линия sysmon на родительском ComboBox — цветная alpha-кисть 255.
                // Паритет BWM OverallHooks::MyCreateSolidBrush для sysmon.ocx и ComboLBoxHooks.
                if (color != 0)
                {
                    HBRUSH alphaBrush = Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(color, 255);
                    if (alphaBrush)
                        return alphaBrush;
                }
            }

            return orig(color);
        }

        // ── Overall: MyGetStockObject — sysmon.ocx на родительском ComboBox ──
        // Паритет BWM OverallHooks::MyGetStockObject: WHITE/DC → BLACK;
        // BLACK/GRAY от sysmon.ocx → alpha-кисти. Иначе — настоящий orig.
        HGDIOBJ WINAPI Overall_MyGetStockObject(int i)
        {
            HMODULE callerMod = GetCallerModule(_ReturnAddress());
            auto orig = GetOverallOrigSafe<OHK_GETSTOCKOBJECT, decltype(&GetStockObject)>(callerMod);

            if (CbxSessionActive())
            {
                if (i == WHITE_BRUSH || i == DC_BRUSH)
                    i = BLACK_BRUSH;
                else if (i == BLACK_BRUSH && CallerBaseIs(callerMod, L"sysmon.ocx"))
                {
                    HBRUSH alphaBrush = Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(0, 255);
                    if (alphaBrush)
                        return alphaBrush;
                }
                else if (i == GRAY_BRUSH && CallerBaseIs(callerMod, L"sysmon.ocx") && orig)
                {
                    HGDIOBJ hStock = orig(i);
                    LOGBRUSH lb{};
                    if (hStock && GetObject(hStock, sizeof(lb), &lb))
                    {
                        HBRUSH alphaBrush = Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(lb.lbColor, 255);
                        if (alphaBrush)
                            return alphaBrush;
                    }
                }
            }

            return orig ? orig(i) : GetStockObject(i);
        }

        // ── Overall: MyLineTo — sysmon.ocx на родительском ComboBox (выбранная линия) ──
        // По мотивам BWM OverallHooks::MyLineTo (ветка sysmon + ComboBox),
        // без GDI+: сплошные и пунктир — LayeredStrokePen::TryCreateStyled alpha 255.
        // Узкое тело: чужие сессии/модули — настоящий orig (IATRegistry first-owner-wins).
        BOOL WINAPI Overall_MyLineTo(HDC hdc, int x, int y)
        {
            HMODULE callerMod = GetCallerModule(_ReturnAddress());
            auto orig = GetOverallOrigSafe<OHK_LINETO, decltype(&LineTo)>(callerMod);

            if (!orig)
                return LineTo(hdc, x, y);
            if (g_shutdown.load(std::memory_order_acquire) || !CbxSessionActive())
                return orig(hdc, x, y);
            if (!CallerBaseIs(callerMod, L"sysmon.ocx"))
                return orig(hdc, x, y);
            if (!HwndForComboBoxDraw(hdc))
                return orig(hdc, x, y);

            HGDIOBJ penObj = GetCurrentObject(hdc, OBJ_PEN);
            LOGPEN lp{};
            if (GetObject(penObj, sizeof(lp), &lp) != sizeof(LOGPEN) || lp.lopnStyle == PS_NULL)
                return orig(hdc, x, y);

            // BWM: INSIDEFRAME → SOLID, ширина 0 → 1
            if (lp.lopnStyle == PS_INSIDEFRAME)
                lp.lopnStyle = PS_SOLID;
            if (lp.lopnWidth.x <= 0)
                lp.lopnWidth.x = 1;

            POINT lppt{};
            if (!GetCurrentPositionEx(hdc, &lppt))
                return orig(hdc, x, y);

            GlassCore::GdiUtils::LayeredStrokePen layeredStroke =
                GlassCore::GdiUtils::LayeredStrokePen::TryCreateStyled(lp, kSysmonComboBoxStrokeAlpha);
            wil::unique_hpen drawPen;
            if (!layeredStroke)
                drawPen = GlassCore::GdiUtils::PenFromLogPen(lp);
            HGDIOBJ hStroke = layeredStroke ? static_cast<HGDIOBJ>(layeredStroke.get())
                                            : static_cast<HGDIOBJ>(drawPen.get());
            if (!hStroke)
                return orig(hdc, x, y);

            wil::unique_hpen nullPen(CreatePen(PS_NULL, 0, 0));
            if (!nullPen)
                return orig(hdc, x, y);

            HGDIOBJ old = SelectObject(hdc, nullPen.get());
            orig(hdc, x, y);
            SelectObject(hdc, hStroke);
            MoveToEx(hdc, lppt.x, lppt.y, nullptr);
            const BOOL r = orig(hdc, x, y);
            SelectObject(hdc, old);
            return r;
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
            // Паритет BWM Paint/ComboBox (caller == comctl32 v6, т.е. g_new):
            // текст поля рисует точка входа (cleanmgr.exe) — цепляем её ExtTextOutW.
            // Тело Overall_MyExtTextOutW гейтится CbxSessionActive, отдельный
            // caller-гейт в теле не нужен.
            if (callerMod && callerMod == g_new && !g_entryPointExtTextOutOn)
            {
                EnableComboBoxEntryPointHooks(true);
                g_entryPointExtTextOutOn = true;
            }
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
            if (g_entryPointExtTextOutOn)
            {
                EnableComboBoxEntryPointHooks(false);
                g_entryPointExtTextOutOn = false;
            }
            g_activePaintMod = nullptr;
            if (!CbxSessionActive())
            {
                g_comboDtbDedupField.reset();
                g_comboHwnd = nullptr;
            }
        }

        // ── GetDC-канал (hot): фокус/хот-перерисовки идут мимо BeginPaint ──
        // Паритет BWM PaintHandler::Dc() — ветка ComboBox + caller COMCTL32 включает
        // то же sysmon-трио (CREATESOLIDBRUSH/GETSTOCKOBJECT/LINETO). CompatibleDc
        // в BWM ветки ComboBox не имеет — не переносим.
        static void OnHotBeginGlobal(HWND hWnd, HMODULE callerMod)
        {
            using namespace GlassShared;
            if (g_shutdown.load(std::memory_order_acquire))
                return;

            const GlassControlKind kind = GlassGetWindowKind(hWnd);
            if (!IsComboBoxSessionKind(kind) || g_activeHotMod)
                return;

            auto* g_new = GlassCore::ModuleHandler::g_comctl32New;
            auto* g_legacy = GlassCore::ModuleHandler::g_comctl32Legacy;
            if (callerMod != g_new && callerMod != g_legacy)
                return;

            const HMODULE comctlMod = ResolveComboComctlHookModule(callerMod);
            if (!comctlMod)
                return;

            GlassCore::ScrollBarTheme::EnsureComctlScrollBarIatIfComctlCaller(hWnd, callerMod);
            g_activeHotMod = comctlMod;
            g_comboHwnd = hWnd;
            EnableComboBoxHooksForSession(kind, comctlMod, true);
        }

        static void OnHotEndGlobal(HWND hWnd, HMODULE /*callerMod*/)
        {
            using namespace GlassShared;
            const GlassControlKind kind = GlassGetWindowKind(hWnd);
            if (!IsComboBoxSessionKind(kind))
                return;
            if (!g_activeHotMod)
                return;

            EnableComboBoxHooksForSession(kind, g_activeHotMod, false);
            g_activeHotMod = nullptr;
            if (!CbxSessionActive())
            {
                g_comboDtbDedupField.reset();
                g_comboHwnd = nullptr;
            }
        }

        // sysmon.ocx — выбранная линия на родительском ComboBox (mmc, Системный монитор).
        // Паритет BWM PaintHandler-ветки ComboBox: CREATESOLIDBRUSH + GETSTOCKOBJECT + LINETO.
        static void EnableComboBoxSysmonHooks(bool enable) noexcept
        {
            if (PVOID base = GetModuleHandleW(L"sysmon.ocx"))
            {
                EnableHooksOnModule(base, OHK_CREATESOLIDBRUSH, enable);
                EnableHooksOnModule(base, OHK_GETSTOCKOBJECT, enable);
                EnableHooksOnModule(base, OHK_LINETO, enable);
            }
        }

        static void EnableComboBoxHooksForSession(
            GlassShared::GlassControlKind kind,
            HMODULE comctlMod,
            bool enable) noexcept
        {
            EnableComboBoxHooksOnComctlModule(comctlMod, enable);
            EnableComboBoxComdlgHooks(enable);
            EnableComboBoxMfcHooks(enable);
            EnableComboBoxSysmonHooks(enable);
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

    // Предпрогрев IAT-кеша (только чтение, без записей).
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
        if (_wcsicmp(base, L"comctl32.dll") != 0 && _wcsicmp(base, L"COMDLG32.dll") != 0
            && _wcsicmp(base, L"MFC42u.dll") != 0 && _wcsicmp(base, L"sysmon.ocx") != 0)
            return;
        PrewarmModule(mod);
    }

    void Startup()
    {
        GlassCore::PaintDispatcher::RegisterPaintProvider({ OnBeginPaintGlobal, OnEndPaintGlobal });
        GlassCore::PaintDispatcher::RegisterGetDCProvider({ OnHotBeginGlobal, OnHotEndGlobal });
        PrewarmModule(GlassCore::ModuleHandler::g_comctl32New);
        PrewarmModule(GlassCore::ModuleHandler::g_comctl32Legacy);
        PrewarmModule(GetModuleHandleW(L"sysmon.ocx"));
        // Точка входа — слот EXTTEXTOUT для paint-сессий ComboBox (cleanmgr.exe):
        // холодный скан PE уходит из первого кадра (модуль уже загружен —
        // ModuleHandler::Startup отработал раньше subsystem Startup).
        PrewarmModule(GlassCore::ModuleHandler::g_entryPointModule);
        GlassCore::ModuleHandler::RegisterModulePrewarmHandler(PrewarmForModule);
    }

    void Shutdown()
    {
        g_shutdown.store(true, std::memory_order_seq_cst);
        DisableAllOverallHooks();
        g_comboDtbDedupField.reset();
        g_activePaintMod = nullptr;
        g_activeHotMod = nullptr;
        g_comboHwnd = nullptr;
        g_shutdown.store(false, std::memory_order_seq_cst);
    }
}
