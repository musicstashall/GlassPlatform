// Хуки для отрисовки кнопок (Button, WinForms .BUTTON).
//
// Кнопки: WinForms .BUTTON и нативный Button; USER32 без темы → ExtTextOut на gdi32full.
// DrawThemeBackground «Button»: полупрозрачный хром для push-кнопок (альфа — ThemePaintRegistry), без группы/чекбокса/радио.
// PaintDispatcher: callerMod из BeginPaint / GetDC, стек сессий + симметричный enable/disable.
// GetDC/ReleaseDC — как в BWM PaintHandler::Dc (comctl32+Button): WM_ERASEBKGND, смена вкладки,
// закрытие модалки часто рисуют без BeginPaint.
// Guard в телах хуков: g_buttonSessionStack.empty().
// BitBlt (comctl32 v6): на время сессии — внешнее звено на IAT g_comctl32New; для «текстовых» push-кнопок (без BS_ICON/BS_BITMAP)
// SRCCOPY обрабатывается как ListView: GdiAlphaBlend / black-key. BS_ICON и BS_BITMAP из перехвата исключены (см. комментарии у хелпера —
// гипотеза не покрыта полным тестом, возможны регрессии на стекле).
//

#include "CorePch.h"
#include <cstdio>

#include <Hooks/Button.h>
#include <Hooks/forms/ButtonFormsNiDtbAccess.h>
#include <Hooks/forms/FormsHooks.h>
#include <Hooks/ListBox.h>
#include <Hooks/Static.h>
#include <Subsystems/RenderText.h>
#include <Helpers/ThemeHelper.h>
#include <Subsystems/ThemePaintRegistry.h>
#include <Core/Intercept/HookDispatcher.h>
#include <Module/ModuleHandler.h>
#include <Hooks/ScrollBarTheme.h>
#include <Subsystems/PaintDispatcher.h>
#include <Subsystems/GdiHookUtils.h>
#include <Subsystems/Settings.h>
#include <GlassControlKind.h>
#include <wil.h>
#include <uxtheme.h>

#include <commctrl.h>
#include <algorithm>
#include <optional>
#include <string>
#include <vector>
#include <WinApiUtils.h>
#include <BitmapUtils.h>
#include <DebugLog.h>

#pragma comment(lib, "msimg32.lib")

#include <detours.h>

using namespace GlassCore::Hooks;
using namespace std::literals;

namespace GlassCore::Button
{
    // =========================================================================
    //  Сессия paint — mod + kind + HWND (для фильтра BS_* в DrawThemeBackground).
    //  Набор хуков полностью задаётся в EnableButtonHooksForModule(...).
    // =========================================================================

    struct ButtonPaintSession
    {
        HMODULE                       mod{};
        GlassShared::GlassControlKind kind{};
        HWND                          hWnd{};
    };

    // Части темы «Button» (vssym32 BUTTONPARTS): без чекбокса/радио/группы.
    [[nodiscard]] static bool IsButtonThemePartSemiGlassEligible(int iPartId) noexcept
    {
        switch (iPartId)
        {
        case BP_GROUPBOX:
        case BP_CHECKBOX:
        case BP_RADIOBUTTON:
            return false;
        default:
            return true;
        }
    }

    // Нативная кнопка: push/defpush/ownerdraw и т.д., не группа и не чекбокс/радио.
    [[nodiscard]] static bool IsNativePushLikeButtonStyle(HWND hWnd) noexcept
    {
        if (!hWnd || !IsWindow(hWnd))
            return false;
        const LONG style = GetWindowLongW(hWnd, GWL_STYLE);
        const UINT t = static_cast<UINT>(style) & BS_TYPEMASK;
        switch (t)
        {
        case BS_GROUPBOX:
        case BS_CHECKBOX:
        case BS_AUTOCHECKBOX:
        case BS_RADIOBUTTON:
        case BS_3STATE:
        case BS_AUTO3STATE:
        case BS_AUTORADIOBUTTON:
            return false;
        default:
            return true;
        }
    }

    // -------------------------------------------------------------------------
    //  BS_ICON / BS_BITMAP — исключение из BitBlt-хука (IAT g_comctl32New)
    //
    //  Гипотеза: для BS_ICON comctl32 чаще выводит HICON путём, уже учитывающим прозрачность
    //  (маска / 32bpp), а наш GdiAlphaBlend + black-key может дублировать обработку или портить
    //  картинку. Для BS_BITMAP предполагается, что источник может уже нести альфу или тема рисует
    //  иначе; однако 24bpp без альфы при исключении снова уходит в цепочку с обычным BitBlt —
    //  на DWM-стекле возможен просвет (как у «голого» BitBlt).
    //
    //  ВАЖНО: эта ветка логики НЕ прогнана по полному набору сценариев (разные темы, Win10/11,
    //  WinForms, разрядность битмапа, вложенные диалоги, PerMonitor DPI). Считать поведение
    //  предварительным: при артефактах на кнопках с картинкой нужна дополнительная отладка
    //  (логирование стиля HWND, формата битмапа, стека вызовов BitBlt) и возможна смена
    //  критерия (например, исключать только BS_ICON или только при наличии альфы в образе).
    // -------------------------------------------------------------------------
    [[nodiscard]] static bool IsButtonIconOrBitmapStyle(HWND hWnd) noexcept
    {
        if (!hWnd || !IsWindow(hWnd))
            return false;
        const LONG style = GetWindowLongW(hWnd, GWL_STYLE);
        return (style & (BS_ICON | BS_BITMAP)) != 0;
    }

    // BS_SPLITBUTTON (0x0C) / BS_DEFSPLITBUTTON (0x0D): comctl32 дважды вызывает DrawThemeBackground
    // на тот же клиентский rect. Первый проход — DrawThemeBackgroundWithConstantAlpha; второй идентичный
    // вызов нельзя снова блендить (двойная альфа) и нельзя вести в orig — непрозрачный DTB перекроет стекло.
    // SplitDtbDedupKey: второй совпадающий вызов — no-op (S_OK).
    [[nodiscard]] static bool IsSplitButtonStyle(HWND hWnd) noexcept
    {
        if (!hWnd || !IsWindow(hWnd))
            return false;
        const UINT t = static_cast<UINT>(GetWindowLongW(hWnd, GWL_STYLE)) & BS_TYPEMASK;
        return t == BS_SPLITBUTTON || t == BS_DEFSPLITBUTTON;
    }

    // Marlett «6» — глиф стрелки дропдауна у сплит-кнопки (comctl DrawTextW).
    [[nodiscard]] static bool IsMarlettSplitArrowGlyph(LPCWSTR text, int cch) noexcept
    {
        return cch == 1 && text && text[0] == L'6';
    }

    // Comctl32 для split (и др.) может дважды вызвать DTB с одним theme/part/state/rect.
    // Клип не входит в ключ — второй вызов часто с другим pClipRect. HWND обязателен при вложенном BeginPaint.
    struct SplitDtbDedupKey
    {
        HWND   hWnd{};
        HTHEME hTheme{};
        int    partId{};
        int    stateId{};
        RECT   rect{};

        [[nodiscard]] bool operator==(const SplitDtbDedupKey& o) const noexcept
        {
            return hWnd == o.hWnd && hTheme == o.hTheme && partId == o.partId && stateId == o.stateId
                && EqualRect(&rect, &o.rect);
        }
    };

    thread_local std::optional<SplitDtbDedupKey> g_splitButtonDtbDedupKey;

    thread_local std::vector<ButtonPaintSession> g_buttonSessionStack;

    // Удаление фона иконок кнопок: Root\Control\Button (presence color = chroma; иначе black-key).
    // Читаем один раз на внешнюю сессию (стек пуст), как ListViewHooks.
    thread_local WndCompat::Menu::IconBackgroundColorRemoval g_buttonIconRemoval{};

    static std::atomic<bool> g_shutdown{ false };

    // =========================================================================
    //  Helpers
    // =========================================================================

    static bool StrEq(LPCWSTR a, LPCWSTR b)
    {
        return _wcsicmp(a, b) == 0;
    }

    static bool BaseNameEq(const std::wstring& base, LPCWSTR expect)
    {
        return _wcsicmp(base.c_str(), expect) == 0;
    }

    [[nodiscard]] static void* ResolveGdiModuleForExtTextOut() noexcept
    {
        if (void* p = GetModuleHandleW(L"gdi32full.dll")) return p;
        return GetModuleHandleW(L"gdi32.dll");
    }

    // =========================================================================
    //  OVERALL DISPATCHER MAP
    // =========================================================================

    int      WINAPI Overall_MyDrawThemeBg(HTHEME, HDC, int, int, LPCRECT, LPCRECT);
    HRESULT  WINAPI Overall_MyDrawThemeText(HTHEME, HDC, int, int, LPCWSTR, int, DWORD, DWORD, LPCRECT);
    HRESULT  WINAPI Overall_MyDrawThemeTextEx(HTHEME, HDC, int, int, LPCWSTR, int, DWORD, LPRECT, const DTTOPTS*);
    int      WINAPI Overall_MyDrawTextW(HDC, LPCWSTR, int, LPRECT, UINT);
    int      WINAPI Overall_MyDrawTextExW(HDC, LPWSTR, int, LPRECT, UINT, LPDRAWTEXTPARAMS);
    BOOL     WINAPI Overall_MyExtTextOutW(HDC, int, int, UINT, const RECT*, LPCWSTR, UINT, const INT*);
    BOOL     WINAPI Overall_MyFillRect(HDC, const RECT*, HBRUSH);
    int      WINAPI Overall_MyGetSysColor(int);
    HBRUSH   WINAPI Overall_MyGetSysColorBrush(int);
    HBRUSH   WINAPI Overall_MyCreateSolidBrush(COLORREF);
    BOOL     WINAPI Overall_MyBitBlt(HDC, int, int, int, int, HDC, int, int, DWORD);
    BOOL     WINAPI Overall_MyDrawEdge(HDC, LPRECT, UINT, UINT);

    enum OverallHookIndex : size_t
    {
        OHK_DRAWTHEMEBG = 0,
        OHK_DRAWTHEMETEXT,
        OHK_DRAWTHEMETEXTEX,
        OHK_DRAWTEXT,
        OHK_DRAWTEXTEX,
        OHK_EXTTEXTOUT,
        OHK_FILLRECT,
        OHK_GETSYSCOLOR,
        OHK_GETSYSCOLORBR,
        OHK_CREATESOLIDBRUSH,
        OHK_BITBLT,
        OHK_DRAWEDGE,
        OHK_COUNT
    };

    inline const HookDispatcherDependency<OHK_COUNT, 3> g_overallHookDep
    { {
            // OHK_DRAWTHEMEBG
            std::tuple{ std::array{ "uxtheme.dll"sv, "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv, ""sv },
                "DrawThemeBackground", reinterpret_cast<PVOID>(Overall_MyDrawThemeBg) },
                // OHK_DRAWTHEMETEXT
                std::tuple{ std::array{ "uxtheme.dll"sv, "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv, ""sv },
                    "DrawThemeText", reinterpret_cast<PVOID>(Overall_MyDrawThemeText) },
                    // OHK_DRAWTHEMETEXTEX
                    std::tuple{ std::array{ "uxtheme.dll"sv, "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv, ""sv },
                        "DrawThemeTextEx", reinterpret_cast<PVOID>(Overall_MyDrawThemeTextEx) },
                        // OHK_DRAWTEXT
                        std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
                            "DrawTextW", reinterpret_cast<PVOID>(Overall_MyDrawTextW) },
                            // OHK_DRAWTEXTEX
                            std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
                                "DrawTextExW", reinterpret_cast<PVOID>(Overall_MyDrawTextExW) },
                                // OHK_EXTTEXTOUT (gdi32full — импорт comctl32 в части процессов)
                                std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv },
                                    "ExtTextOutW", reinterpret_cast<PVOID>(Overall_MyExtTextOutW) },
                                    // OHK_FILLRECT
                                    std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
                                        "FillRect", reinterpret_cast<PVOID>(Overall_MyFillRect) },
                                        // OHK_GETSYSCOLOR
                                        std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
                                            "GetSysColor", reinterpret_cast<PVOID>(Overall_MyGetSysColor) },
                                            // OHK_GETSYSCOLORBR
                                            std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
                                                "GetSysColorBrush", reinterpret_cast<PVOID>(Overall_MyGetSysColorBrush) },
                                                // OHK_CREATESOLIDBRUSH (inetcpl / sysmon)
                                                std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv },
                                                    "CreateSolidBrush", reinterpret_cast<PVOID>(Overall_MyCreateSolidBrush) },
                                                    // OHK_BITBLT (см. исключение BS_ICON/BS_BITMAP — комментарий у IsButtonIconOrBitmapStyle)
                                                    std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv },
                                                        "BitBlt", reinterpret_cast<PVOID>(Overall_MyBitBlt) },
                                                        // OHK_DRAWEDGE (COMCTL32 → split-кнопка, см. EnableButtonHooksForModule)
                                                        std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
                                                            "DrawEdge", reinterpret_cast<PVOID>(Overall_MyDrawEdge) },
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

    [[nodiscard]] static bool IsSystemWindowsFormsNiModule(HMODULE mod) noexcept
    {
        if (!mod) return false;
        wchar_t path[MAX_PATH]{};
        if (!GetModuleFileNameW(mod, path, MAX_PATH)) return false;
        const wchar_t* base = wcsrchr(path, L'\\');
        base = base ? base + 1 : path;
        return _wcsicmp(base, L"System.Windows.Forms.ni.dll") == 0;
    }

    // WinForms .BUTTON: DTB/DrawText рисует comctl32, не caller BeginPaint.
    [[nodiscard]] static HMODULE ResolveButtonComctlHookModule(HMODULE callerMod) noexcept
    {
        auto* g_new = GlassCore::ModuleHandler::g_comctl32New;
        auto* g_legacy = GlassCore::ModuleHandler::g_comctl32Legacy;
        if (callerMod == g_new || callerMod == g_legacy)
            return callerMod;
        return g_new ? g_new : g_legacy;
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

    [[nodiscard]] static bool TryGetButtonThemeDrawRect(
        LPCRECT pRect, LPCRECT pClipRect, RECT& out) noexcept
    {
        if (pRect && !IsRectEmpty(pRect))
        {
            out = *pRect;
            if (pClipRect)
            {
                RECT inter{};
                if (!IntersectRect(&inter, &out, pClipRect))
                    return false;
                out = inter;
            }
            return !IsRectEmpty(&out);
        }
        if (pClipRect && !IsRectEmpty(pClipRect))
        {
            out = *pClipRect;
            return true;
        }
        return false;
    }

    // Сессия для InvokeButtonDrawThemeBackground: WinForms .LISTBOX (PaintDispatcher) или стек .BUTTON.
    [[nodiscard]] static bool TryResolveWinFormsNiButtonThemeSession(
        HDC hdc, ButtonPaintSession& out) noexcept
    {
        if (GlassCore::ListBoxControl::IsInsideWinFormsListBoxPaint())
        {
            HWND hWnd = GlassCore::PaintDispatcher::GetPaintSessionHostHwnd();
            if (!hWnd || !IsWindow(hWnd))
                hWnd = WindowFromDC(hdc);
            if (GlassShared::GlassGetWindowKind(hWnd) != GlassShared::GlassControlKind::WinForms_ListBox)
            {
                if (!g_buttonSessionStack.empty())
                {
                    out = g_buttonSessionStack.back();
                    return true;
                }
                return false;
            }
            out = { nullptr, GlassShared::GlassControlKind::WinForms_ListBox, hWnd };
            return true;
        }
        if (!g_buttonSessionStack.empty())
        {
            out = g_buttonSessionStack.back();
            return true;
        }
        return false;
    }

    [[nodiscard]] static HRESULT InvokeButtonDrawThemeBackgroundImpl(
        Shared::WinApiUtils::DrawThemeBackground_pfn orig,
        HMODULE callerMod,
        HTHEME hTheme,
        HDC hdc,
        int iPartId,
        int iStateId,
        LPCRECT pRect,
        LPCRECT pClipRect,
        uint32_t seq) noexcept
    {
        if (!orig)
            return DrawThemeBackground(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

        ButtonPaintSession sess{};
        if (!TryResolveWinFormsNiButtonThemeSession(hdc, sess))
        {
            DebugLog(DBG_NORMAL,
                L"[GlassCore][Button] InvokeButtonDrawThemeBg: seq=%u branch=Skip reason=noSession caller=%p part=%d state=%d",
                seq, callerMod, iPartId, iStateId);
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
        }

        if (!hTheme)
        {
            DebugLog(DBG_NORMAL,
                L"[GlassCore][Button] InvokeButtonDrawThemeBg: seq=%u branch=Skip reason=nullTheme caller=%p part=%d state=%d",
                seq, callerMod, iPartId, iStateId);
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
        }

        WCHAR themeClass[MAX_PATH + 1]{};
        const HRESULT hrThemeClass =
            Shared::ThemeHelper::GetThemeClass(hTheme, themeClass, MAX_PATH);

        using GK = GlassShared::GlassControlKind;

        if (sess.kind == GK::WinForms_ListBox)
        {
            if (FAILED(hrThemeClass))
            {
                DebugLog(DBG_WARNING,
                    L"[GlassCore][Button] InvokeButtonDrawThemeBg: seq=%u branch=Skip reason=GetThemeClassFailed hr=0x%08X caller=%p part=%d state=%d",
                    seq, static_cast<unsigned>(hrThemeClass), callerMod, iPartId, iStateId);
                return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
            }

            if (StrEq(themeClass, L"ListBox") && iPartId == 0 && iStateId == 0)
            {
                DebugLog(DBG_NORMAL,
                    L"[GlassCore][Button] InvokeButtonDrawThemeBg: seq=%u branch=Skip reason=ListBoxBkg caller=%p",
                    seq, callerMod);
                return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
            }

            RECT drawRect{};
            if (!TryGetButtonThemeDrawRect(pRect, pClipRect, drawRect))
            {
                DebugLog(DBG_NORMAL,
                    L"[GlassCore][Button] InvokeButtonDrawThemeBg: seq=%u branch=Skip reason=emptyRect theme=%s caller=%p part=%d state=%d",
                    seq, themeClass, callerMod, iPartId, iStateId);
                return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
            }

            DebugLog(DBG_NORMAL,
                L"[GlassCore][Button] InvokeButtonDrawThemeBg: seq=%u branch=PatBltOrig sessKind=WinForms_ListBox theme=%s hostHwnd=%p rect=[%d,%d,%d,%d] caller=%p part=%d state=%d",
                seq, themeClass, sess.hWnd,
                drawRect.left, drawRect.top, drawRect.right, drawRect.bottom,
                callerMod, iPartId, iStateId);

            PatBlt(hdc,
                drawRect.left, drawRect.top,
                drawRect.right - drawRect.left,
                drawRect.bottom - drawRect.top,
                BLACKNESS);
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
        }

        if (!SUCCEEDED(hrThemeClass) || !StrEq(themeClass, L"Button"))
        {
            DebugLog(DBG_NORMAL,
                L"[GlassCore][Button] InvokeButtonDrawThemeBg: seq=%u branch=Skip reason=notButtonTheme theme=%s caller=%p part=%d state=%d",
                seq, SUCCEEDED(hrThemeClass) ? themeClass : L"?", callerMod, iPartId, iStateId);
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
        }

        RECT drawRect{};
        if (!TryGetButtonThemeDrawRect(pRect, pClipRect, drawRect))
        {
            DebugLog(DBG_NORMAL,
                L"[GlassCore][Button] InvokeButtonDrawThemeBg: seq=%u branch=Skip reason=emptyRect caller=%p part=%d state=%d",
                seq, callerMod, iPartId, iStateId);
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
        }

        bool useAlphaPath = false;
        if (sess.kind == GK::WinForms_Button)
            useAlphaPath = IsNativePushLikeButtonStyle(sess.hWnd) && IsButtonThemePartSemiGlassEligible(iPartId);
        else if (sess.kind == GK::Button)
            useAlphaPath = IsNativePushLikeButtonStyle(sess.hWnd) && IsButtonThemePartSemiGlassEligible(iPartId);

        if (!useAlphaPath)
        {
            if (sess.kind == GK::WinForms_Button || sess.kind == GK::WinForms_ListBox)
            {
                DebugLog(DBG_NORMAL,
                    L"[GlassCore][Button] InvokeButtonDrawThemeBg: seq=%u branch=PatBltOrig sessKind=%d rect=[%d,%d,%d,%d] caller=%p part=%d state=%d",
                    seq, static_cast<int>(sess.kind),
                    drawRect.left, drawRect.top, drawRect.right, drawRect.bottom,
                    callerMod, iPartId, iStateId);
                PatBlt(hdc,
                    drawRect.left, drawRect.top,
                    drawRect.right - drawRect.left,
                    drawRect.bottom - drawRect.top,
                    BLACKNESS);
            }
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
        }

        const bool dedupNativePush = (sess.kind == GK::Button);

        SplitDtbDedupKey dedupKey{};
        if (dedupNativePush)
        {
            dedupKey.hWnd    = sess.hWnd;
            dedupKey.hTheme  = hTheme;
            dedupKey.partId  = iPartId;
            dedupKey.stateId = iStateId;
            dedupKey.rect    = drawRect;
            if (g_splitButtonDtbDedupKey && *g_splitButtonDtbDedupKey == dedupKey)
            {
                DebugLog(DBG_NORMAL,
                    L"[GlassCore][Button] InvokeButtonDrawThemeBg: seq=%u branch=Skip reason=doubleDtbDedup caller=%p part=%d state=%d",
                    seq, callerMod, iPartId, iStateId);
                return S_OK;
            }
        }

        const BYTE effAlpha =
            GlassCore::ThemePaint::GetEffectiveDrawThemeConstantAlphaForSession(sess.kind, sess.hWnd);
        DebugLog(DBG_NORMAL,
            L"[GlassCore][Button] InvokeButtonDrawThemeBg: seq=%u branch=Alpha caller=%p part=%d state=%d rect=[%d,%d,%d,%d] effAlpha=%d",
            seq, callerMod, iPartId, iStateId,
            drawRect.left, drawRect.top, drawRect.right, drawRect.bottom, effAlpha);
        const bool alphaOk = Shared::WinApiUtils::DrawThemeBackgroundWithConstantAlpha(
            orig, hTheme, hdc, iPartId, iStateId, pRect, pClipRect, effAlpha);

        if (dedupNativePush)
            g_splitButtonDtbDedupKey = dedupKey;

        if (alphaOk)
            return S_OK;

        return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
    }

    // =========================================================================
    //  EnableButtonHooksForModule — включить/выключить хуки по типу контрола
    //  и вызывающему модулю (по паттерну EnableListViewHooksForModule)
    // =========================================================================
    //
    //  Взаимоисключение Glow vs DTTEx / DrawTextEx — те же условия при enable=true и false
    //  (счётчики HookDispatcher), как в EnableTreeViewHooksForModule.
    //
    //  GlassControlKind::WinForms_Button:
    //    callerMod: DrawTextW, DrawThemeBackground (IAT); без glow — DrawThemeTextEx (caller).
    //    При glow — DrawTextExW: uxtheme только для чекбокса/группы/радио (!IsNativePushLikeButtonStyle) и если есть uxtheme;
    //    иначе при glow — DrawTextExW на caller.
    //    System.Windows.Forms.ni.dll: DrawTextExW (глобальный Detours-хук)
    //
    //  GlassControlKind::Button (USER32, без темы окна):
    //    gdi32full/gdi32: ExtTextOutW (IAT)
    //
    //  GlassControlKind::Button (themed, comctl32):
    //    callerMod: DrawThemeBackground (IAT); текст themed:
    //      push-like → всегда DrawThemeTextEx (caller); glow для push не вешает DrawTextExW на uxtheme.
    //      чекбокс/группа/радио + glow → DrawTextExW на uxtheme.dll (если есть), иначе DrawTextExW на caller.
    //      чекбокс/группа/радио без glow → DrawThemeTextEx (caller).
    //    DrawTextW / DrawEdge — только BS_SPLITBUTTON
    //    inetcpl.cpl: DrawTextW (только split), GetSysColorBrush, CreateSolidBrush (IAT)
    //    sysmon.ocx: CreateSolidBrush (IAT)
    //    g_entryPointModule: DrawTextW (IAT)
    //
    //  GlassControlKind::Button (нетематическая, не USER32):
    //    gdi32full/gdi32: ExtTextOutW (IAT)

    static void EnableButtonHooksForModule(
        HWND hWnd,
        HMODULE callerMod,
        GlassShared::GlassControlKind kind,
        bool enable)
    {
        using namespace GlassShared;
        void* const caller = static_cast<void*>(callerMod);

        // ── Нативный Button ─
        const std::wstring base = Shared::WinApiUtils::GetModuleFileBaseName(callerMod);
        const bool winHasTheme = (GetWindowTheme(hWnd) != nullptr);
        const bool buttonThemed = Shared::WinApiUtils::IsThemed(hWnd, L"Button")
            || (BaseNameEq(base, L"COMCTL32.dll") && winHasTheme);

        if (kind == GlassControlKind::WinForms_Button)
        {
            // ── WinForms .BUTTON ──────────────────────────────────────────
            HMODULE const comctlMod = ResolveButtonComctlHookModule(callerMod);
            void* const comctl = static_cast<void*>(comctlMod);
            if (!comctl)
                return;

            EnableHooksOnModule(comctl, OHK_DRAWTEXT, enable);
            if (buttonThemed)
            {
                EnableHooksOnModule(comctl, OHK_DRAWTHEMEBG, enable);
                const bool pushLike = hWnd && IsNativePushLikeButtonStyle(hWnd);
                const bool glow = GlassCore::Text::IsInkGlowEffectActiveForPaintSession(hWnd);
                HMODULE hUx = GetModuleHandleW(L"uxtheme.dll");

                if (pushLike || !glow || (!hUx && glow))
                    EnableHooksOnModule(comctl, OHK_DRAWTHEMETEXT, enable);
                else
                    EnableHooksOnModule(hUx, OHK_DRAWTEXTEX, enable);
            }

            if (GetModuleHandleW(L"System.Windows.Forms.ni.dll"))
            {
                GlassCore::FormsHooks::DrawTextExAttach(enable);
                GlassCore::FormsHooks::DrawThemeBackgroundAttach(enable);
            }

            return;
        }

        if (kind != GlassControlKind::Button)
            return;


        // USER32 + нет визуальной темы окна → текст только через gdi32full ExtTextOut
        if (BaseNameEq(base, L"USER32.dll") && !winHasTheme)
        {
            if (void* gdi = ResolveGdiModuleForExtTextOut())
                EnableHooksOnModule(gdi, OHK_EXTTEXTOUT, enable);
            return;
        }

        const bool split = hWnd && IsSplitButtonStyle(hWnd);

        // Themed comctl32 button: основной набор на callerMod
        if (caller)
        {
            if (split)
            {
                EnableHooksOnModule(caller, OHK_DRAWTEXT, enable);
                if (BaseNameEq(base, L"COMCTL32.dll"))
                    EnableHooksOnModule(caller, OHK_DRAWEDGE, enable);
            }
            // Themed text: push-like — только DTTEx; DrawTextExW на uxtheme — только чекбокс/группа/радио + glow.
            if (buttonThemed)
            {
                const bool pushLike = hWnd && IsNativePushLikeButtonStyle(hWnd);
                const bool glow = GlassCore::Text::IsInkGlowEffectActiveForPaintSession(hWnd);
                HMODULE hUx = GetModuleHandleW(L"uxtheme.dll");

                EnableHooksOnModule(caller, OHK_DRAWTHEMEBG, enable);

                if (pushLike || !glow || (!hUx && glow))
                {
                    EnableHooksOnModule(caller, OHK_DRAWTHEMETEXT, enable);
                }
                else
                    EnableHooksOnModule(hUx, OHK_DRAWTEXTEX, enable);
            }
            
            if (hWnd && !IsButtonIconOrBitmapStyle(hWnd))
                EnableHooksOnModule(caller, OHK_BITBLT, enable);
        }

        // Дополнительные модули, встречающиеся в отдельных процессах
        if (void* inetcpl = GetModuleHandleW(L"inetcpl.cpl"))
        {
            if (split)
                EnableHooksOnModule(inetcpl, OHK_DRAWTEXT, enable);
            EnableHooksOnModule(inetcpl, OHK_GETSYSCOLORBR, enable);
            EnableHooksOnModule(inetcpl, OHK_CREATESOLIDBRUSH, enable);
            EnableHooksOnModule(inetcpl, OHK_DRAWTEXTEX, enable && GlassCore::Text::IsInkGlowEffectActive());
        }
        // System.Windows.Forms.ni.dll вызывает DrawTextExW напрямую из своего IAT —
        // IAT callerMod его не ловит. Глобальный Detours-хук покрывает этот случай.
        if (GetModuleHandleW(L"System.Windows.Forms.ni.dll"))
            GlassCore::FormsHooks::DrawTextExAttach(enable);
        if (void* sysmon = GetModuleHandleW(L"sysmon.ocx"))
            EnableHooksOnModule(sysmon, OHK_CREATESOLIDBRUSH, enable);
        if (split && GlassCore::ModuleHandler::g_entryPointModule)
            EnableHooksOnModule(GlassCore::ModuleHandler::g_entryPointModule, OHK_DRAWTEXT, enable);

        return;

        // Нетематическая кнопка (не USER32) → только ExtTextOut
        if (void* gdi = ResolveGdiModuleForExtTextOut())
            EnableHooksOnModule(gdi, OHK_EXTTEXTOUT, enable);
    }

    // =========================================================================
    //  Hook implementations
    // =========================================================================

    // ── DrawThemeBackground ──────────────────────────────────────────────────
    // Тема «Button»: для push-кнопок (не группа, не чекбокс/радио) — как Combo/Tab:
    // offscreen orig + GdiAlphaBlend с постоянной альфой; иначе только orig.
    int WINAPI Overall_MyDrawThemeBg(
        HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
        LPCRECT pRect, LPCRECT pClipRect)
    {
        if (g_shutdown.load(std::memory_order_acquire))
            return DrawThemeBackground(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_DRAWTHEMEBG, decltype(&DrawThemeBackground)>(callerMod);

        if (!orig)
            return DrawThemeBackground(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

        return static_cast<int>(InvokeButtonDrawThemeBackgroundImpl(
            reinterpret_cast<Shared::WinApiUtils::DrawThemeBackground_pfn>(orig),
            callerMod,
            hTheme,
            hdc,
            iPartId,
            iStateId,
            pRect,
            pClipRect,
            0));
    }

    // ── DrawThemeText ────────────────────────────────────────────────────────
    HRESULT WINAPI Overall_MyDrawThemeText(
        HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
        LPCWSTR pszText, int cchText, DWORD dwTextFlags, DWORD /*dwTextFlags2*/,
        LPCRECT pRect)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_DRAWTHEMETEXT, decltype(&DrawThemeText)>(callerMod);

        if (!orig)
            return DrawThemeText(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, 0, pRect);

        if (g_buttonSessionStack.empty())
            return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, 0, pRect);

        if (!pRect || IsRectEmpty(pRect) || !pszText || cchText == 0)
            return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, 0, pRect);


        DebugLog(DBG_NORMAL,
            L"[GlassCore][Button] Overall_MyDrawThemeText: caller=%p part=%d state=%d textLen=%d rect=[%d,%d,%d,%d] flags=0x%X",
            callerMod, iPartId, iStateId, cchText, pRect->left, pRect->top, pRect->right, pRect->bottom, dwTextFlags);

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

    // ── DrawThemeTextEx ──────────────────────────────────────────────────────
    HRESULT WINAPI Overall_MyDrawThemeTextEx(
        HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
        LPCWSTR pszText, int cchText, DWORD dwTextFlags,
        LPRECT pRect, const DTTOPTS* pOptions)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_DRAWTHEMETEXTEX, decltype(&DrawThemeTextEx)>(callerMod);

        if (!orig)
            return DrawThemeTextEx(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions);

        if (g_buttonSessionStack.empty())
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

        if (iPartId == BP_CHECKBOX || iPartId == BP_RADIOBUTTON || iPartId == BP_GROUPBOX)
            return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions);

        DebugLog(DBG_NORMAL,
            L"[GlassCore][Button] Overall_MyDrawThemeTextEx: caller=%p part=%d state=%d textLen=%d rect=[%d,%d,%d,%d] flags=0x%X",
            callerMod, iPartId, iStateId, cchText, pRect->left, pRect->top, pRect->right, pRect->bottom, dwTextFlags);

        // Принудительно включаем DTT_COMPOSITED для правильного альфа-рендеринга
        DTTOPTS opts = *effectiveOpts;
        if (opts.dwSize < sizeof(DTTOPTS))
            opts.dwSize = sizeof(DTTOPTS);
        opts.dwFlags |= DTT_COMPOSITED;

        return GlassCore::Text::DrawThemeContent(
            hdc, *pRect, nullptr, nullptr, 0,
            [&](HDC memDC, HPAINTBUFFER, RGBQUAD*, int)
            {
                DrawThemeTextEx(hTheme, memDC, iPartId, iStateId,
                    pszText, cchText, dwTextFlags, pRect, &opts);
            });
    }

    // ── DrawTextW отрисовка стрелки в сплит-кнопке глиф с индексом 6 ────────────────────────────────────────────────────────────
    int WINAPI Overall_MyDrawTextW(
        HDC hdc, LPCWSTR lpchText, int cchText, LPRECT lprc, UINT format)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_DRAWTEXT, decltype(&DrawTextW)>(callerMod);

        if (!orig)
            return DrawTextW(hdc, lpchText, cchText, lprc, format);

        if (g_buttonSessionStack.empty())
            return orig(hdc, lpchText, cchText, lprc, format);

        if (format & DT_CALCRECT)
            return orig(hdc, lpchText, cchText, lprc, format);

        if (!lprc || IsRectEmpty(lprc) || !lpchText || cchText == 0)
            return orig(hdc, lpchText, cchText, lprc, format);

        const ButtonPaintSession& sess = g_buttonSessionStack.back();
        const bool splitArrowGlyph =
            sess.hWnd && IsSplitButtonStyle(sess.hWnd) && IsMarlettSplitArrowGlyph(lpchText, cchText);

        DebugLog(DBG_NORMAL,
            L"[GlassCore][Button] Overall_MyDrawTextW: caller=%p text=\"%.*s\" rect=[%d,%d,%d,%d] format=0x%X",
            callerMod, cchText, lpchText, lprc->left, lprc->top, lprc->right, lprc->bottom, format);

        // Сплит-стрелка Marlett: полный lprc (DT_VCENTER). Buffered paint с BPPF_ERASE заливает весь rect
        // непрозрачным фоном окна — transparentBufferBackground убирает erase и обнуляет DIB.
        if (splitArrowGlyph)
        {
            int splitResult = 0;
            if (!FAILED(GlassCore::Text::DrawTextWithAlpha(
                    hdc, lpchText, cchText, lprc, format, splitResult, true)))
                return splitResult;
        }

        int result = 0;
        if (!FAILED(GlassCore::Text::DrawTextWithAlpha(hdc, lpchText, cchText, lprc, format, result)))
            return result;

        return orig(hdc, lpchText, cchText, lprc, format);
    }

    // ── DrawTextExW (IAT-хук) ────────────────────────────────────────────────
    // Перехватывает DrawTextExW через IAT вызывающего модуля (comctl32 и др.).
    // Вызовы из System.Windows.Forms.ni.dll покрыты глобальным Detours-хуком
    // (WinFormsDetour_MyDrawTextExW) и сюда не попадают.
    int WINAPI Overall_MyDrawTextExW(
        HDC hdc, LPWSTR lpchText, int cchText,
        LPRECT lprc, UINT format, LPDRAWTEXTPARAMS lpdtp)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_DRAWTEXTEX, decltype(&DrawTextExW)>(callerMod);

        if (!orig)
            return DrawTextExW(hdc, lpchText, cchText, lprc, format, lpdtp);

        if (g_buttonSessionStack.empty())
            return orig(hdc, lpchText, cchText, lprc, format, lpdtp);

        if (format & DT_CALCRECT)
            return orig(hdc, lpchText, cchText, lprc, format, lpdtp);

        if (!lprc || IsRectEmpty(lprc) || !lpchText || cchText == 0)
            return orig(hdc, lpchText, cchText, lprc, format, lpdtp);

        DebugLog(DBG_NORMAL,
            L"[GlassCore][Button] Overall_MyDrawTextExW: caller=%p textLen=%d rect=[%d,%d,%d,%d] format=0x%X",
            callerMod, cchText, lprc->left, lprc->top, lprc->right, lprc->bottom, format);

        if (lpdtp)
            return orig(hdc, lpchText, cchText, lprc, format, lpdtp);

        PatBlt(hdc, lprc->left, lprc->top,
            lprc->right - lprc->left, lprc->bottom - lprc->top, BLACKNESS);

        int result = 0;
        if (!FAILED(GlassCore::Text::DrawTextWithAlpha(hdc, lpchText, cchText, lprc, format, result)))
            return result;

        return orig(hdc, lpchText, cchText, lprc, format, lpdtp);
    }

    // ── ExtTextOutW ──────────────────────────────────────────────────────────
    BOOL WINAPI Overall_MyExtTextOutW(
        HDC hdc, int x, int y, UINT options, const RECT* lprect,
        LPCWSTR lpString, UINT c, const INT* lpDx)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_EXTTEXTOUT, decltype(&ExtTextOutW)>(callerMod);

        if (!orig)
            return ExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);

        if (g_buttonSessionStack.empty())
            return orig(hdc, x, y, options, lprect, lpString, c, lpDx);

        DebugLog(DBG_NORMAL,
            L"[GlassCore][Button] Overall_MyExtTextOutW: caller=%p options=0x%X c=%u rect=%p",
            callerMod, options, c, lprect);

        if ((options & ETO_OPAQUE) && lprect)
        {
            PatBlt(hdc, lprect->left, lprect->top,
                lprect->right - lprect->left, lprect->bottom - lprect->top,
                BLACKNESS);

            if (!c) return TRUE;

            RECT rc{ x, y, lprect->right, lprect->bottom };
            DWORD flags = DT_EXPANDTABS | DT_NOPREFIX;
            if (options & ETO_RTLREADING) flags |= DT_RTLREADING;

            DebugLog(DBG_NORMAL,
                L"[GlassCore][Button] Overall_MyExtTextOutW: drawing alpha text caller=%p c=%u",
                callerMod, c);

            int result = 0;
            if (!FAILED(GlassCore::Text::DrawTextWithAlpha(hdc, lpString, c, &rc, flags, result)))
                return result;
        }

        return orig(hdc, x, y, options, lprect, lpString, c, lpDx);
    }

    // ── FillRect ─────────────────────────────────────────────────────────────
    // Системные фоновые кисти (BTNFACE, WINDOW) → PatBlt(BLACKNESS) → прозрачно.
    BOOL WINAPI Overall_MyFillRect(HDC hdc, const RECT* lprc, HBRUSH hbr)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_FILLRECT, decltype(&FillRect)>(callerMod);

        if (!orig)
            return FillRect(hdc, lprc, hbr);

        if (g_buttonSessionStack.empty())
            return orig(hdc, lprc, hbr);

        if (lprc && hbr)
        {
            // Сравнение по RGB (LOGBRUSH), а не по HBRUSH: у системных кистей дескрипторы нестабильны.
            const COLORREF sysBtn = ::GetSysColor(COLOR_BTNFACE);
            const COLORREF sysWin = ::GetSysColor(COLOR_WINDOW);
            bool isSysBg = false;
            if (hbr == static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)))
                isSysBg = true;
            else
            {
                LOGBRUSH lb{};
                if (GetObjectW(hbr, sizeof(lb), &lb) == sizeof(lb) && lb.lbStyle == BS_SOLID)
                    isSysBg = (lb.lbColor == sysBtn || lb.lbColor == sysWin);
            }

            if (isSysBg)
            {
                PatBlt(hdc,
                    lprc->left, lprc->top,
                    lprc->right - lprc->left, lprc->bottom - lprc->top,
                    BLACKNESS);
                return TRUE;
            }

            // Прочие кисти — рисуем через alpha-совместимую кисть
            LOGBRUSH lb{};
            if (GetObject(hbr, sizeof(lb), &lb) && lb.lbStyle == BS_SOLID && lb.lbColor != 0)
            {
                HBRUSH alphaBrush = Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(lb.lbColor, 255);
                if (alphaBrush)
                {
                    BOOL res = orig(hdc, lprc, alphaBrush);
                    DeleteObject(alphaBrush);
                    return res;
                }
            }
        }

        return orig(hdc, lprc, hbr);
    }

    // ── CreateSolidBrush (inetcpl / sysmon) ─────────────────────────────────
    HBRUSH WINAPI Overall_MyCreateSolidBrush(COLORREF color)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_CREATESOLIDBRUSH, decltype(&CreateSolidBrush)>(callerMod);

        if (!orig)
            return CreateSolidBrush(color);

        if (g_buttonSessionStack.empty())
            return orig(color);

        if (color == 0)
            return orig(0);

        HBRUSH alphaBrush = Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(color, 255);
        return alphaBrush ? alphaBrush : orig(color);
    }

    // ── GetSysColor ──────────────────────────────────────────────────────────
    int WINAPI Overall_MyGetSysColor(int nIndex)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_GETSYSCOLOR, decltype(&GetSysColor)>(callerMod);

        if (!g_buttonSessionStack.empty())
        {
            if (nIndex == COLOR_BTNFACE || nIndex == COLOR_WINDOW)
                return 0;
        }

        if (!orig)
            return GetSysColor(nIndex);
        return orig(nIndex);
    }

    // ── GetSysColorBrush ─────────────────────────────────────────────────────
    HBRUSH WINAPI Overall_MyGetSysColorBrush(int nIndex)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_GETSYSCOLORBR, decltype(&GetSysColorBrush)>(callerMod);

        if (!g_buttonSessionStack.empty())
        {
            if (nIndex == COLOR_BTNFACE || nIndex == COLOR_WINDOW)
                return static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        }

        if (!orig)
            return GetSysColorBrush(nIndex);
        return orig(nIndex);
    }

    // ── BitBlt (сессия Button, comctl32; не BS_ICON/BS_BITMAP — см. IsButtonIconOrBitmapStyle) ──
    // Как ListView: GdiAlphaBlend — обычный BitBlt на DWM-стекле даёт просвет без альфы.
    // Для icon/bitmap-кнопок внешнее звено не включается; если сессия всё же попала с таким HWND — orig.
    BOOL WINAPI Overall_MyBitBlt(
        HDC hdc, int x, int y, int cx, int cy,
        HDC hdcSrc, int x1, int y1, DWORD rop)
    {
        if (g_shutdown.load(std::memory_order_acquire))
            return BitBlt(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);

        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_BITBLT, decltype(&BitBlt)>(callerMod);

        if (!orig)
            return BitBlt(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);

        if (GlassCore::ScrollBarTheme::IsInsideScrollBarDrawThemeOrig())
            return orig(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);

        if (g_buttonSessionStack.empty())
            return orig(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);

        const ButtonPaintSession& sess = g_buttonSessionStack.back();
        if (!sess.hWnd)
            return orig(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);

        // Согласовано с EnableComctlBitBltForButtonPaint; защита от несоответствия стека/стиля.
        if (IsButtonIconOrBitmapStyle(sess.hWnd))
            return orig(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);

        if (rop != SRCCOPY || !cx || !cy || !hdcSrc)
            return orig(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);

        HBITMAP hSrcBitmap = static_cast<HBITMAP>(GetCurrentObject(hdcSrc, OBJ_BITMAP));
        if (!hSrcBitmap || Shared::BitmapUtils::IsOemBitmap(hSrcBitmap))
            return orig(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);

        if (Shared::BitmapUtils::HasAlphaChannel(hSrcBitmap))
        {
            BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            return GdiAlphaBlend(hdc, x, y, cx, cy, hdcSrc, x1, y1, cx, cy, bf);
        }

        wil::unique_hbitmap hConverted{ Shared::BitmapUtils::Promise32BPP(hSrcBitmap) };
        if (!hConverted)
            return orig(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);

        GlassCore::GdiUtils::ApplyIconBackgroundAlpha(hConverted.get(), g_buttonIconRemoval);

        HGDIOBJ hOldBitmap = SelectObject(hdcSrc, hConverted.get());
        auto restoreBitmap = wil::scope_exit([&] { SelectObject(hdcSrc, hOldBitmap); });

        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        return GdiAlphaBlend(hdc, x, y, cx, cy, hdcSrc, x1, y1, cx, cy, bf);
    }

    // ── DrawEdge (только IAT COMCTL32 + BS_SPLITBUTTON; узкий фильтр EDGE_SUNKEN|BF_LEFT) ──
    // Оригинальный DrawEdge в offscreen DIB + GdiAlphaBlend (AC_SRC_ALPHA), см. WinApiUtils::DrawEdgeWithConstantAlpha.
    BOOL WINAPI Overall_MyDrawEdge(HDC hdc, LPRECT qrc, UINT edge, UINT grfFlags)
    {
        if (g_shutdown.load(std::memory_order_acquire))
            return DrawEdge(hdc, qrc, edge, grfFlags);
        
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_DRAWEDGE, decltype(&DrawEdge)>(callerMod);

        if (!orig)
            return DrawEdge(hdc, qrc, edge, grfFlags);

        if (g_buttonSessionStack.empty())
            return orig(hdc, qrc, edge, grfFlags);

        const ButtonPaintSession& sess = g_buttonSessionStack.back();
        if (!IsSplitButtonStyle(sess.hWnd))
            return orig(hdc, qrc, edge, grfFlags);

        if (edge != EDGE_SUNKEN || (grfFlags & BF_LEFT) == 0)
            return orig(hdc, qrc, edge, grfFlags);

        if (!qrc || qrc->right <= qrc->left || qrc->bottom <= qrc->top)
            return orig(hdc, qrc, edge, grfFlags);

        const BYTE effAlpha =
            GlassCore::ThemePaint::GetEffectiveDrawThemeConstantAlphaForSession(sess.kind, sess.hWnd);
        DebugLog(DBG_NORMAL,
            L"[GlassCore][Button] Overall_MyDrawEdge: caller=%p edge=%d flags=0x%X rect=[%d,%d,%d,%d] effAlpha=%d",
			callerMod, edge, grfFlags, qrc->left, qrc->top, qrc->right, qrc->bottom, effAlpha);

        constexpr int kSplitDividerShrinkPx = 2;
        if (Shared::WinApiUtils::DrawEdgeWithConstantAlpha(
                reinterpret_cast<Shared::WinApiUtils::DrawEdge_pfn>(orig),
                hdc,
                qrc,
                edge,
                grfFlags,
                effAlpha,
                kSplitDividerShrinkPx))
            return TRUE;

        return orig(hdc, qrc, edge, grfFlags);
    }

    // =========================================================================
    //  PaintDispatcher provider callbacks
    // =========================================================================

    // Вызывается из PaintDispatcher при BeginPaint.
    static void OnBeginPaintGlobal(HWND hWnd, HMODULE callerMod)
    {
        using namespace GlassShared;
        const auto kind = GlassGetWindowKind(hWnd);
        if (kind != GlassControlKind::Button && kind != GlassControlKind::WinForms_Button)
            return;

        if (g_buttonSessionStack.empty())
            GlassCore::Settings::QueryIconBackgroundColorRemovalContext(
                { L"Root", L"Control", L"Button" }, g_buttonIconRemoval);

        g_buttonSessionStack.push_back({ callerMod, kind, hWnd });
        if (g_buttonSessionStack.size() == 1)
            g_splitButtonDtbDedupKey.reset();
        EnableButtonHooksForModule(hWnd, callerMod, kind, true);
        DebugLog(DBG_NORMAL,
            L"[GlassCore][Button] BeginPaint: callerMod=0x%p hWnd=0x%p kind=%d depth=%zu",
            callerMod, hWnd, static_cast<int>(kind), g_buttonSessionStack.size());
    }

    static void PopButtonPaintSession(HWND hWnd, const wchar_t* endReason) noexcept
    {
        using namespace GlassShared;
        const auto kind = GlassGetWindowKind(hWnd);
        if (kind != GlassControlKind::Button && kind != GlassControlKind::WinForms_Button)
            return;
        if (g_buttonSessionStack.empty()) return;

        const ButtonPaintSession s = g_buttonSessionStack.back();
        g_buttonSessionStack.pop_back();
        EnableButtonHooksForModule(s.hWnd ? s.hWnd : hWnd, s.mod, s.kind, false);
        DebugLog(DBG_NORMAL,
            L"[GlassCore][Button] %s: hWnd=0x%p depth=%zu",
            endReason, hWnd, g_buttonSessionStack.size());
    }

    // Вызывается из PaintDispatcher при EndPaint.
    static void OnEndPaintGlobal(HWND hWnd, HMODULE /*callerMod*/)
    {
        PopButtonPaintSession(hWnd, L"EndPaint");
    }

    // Вызывается из PaintDispatcher при GetDC (цепочка без BeginPaint).
    static void OnGetDCBeginGlobal(HWND hWnd, HMODULE callerMod)
    {
        using namespace GlassShared;
        const auto kind = GlassGetWindowKind(hWnd);
        if (kind != GlassControlKind::Button && kind != GlassControlKind::WinForms_Button)
            return;

        if (g_buttonSessionStack.empty())
            GlassCore::Settings::QueryIconBackgroundColorRemovalContext(
                { L"Root", L"Control", L"Button" }, g_buttonIconRemoval);

        g_buttonSessionStack.push_back({ callerMod, kind, hWnd });
        if (g_buttonSessionStack.size() == 1)
            g_splitButtonDtbDedupKey.reset();
        EnableButtonHooksForModule(hWnd, callerMod, kind, true);
        DebugLog(DBG_NORMAL,
            L"[GlassCore][Button] GetDC: callerMod=0x%p hWnd=0x%p kind=%d depth=%zu",
            callerMod, hWnd, static_cast<int>(kind), g_buttonSessionStack.size());
    }

    static void OnGetDCEndGlobal(HWND hWnd, HMODULE /*callerMod*/)
    {
        PopButtonPaintSession(hWnd, L"ReleaseDC");
    }

    // BWM WndList: erase=true для WinForms .BUTTON → PatBlt по rcPaint после BeginPaint.
    [[nodiscard]] static bool OnWinFormsButtonAfterBeginPaint(
        HWND hWnd, HDC hdc, LPPAINTSTRUCT ps, HMODULE /*callerMod*/) noexcept
    {
        if (g_shutdown.load(std::memory_order_acquire))
            return false;

        using namespace GlassShared;
        if (GlassGetWindowKind(hWnd) != GlassControlKind::WinForms_Button)
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

    bool IsInsideButtonPaint() { return !g_buttonSessionStack.empty(); }
    bool IsInsideWinFormsButtonPaint() { return !g_buttonSessionStack.empty(); }

    // =========================================================================
    //  Lifecycle
    // =========================================================================

    void Prepare() {}

    void Startup()
    {
        // Регистрация провайдеров в PaintDispatcher (до PaintDispatcher::Startup)
        GlassCore::PaintDispatcher::RegisterPaintProvider({ OnBeginPaintGlobal, OnEndPaintGlobal });
        GlassCore::PaintDispatcher::RegisterPostPaintProvider({ OnWinFormsButtonAfterBeginPaint });
        GlassCore::PaintDispatcher::RegisterGetDCProvider({ OnGetDCBeginGlobal, OnGetDCEndGlobal });

        // Пре-кеш IAT-слотов для известных модулей
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
                    DebugLog(DBG_NORMAL, L"[GlassCore][Button] Startup: cached 0x%p", mod);
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
        cacheModule(GetModuleHandleW(L"uxtheme.dll"));
    }

    void Shutdown()
    {
        g_shutdown.store(true, std::memory_order_seq_cst);

        DisableAllOverallHooks();

        g_buttonSessionStack.clear();
        g_splitButtonDtbDedupKey.reset();

        g_shutdown.store(false, std::memory_order_seq_cst);
    }

    void SyncTextGlowHooksFromRegistry() noexcept
    {
        if (g_shutdown.load(std::memory_order_acquire))
            return;

        if (void* inetcpl = GetModuleHandleW(L"inetcpl.cpl"))
            EnableHooksOnModule(inetcpl, OHK_DRAWTEXTEX, GlassCore::Text::IsInkGlowEffectActive());

        std::vector<ButtonPaintSession> sessions;
        sessions.reserve(g_buttonSessionStack.size());
        for (const ButtonPaintSession& s : g_buttonSessionStack)
            sessions.push_back(s);
        for (const ButtonPaintSession& s : sessions)
            EnableButtonHooksForModule(s.hWnd, s.mod, s.kind, false);
        for (const ButtonPaintSession& s : sessions)
            EnableButtonHooksForModule(s.hWnd, s.mod, s.kind, true);
    }

    namespace FormsNiDtbAccess
    {
    bool IsDrawThemeBackgroundDetourSessionActive() noexcept
    {
        if (GlassCore::ListBoxControl::IsInsideWinFormsListBoxPaint())
            return true;
        return !g_buttonSessionStack.empty()
            && g_buttonSessionStack.back().kind == GlassShared::GlassControlKind::WinForms_Button;
    }

    bool TryGetThemeSession(HDC hdc, ThemeSession& out) noexcept
    {
        ButtonPaintSession sess{};
        if (!TryResolveWinFormsNiButtonThemeSession(hdc, sess))
            return false;
        out.hWnd = sess.hWnd;
        out.kind = sess.kind;
        return true;
    }

    bool ShouldSkipDuplicateSplitDtb(
        const ThemeSession& sess,
        HWND hWnd,
        HTHEME hTheme,
        int iPartId,
        int iStateId,
        const RECT& drawRect) noexcept
    {
        if (sess.kind != GlassShared::GlassControlKind::Button)
            return false;

        SplitDtbDedupKey dedupKey{};
        dedupKey.hWnd    = hWnd;
        dedupKey.hTheme  = hTheme;
        dedupKey.partId  = iPartId;
        dedupKey.stateId = iStateId;
        dedupKey.rect    = drawRect;
        return g_splitButtonDtbDedupKey && *g_splitButtonDtbDedupKey == dedupKey;
    }

    void RememberSplitDtbDedup(
        HWND hWnd,
        HTHEME hTheme,
        int iPartId,
        int iStateId,
        const RECT& drawRect) noexcept
    {
        SplitDtbDedupKey dedupKey{};
        dedupKey.hWnd    = hWnd;
        dedupKey.hTheme  = hTheme;
        dedupKey.partId  = iPartId;
        dedupKey.stateId = iStateId;
        dedupKey.rect    = drawRect;
        g_splitButtonDtbDedupKey = dedupKey;
    }

    HRESULT TryDrawThemeBackground(
        Shared::WinApiUtils::DrawThemeBackground_pfn orig,
        HMODULE callerMod,
        HTHEME hTheme,
        HDC hdc,
        int iPartId,
        int iStateId,
        LPCRECT pRect,
        LPCRECT pClipRect,
        uint32_t seq) noexcept
    {
        return InvokeButtonDrawThemeBackgroundImpl(
            orig, callerMod, hTheme, hdc, iPartId, iStateId, pRect, pClipRect, seq);
    }
    }
}