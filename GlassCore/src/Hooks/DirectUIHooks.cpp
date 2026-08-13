// GlassCore/src/Hooks/DirectUIHooks.cpp
//
// Перехват отрисовки DirectUIHWND.
// Портировано с BWM/DUIHooks.cpp + PaintHandler (порядок как в BWM).
//
// Архитектура:
//
//   DUI70.dll — при загрузке модуля только IAT CreateSolidBrush (как BWM ModuleHandler).
//   Остальные слоты DUI70 (DrawTextW, DrawTextExW при glow, ExtTextOutW, SetBkColor, GetSysColorBrush,
//   StretchBlt, DrawThemeBackground) — только на время WM_PAINT DirectUIHWND,
//   когда caller BeginPaint — DUser.dll (PaintDispatcher + OnDuiPaintBegin/End).
//
//   DUser.dll / wdc.dll — без полного набора IAT при загрузке; ветка wdc (LineTo…)
//   остаётся в Root (AppendDirectUiRootPaintSpecsForHwnd + EnableRootHook на DUser).
//
//   Сопутствующие root-IAT на время той же paint-сессии (как BWM EnableHooks):
//     shell32 — DrawTextW; uxtheme — ExtTextOutW; comctl32 — DrawTextW (RootGdiHooks).
//
//   Detour только DrawThemeBackgroundEx на uxtheme — ref-счётчик по сессии DirectUIHWND
//   (как BWM EnableDrawThemeBackgroundEx). DrawThemeText глобально не цепляем в этой сессии.
//
//   comctl32 ScrollBar IAT — ScrollBarThemeHooks + PaintDispatcher::GetDCEx.
//

#include "CorePch.h"
#include <Hooks/DirectUI.h>
#include <Hooks/RootWindowHooks.h>
#include <Hooks/RootGdiHooks.h>
#include <Module/ModuleHandler.h>
#include <Core/Intercept/DebugDetourHooks.h>
#include <Core/Intercept/HookDispatcher.h>
#include <Core/Intercept/HookHelper.h>
#include <Subsystems/RenderText.h>
#include <Helpers/ThemeHelper.h>
#include <WinApiUtils.h>
#include <GlassBackdropMark.h>
#include <uxtheme.h>
#include <detours.h>
#include <intrin.h>
#include <unordered_map>
#include <vector>
#include <Subsystems/PaintDispatcher.h>

#pragma comment(lib, "msimg32.lib")

using namespace GlassCore::Hooks;
using namespace std::literals;

namespace GlassCore::DirectUI
{
    static std::atomic<bool> g_shutdown{ false };
    static std::atomic<bool> g_started{ false };
#if defined(_DEBUG)
    // Глобальный Detour BitBlt: сравнение с IAT ScrollBarTheme::ComctlScrollBarMyBitBlt.
    static std::atomic<bool> g_debugDetourBitBltFromPrepare{ false };
#endif

    // =========================================================================
    //  Реестр загруженных DUI-модулей (для фильтрации в Detours-хуках)
    // =========================================================================

    static wil::srwlock         g_moduleLock;
    static std::vector<HMODULE> g_duiModules;

    static bool IsDuiModule(HMODULE hMod)
    {
        if (!hMod) return false;
        auto lock = g_moduleLock.lock_shared();
        for (auto m : g_duiModules)
            if (m == hMod) return true;
        return false;
    }

    // =========================================================================
    //  IAT hook dependency — DrawTextW, ExtTextOutW, SetBkColor,
    //  GetSysColorBrush, CreateSolidBrush, StretchBlt, FillRect, DrawThemeBackground
    //
// =========================================================================

    int      WINAPI My_DrawTextW(HDC, LPCWSTR, int, LPRECT, UINT);
    int      WINAPI My_DrawTextExW(HDC, LPWSTR, int, LPRECT, UINT, LPDRAWTEXTPARAMS);
    BOOL     WINAPI My_ExtTextOutW(HDC, int, int, UINT, const RECT*, LPCWSTR, UINT, const INT*);
    COLORREF WINAPI My_SetBkColor(HDC, COLORREF);
    HBRUSH   WINAPI My_GetSysColorBrush(int);
    HBRUSH   WINAPI My_CreateSolidBrush(COLORREF);
    BOOL     WINAPI My_StretchBlt(HDC, int, int, int, int, HDC, int, int, int, int, DWORD);
    int      WINAPI My_FillRect(HDC, const RECT*, HBRUSH);
    HRESULT  WINAPI My_DrawThemeBackground(HTHEME, HDC, int, int, LPCRECT, LPCRECT);

    enum DuiIATIndex : size_t
    {
        OHK_DUI_DRAWTEXT = 0,
        OHK_DUI_EXTTEXTOUT,
        OHK_DUI_SETBKCOLOR,
        OHK_DUI_GETSYSCOLORBRUSH,
        OHK_DUI_CREATESOLIDBRUSH,
        OHK_DUI_STRETCHBLT,
        OHK_DUI_FILLRECT,
        OHK_DUI_DRAWTHEMEBACKGROUND,
        OHK_DUI_DRAWTEXTEX,
        OHK_DUI_IAT_COUNT
    };

    inline const HookDispatcherDependency<OHK_DUI_IAT_COUNT, 3> g_duiHookDep
    { {
        std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
            "DrawTextW", reinterpret_cast<PVOID>(My_DrawTextW) },
        std::tuple{ std::array{ "gdi32.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
            "ExtTextOutW", reinterpret_cast<PVOID>(My_ExtTextOutW) },
        std::tuple{ std::array{ "gdi32.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
            "SetBkColor", reinterpret_cast<PVOID>(My_SetBkColor) },
        std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
            "GetSysColorBrush", reinterpret_cast<PVOID>(My_GetSysColorBrush) },
        std::tuple{ std::array{ "gdi32.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
            "CreateSolidBrush", reinterpret_cast<PVOID>(My_CreateSolidBrush) },
        std::tuple{ std::array{ "gdi32.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
            "StretchBlt", reinterpret_cast<PVOID>(My_StretchBlt) },
        std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
            "FillRect", reinterpret_cast<PVOID>(My_FillRect) },
        std::tuple{ std::array{ "uxtheme.dll"sv, "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv, ""sv },
            "DrawThemeBackground", reinterpret_cast<PVOID>(My_DrawThemeBackground) },
        std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
            "DrawTextExW", reinterpret_cast<PVOID>(My_DrawTextExW) },
    } };

    using DuiDispatcher = HookDispatcher<OHK_DUI_IAT_COUNT, 3>;

    static wil::srwlock g_duiMapLock;
    static std::unordered_map<void*, DuiDispatcher> g_duiModuleMap;

    // LIFO по HWND: только EndPaint того же окна снимает сессию (вложенный Begin дочернего без push не ломает родителя).
    thread_local std::vector<HWND> g_duiPaintArmStack;

    // Оригинал слота только для DUI-карты (текст, GDI и т.д.)
    template <size_t index, typename T>
    static T GetOrigDui(HMODULE callerMod)
    {
        auto lock = g_duiMapLock.lock_shared();
        auto it = g_duiModuleMap.find(static_cast<void*>(callerMod));
        if (it != g_duiModuleMap.end())
            return it->second.GetOrg<index, T>();
        return nullptr;
    }

    [[nodiscard]] static decltype(&DrawThemeBackground) GetOrigDrawThemeBackgroundDui(HMODULE callerMod)
    {
        auto lock = g_duiMapLock.lock_shared();
        if (auto it = g_duiModuleMap.find(static_cast<void*>(callerMod)); it != g_duiModuleMap.end())
            return it->second.GetOrg<OHK_DUI_DRAWTHEMEBACKGROUND, decltype(&DrawThemeBackground)>();
        return nullptr;
    }

    static bool BaseNameEq(const std::wstring& base, LPCWSTR expect)
    {
        return _wcsicmp(base.c_str(), expect) == 0;
    }

    static bool IsWindowClass(HWND hWnd, const wchar_t* className)
    {
        if (!hWnd) return false;
        wchar_t buf[64]{};
        GetClassNameW(hWnd, buf, static_cast<int>(std::size(buf)));
        return _wcsicmp(buf, className) == 0;
    }
    // =========================================================================
    //  Detour uxtheme!DrawThemeBackgroundEx — только сессия DirectUI (BWM EnableDrawThemeBackgroundEx)
    // =========================================================================

    HRESULT WINAPI My_DrawThemeBackgroundEx(HTHEME, HDC, int, int, LPCRECT, const DTBGOPTS*);
    HRESULT WINAPI My_DrawThemeText(HTHEME, HDC, int, int, LPCWSTR, int, DWORD, DWORD, LPCRECT);

    static decltype(&My_DrawThemeBackgroundEx) g_origDTBEx{ nullptr };
    static decltype(&DrawThemeText)            g_origDTT{ nullptr };

    static wil::srwlock g_detoursLock;
    static size_t       g_dtbExPaintSessionRef{ 0 };
    // DetourAttach/Detach симметричны: Detach без предшествующего Attach даёт ERROR_INVALID_BLOCK (0x80070009).
    // В процессах без сессии DirectUIHWND (например host UI настроек) Attach никогда не вызывается — force-detach ломал shutdown.
    static bool         g_dtbExDetourAttached{ false };

    static void EnableDtbExPaintSessionDetour(bool enable)
    {
        auto lock = g_detoursLock.lock_exclusive();

        const bool changed = (enable && g_dtbExPaintSessionRef == 0) ||
            (!enable && g_dtbExPaintSessionRef == 1);
        g_dtbExPaintSessionRef += enable ? 1 : static_cast<size_t>(-1);
        if (!changed)
            return;

        DebugLog(L"[GlassCore][DirectUI] EnableDtbExPaintSessionDetour: %d", (int)enable);

        if (g_origDTBEx)
        {
            const HRESULT hr = Hooks::Detours::Write([&]() {
                if (enable)
                    Hooks::Detours::Attach(reinterpret_cast<PVOID*>(&g_origDTBEx), My_DrawThemeBackgroundEx);
                else
                    Hooks::Detours::Detach(reinterpret_cast<PVOID*>(&g_origDTBEx), My_DrawThemeBackgroundEx);
            });
            if (SUCCEEDED(hr))
                g_dtbExDetourAttached = enable;
            DebugLog(L"[GlassCore][DirectUI] DTBEx detour %s hr=0x%08x",
                enable ? L"attach" : L"detach", hr);
        }
    }

    // Shutdown: принудительно снять DTBEx (DrawThemeText в эту сессию не ставим).
    static void DisableAllUxThemeDetours() noexcept
    {
        auto lock = g_detoursLock.lock_exclusive();
        DebugLog(L"[GlassCore][DirectUI] DisableAllUxThemeDetours: было dtbExRef=%zu attached=%d",
            g_dtbExPaintSessionRef, (int)g_dtbExDetourAttached);
        g_dtbExPaintSessionRef = 0;

        if (!g_origDTBEx || !g_dtbExDetourAttached)
            return;

        const HRESULT hr = Hooks::Detours::Write([&]() {
            Hooks::Detours::Detach(reinterpret_cast<PVOID*>(&g_origDTBEx), My_DrawThemeBackgroundEx);
        });
        if (SUCCEEDED(hr))
            g_dtbExDetourAttached = false;
        DebugLog(L"[GlassCore][DirectUI] DTBEx detour force-detach hr=0x%08x", hr);
    }

    // =========================================================================
    //  Реализации IAT-хуков
    //  Фильтрация не нужна: IAT-хуки срабатывают только для вызовов из модуля.
    // =========================================================================

    int WINAPI My_DrawTextW(HDC hdc, LPCWSTR lpchText, int cchText, LPRECT lprc, UINT format)
    {
        HMODULE callerMod = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(_ReturnAddress()), &callerMod);
        auto orig = GetOrigDui<OHK_DUI_DRAWTEXT, decltype(&DrawTextW)>(callerMod);

        if ((format & DT_CALCRECT) || (format & DT_INTERNAL) || (format & DT_NOCLIP))
            return orig ? orig(hdc, lpchText, cchText, lprc, format)
            : DrawTextW(hdc, lpchText, cchText, lprc, format);

        //DebugLog(L"[GlassCore][DirectUI] DrawTextW: \"%s\"", lpchText ? lpchText : L"");

        int result{ 0 };
        if (!FAILED(GlassCore::Text::DrawTextWithAlpha(hdc, lpchText, cchText, lprc, format, result)))
            return result;

        return orig ? orig(hdc, lpchText, cchText, lprc, format)
            : DrawTextW(hdc, lpchText, cchText, lprc, format);
    }

    int WINAPI My_DrawTextExW(
        HDC hdc, LPWSTR lpchText, int cchText,
        LPRECT lprc, UINT format, LPDRAWTEXTPARAMS lpdtp)
    {
        HMODULE callerMod = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(_ReturnAddress()), &callerMod);
        auto orig = GetOrigDui<OHK_DUI_DRAWTEXTEX, decltype(&DrawTextExW)>(callerMod);
        if (!orig)
            return DrawTextExW(hdc, lpchText, cchText, lprc, format, lpdtp);
        if (g_duiPaintArmStack.empty())
            return orig(hdc, lpchText, cchText, lprc, format, lpdtp);
        if (lpdtp)
            return orig(hdc, lpchText, cchText, lprc, format, lpdtp);
        int result = 0;
        if (!FAILED(GlassCore::Text::DrawTextWithAlpha(hdc, lpchText, cchText, lprc, format, result)))
            return result;
        return orig(hdc, lpchText, cchText, lprc, format, lpdtp);
    }

    BOOL WINAPI My_ExtTextOutW(
        HDC hdc, int x, int y, UINT options, const RECT* lprect,
        LPCWSTR lpString, UINT c, const INT* lpDx)
    {
        HMODULE callerMod = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(_ReturnAddress()), &callerMod);
        auto orig = GetOrigDui<OHK_DUI_EXTTEXTOUT, decltype(&ExtTextOutW)>(callerMod);

        int result{ S_OK };
        DWORD dwTextFlags{ 0 };

        if (options & ETO_OPAQUE)
        {
            COLORREF color = GetBkColor(hdc);
            if (color == GetSysColor(COLOR_HIGHLIGHT) /*|| color == GetSysColor(COLOR_BTNFACE)*/)
            {
                wil::unique_hbrush brush{
                    Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(color, 255) };
                if (brush)
                    FillRect(hdc, lprect, brush.get());
            }
            else
            {
                PatBlt(hdc, lprect->left, lprect->top,
                    lprect->right - lprect->left, lprect->bottom - lprect->top, BLACKNESS);
            }
            if (!lpString || c == 0) return TRUE;
        }

        if (options == ETO_IGNORELANGUAGE || options == ETO_GLYPH_INDEX)
            return orig ? orig(hdc, x, y, options, lprect, lpString, c, lpDx)
            : ExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);

        if (!lpString || c == 0)
            return orig ? orig(hdc, x, y, options, lprect, lpString, c, lpDx)
            : ExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);

        //DebugLog(L"[GlassCore][DirectUI] ExtTextOutW: \"%s\"", lpString);

        RECT rc{};
        if (lprect) rc = *lprect;
        else        GetClipBox(hdc, &rc);
        rc.left = x;
        rc.top = y;

        dwTextFlags |= (options & ETO_RTLREADING) ? (DT_RIGHT | DT_RTLREADING) : DT_LEFT;
        SetBkMode(hdc, TRANSPARENT);

        if (!FAILED(GlassCore::Text::DrawTextWithAlpha(hdc, lpString, c, &rc, dwTextFlags, result)))
            return result;

        return orig ? orig(hdc, x, y, options, lprect, lpString, c, lpDx)
            : ExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);
    }

    COLORREF WINAPI My_SetBkColor(HDC hdc, COLORREF color)
    {
        HMODULE callerMod = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(_ReturnAddress()), &callerMod);
        auto orig = GetOrigDui<OHK_DUI_SETBKCOLOR, decltype(&SetBkColor)>(callerMod);

        if ((0xFFFFFF & color) == GetSysColor(COLOR_WINDOW))
        {
            //DebugLog(L"[GlassCore][DirectUI] SetBkColor: replace COLOR_WINDOW → 0");
            color = 0;
        }
        return orig ? orig(hdc, color) : SetBkColor(hdc, color);
    }

    HBRUSH WINAPI My_GetSysColorBrush(int nIndex)
    {
        HMODULE callerMod = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(_ReturnAddress()), &callerMod);
        auto orig = GetOrigDui<OHK_DUI_GETSYSCOLORBRUSH, decltype(&GetSysColorBrush)>(callerMod);

        if (nIndex == COLOR_WINDOW || nIndex == COLOR_BTNFACE || nIndex == COLOR_MENU)
        {
            //DebugLog(L"[GlassCore][DirectUI] GetSysColorBrush: idx=%d → BLACK_BRUSH", nIndex);
            return GetStockBrush(BLACK_BRUSH);
        }
        return orig ? orig(nIndex) : GetSysColorBrush(nIndex);
    }

    HBRUSH WINAPI My_CreateSolidBrush(COLORREF color)
    {
        HMODULE callerMod = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(_ReturnAddress()), &callerMod);
        auto orig = GetOrigDui<OHK_DUI_CREATESOLIDBRUSH, decltype(&CreateSolidBrush)>(callerMod);

        // Фон горячих пунктов дерева и разделителей в файловых диалогах.
        // BWM pattern: DUIHooks::MyCreateSolidBrush.
        if (color == GetSysColor(COLOR_WINDOW)
            || color == GetSysColor(COLOR_BTNFACE)
            || Shared::ThemeHelper::IsThemeColor(color, L"ExplorerNavPane")
            || Shared::ThemeHelper::IsThemeColor(color, L"ProperTree"))
        {
            //DebugLog(L"[GlassCore][DirectUI] CreateSolidBrush: 0x%06x → black", (DWORD)color);
            color = RGB(0, 0, 0);
        }
        return orig ? orig(color) : CreateSolidBrush(color);
    }

    BOOL WINAPI My_StretchBlt(
        HDC hdcDest, int xDest, int yDest, int wDest, int hDest,
        HDC hdcSrc, int xSrc, int ySrc, int wSrc, int hSrc, DWORD rop)
    {
        HMODULE callerMod = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(_ReturnAddress()), &callerMod);
        auto orig = GetOrigDui<OHK_DUI_STRETCHBLT, decltype(&StretchBlt)>(callerMod);

        //DebugLog(L"[GlassCore][DirectUI] StretchBlt: rop=0x%08x → BLACKNESS", rop);
        rop = BLACKNESS;

        return orig
            ? orig(hdcDest, xDest, yDest, wDest, hDest, hdcSrc, xSrc, ySrc, wSrc, hSrc, rop)
            : StretchBlt(hdcDest, xDest, yDest, wDest, hDest, hdcSrc, xSrc, ySrc, wSrc, hSrc, rop);
    }

    int WINAPI My_FillRect(HDC hDC, const RECT* lprc, HBRUSH hbr)
    {
        HMODULE callerMod = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(_ReturnAddress()), &callerMod);
        auto orig = GetOrigDui<OHK_DUI_FILLRECT, decltype(&FillRect)>(callerMod);

        if (hbr && lprc)
        {
            LOGBRUSH lb{};
            if (GetObject(hbr, sizeof(lb), &lb) && lb.lbStyle == BS_SOLID &&
                (lb.lbColor == GetSysColor(COLOR_WINDOW) ||
                    lb.lbColor == GetSysColor(COLOR_BTNFACE)))
            {
                //DebugLog(L"[GlassCore][DirectUI] FillRect: background → BLACKNESS");
                PatBlt(hDC, lprc->left, lprc->top,
                    lprc->right - lprc->left, lprc->bottom - lprc->top, BLACKNESS);
                return 1;
            }
        }
        return orig ? orig(hDC, lprc, hbr) : FillRect(hDC, lprc, hbr);
    }

    // Тема ScrollBar для comctl32 — только ScrollBarThemeHooks (IAT по GetDCEx).
    HRESULT WINAPI My_DrawThemeBackground(
        HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
        LPCRECT pRect, LPCRECT pClipRect)
    {
        HMODULE callerMod = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(_ReturnAddress()), &callerMod);
        auto orig = GetOrigDrawThemeBackgroundDui(callerMod);
        if (!orig)
            return DrawThemeBackground(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
        if (g_shutdown.load(std::memory_order_acquire))
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
        return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
    }

    // =========================================================================
    //  Реализации Detours-хуков (DrawThemeBackgroundEx, DrawThemeText)
    //  Фильтр по caller: только DUI-модули
    // =========================================================================

    HRESULT WINAPI My_DrawThemeBackgroundEx(
        HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
        LPCRECT pRect, const DTBGOPTS* pOptions)
    {
        if (!IsDuiModule(DetourGetContainingModule(_ReturnAddress())))
            return g_origDTBEx(hTheme, hdc, iPartId, iStateId, pRect, pOptions);

        WCHAR cls[MAX_PATH + 1]{};
        bool classOk = SUCCEEDED(
            Shared::ThemeHelper::GetThemeClass(hTheme, cls, MAX_PATH));

        auto blackout = [&]() -> bool
            {
                if (!pRect) return false;
                PatBlt(hdc, pRect->left, pRect->top,
                    pRect->right - pRect->left, pRect->bottom - pRect->top, BLACKNESS);
                return true;
            };

        if (classOk)
        {
            //DebugLog(L"[GlassCore][DirectUI] DrawThemeBackgroundEx: class=\"%s\" part=%d state=%d",
            //    cls, iPartId, iStateId);
            if (_wcsicmp(cls, L"Header") == 0
                && iPartId < 2 && (iStateId == 1 || iStateId == 4)) {
                if (blackout()) return S_OK;
            }
            else if (_wcsicmp(cls, L"ControlPanel") == 0
                && iPartId == 2 && iStateId == 0) {
                if (blackout()) return S_OK;
            }
            else if (_wcsicmp(cls, L"CommandModule") == 0
                && (iPartId == 1 || iPartId == 2) && iStateId == 0) {
                if (blackout()) return S_OK;
            }
            else if (_wcsicmp(cls, L"PreviewPane") == 0
                && (iPartId == 1 || iPartId == 3 || iPartId == 4)) {
                if (blackout()) return S_OK;
            }
            else if (_wcsicmp(cls, L"AeroWizard") == 0
                && (iPartId == 3 || iPartId == 4)) {
                if (blackout()) return S_OK;
            }
            else if (_wcsicmp(cls, L"CommonItemsDialog") == 0
                && (iPartId == 0 || iPartId == 1)) {
                if (blackout()) return S_OK;
            }
            else if (_wcsicmp(cls, L"TaskDialog") == 0
                && ((iPartId == 1 || iPartId == 8) && iStateId == 0)) {
                if (blackout()) return S_OK;
            }
            else if (_wcsicmp(cls, L"Tab") == 0
                && (iStateId == 0 || iStateId == 1)) {
                if (blackout()) return S_OK;
            }
        }

        return g_origDTBEx(hTheme, hdc, iPartId, iStateId, pRect, pOptions);
    }

    HRESULT WINAPI My_DrawThemeText(
        HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
        LPCWSTR pszText, int cchText, DWORD dwTextFlags, DWORD dwTextFlags2,
        LPCRECT pRect)
    {
        if (!IsDuiModule(DetourGetContainingModule(_ReturnAddress())))
            return g_origDTT(hTheme, hdc, iPartId, iStateId,
                pszText, cchText, dwTextFlags, dwTextFlags2, pRect);

        if (!pszText || cchText == 0 || !pRect)
            return g_origDTT(hTheme, hdc, iPartId, iStateId,
                pszText, cchText, dwTextFlags, dwTextFlags2, pRect);

        /*DebugLog(L"[GlassCore][DirectUI] DrawThemeText: part=%d state=%d \"%s\"",
            iPartId, iStateId, pszText);*/

        RECT rc = *pRect;
        int result{ 0 };
        if (!FAILED(GlassCore::Text::DrawTextWithAlpha(hdc, pszText, cchText, &rc, dwTextFlags, result)))
            return S_OK;

        return g_origDTT(hTheme, hdc, iPartId, iStateId,
            pszText, cchText, dwTextFlags, dwTextFlags2, pRect);
    }

    // =========================================================================
    //  IAT: DUI70 — CSB при загрузке; остальное — сессия paint (BWM EnableHooks)
    // =========================================================================

    static void InstallDui70LoadHooks(HMODULE hModule)
    {
        if (!hModule) return;

        {
            auto lock = g_duiMapLock.lock_shared();
            if (g_duiModuleMap.count(static_cast<void*>(hModule))) return;
        }

        auto lock = g_duiMapLock.lock_exclusive();
        if (g_duiModuleMap.count(static_cast<void*>(hModule))) return;

        auto [it, ok] = g_duiModuleMap.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(static_cast<void*>(hModule)),
            std::forward_as_tuple(g_duiHookDep, static_cast<void*>(hModule)));

        if (!ok) return;

        it->second.CacheHookData();
        it->second.EnableHook(OHK_DUI_CREATESOLIDBRUSH, true);
        DebugLog(L"[GlassCore][DirectUI] DUI70 load hooks: CreateSolidBrush only, module=%p", hModule);
    }

    static void ApplyDui70SessionIat(HMODULE dui70, HWND hWnd, bool enable)
    {
        if (!dui70) return;

        auto lock = g_duiMapLock.lock_exclusive();
        auto it = g_duiModuleMap.find(static_cast<void*>(dui70));
        if (it == g_duiModuleMap.end())
            return;

        DuiDispatcher& d = it->second;
        d.EnableHook(OHK_DUI_DRAWTEXT, enable);
        d.EnableHook(OHK_DUI_EXTTEXTOUT, enable);
        d.EnableHook(OHK_DUI_SETBKCOLOR, enable);
        d.EnableHook(OHK_DUI_GETSYSCOLORBRUSH, enable);
        d.EnableHook(OHK_DUI_STRETCHBLT, enable);
        if (enable && GlassCore::Text::IsInkGlowEffectActiveForPaintSession(hWnd))
            d.EnableHook(OHK_DUI_DRAWTEXTEX, true);
        else if (!enable)
            d.EnableHook(OHK_DUI_DRAWTEXTEX, false);
    }

    static void SetShellUxComctlDrawSession(bool enable) noexcept
    {
        using namespace GlassCore::Root;
        if (void* m = GetModuleHandleW(L"shell32.dll"))
            EnableRootHook(m, OHK_DRAWTEXT, enable);
        if (void* m = GetModuleHandleW(L"uxtheme.dll"))
            EnableRootHook(m, OHK_EXTTEXTOUT, enable);

        bool anyComctl = false;
        if (GlassCore::ModuleHandler::g_comctl32New)
        {
            anyComctl = true;
            EnableRootHook(GlassCore::ModuleHandler::g_comctl32New, OHK_DRAWTEXT, enable);
        }
        if (GlassCore::ModuleHandler::g_comctl32Legacy)
        {
            anyComctl = true;
            EnableRootHook(GlassCore::ModuleHandler::g_comctl32Legacy, OHK_DRAWTEXT, enable);
        }
        if (!anyComctl)
        {
            if (HMODULE h = GetModuleHandleW(L"comctl32.dll"))
                EnableRootHook(h, OHK_DRAWTEXT, enable);
        }
    }

    static void UninstallDui70MapEntry(HMODULE hModule)
    {
        if (!hModule) return;

        auto lock = g_duiMapLock.lock_exclusive();
        auto it = g_duiModuleMap.find(static_cast<void*>(hModule));
        if (it == g_duiModuleMap.end()) return;

        it->second.DisableAllHooks();
        g_duiModuleMap.erase(it);
        DebugLog(L"[GlassCore][DirectUI] DUI70 IAT map entry removed, module=%p", hModule);
    }

    // =========================================================================
    //  Provider callbacks — PaintDispatcher
    // =========================================================================

    void OnDuiPaintBegin(HWND hWnd, HMODULE callerModule)
    {
        if (g_shutdown.load(std::memory_order_acquire))
            return;

        if (!hWnd || !IsWindow(hWnd))
            return;

        if (!GlassShared::GlassIsBackdropAllowedForHwnd(hWnd))
            return;

        if (!IsWindowClass(hWnd, L"DirectUIHWND"))
            return;

        std::wstring callerModuleName = Shared::WinApiUtils::GetModuleFileBaseName(callerModule);
        if (!BaseNameEq(callerModuleName, L"DUser.dll"))
            return;

        HMODULE dui70 = GetModuleHandleW(L"DUI70.dll");
        if (!dui70)
            return;

        //DebugLog(L"[GlassCore][DirectUI] OnDuiPaintBegin: caller=%s", callerModuleName.c_str());

        ApplyDui70SessionIat(dui70, hWnd, true);
        SetShellUxComctlDrawSession(true);
        EnableDtbExPaintSessionDetour(true);
        g_duiPaintArmStack.push_back(hWnd);
    }

    void OnDuiPaintEnd(HWND hWnd, HMODULE callerModule)
    {
        (void)callerModule;

        if (g_shutdown.load(std::memory_order_acquire))
            return;

        if (g_duiPaintArmStack.empty() || g_duiPaintArmStack.back() != hWnd)
            return;

        g_duiPaintArmStack.pop_back();

        //DebugLog(L"[GlassCore][DirectUI] OnDuiPaintEnd: hwnd=0x%p stack=%zu",
        //    hWnd, g_duiPaintArmStack.size());

        EnableDtbExPaintSessionDetour(false);
        SetShellUxComctlDrawSession(false);
        if (HMODULE dui70 = GetModuleHandleW(L"DUI70.dll"))
            ApplyDui70SessionIat(dui70, hWnd, false);
    }


    // =========================================================================
    //  Lifecycle
    // =========================================================================

    void Prepare()
    {
        DebugLog(L"[GlassCore][DirectUI] Prepare");

        HMODULE hUxTheme = GetModuleHandleW(L"uxtheme.dll");
        if (hUxTheme)
        {
            if (!g_origDTBEx)
                g_origDTBEx = reinterpret_cast<decltype(g_origDTBEx)>(
                    GetProcAddress(hUxTheme, "DrawThemeBackgroundEx"));
            if (!g_origDTT)
                g_origDTT = reinterpret_cast<decltype(g_origDTT)>(
                    GetProcAddress(hUxTheme, "DrawThemeText"));
        }

        DebugLog(L"[GlassCore][DirectUI] Prepare done: DTBEx=%p DTT=%p",
            g_origDTBEx, g_origDTT);

    }

    void Startup()
    {
        if (g_started.exchange(true)) return;
        Prepare();
        DebugLog(L"[GlassCore][DirectUI] Startup done");

        GlassCore::PaintDispatcher::RegisterPaintProvider({ OnDuiPaintBegin, OnDuiPaintEnd });
    }

    void Shutdown()
    {
        if (!g_started.exchange(false)) return;

        DebugLog(L"[GlassCore][DirectUI] Shutdown: begin");
        g_shutdown.store(true, std::memory_order_release);

        g_duiPaintArmStack.clear();

        DisableAllUxThemeDetours();

        {
            auto lock = g_duiMapLock.lock_exclusive();
            for (auto& [mod, d] : g_duiModuleMap)
                d.DisableAllHooks();
            g_duiModuleMap.clear();
        }

        {
            auto lock = g_moduleLock.lock_exclusive();
            g_duiModules.clear();
        }

        DebugLog(L"[GlassCore][DirectUI] Shutdown: done");
    }

    // =========================================================================
    //  Module notifications (из ModuleHandler::HandleModuleAttach)
    // =========================================================================

    void OnDuiModuleLoaded(HMODULE hModule, const wchar_t* baseName)
    {
        if (g_shutdown.load(std::memory_order_acquire)) return;

        // Идемпотентно: если Load пришёл до Engine (не бывает) или порядок инициализации изменён —
        // без адресов uxtheme в Prepare детур DTBEx не прикрепится.
        Prepare();

        DebugLog(L"[GlassCore][DirectUI] DUI module loaded: %s (%p)", baseName, hModule);

        {
            auto lock = g_moduleLock.lock_exclusive();
            for (auto m : g_duiModules)
                if (m == hModule) return;
            g_duiModules.push_back(hModule);
        }

        if (baseName && _wcsicmp(baseName, L"DUI70.dll") == 0)
            InstallDui70LoadHooks(hModule);
    }

    void OnDuiModuleUnloaded(HMODULE hModule, const wchar_t* baseName)
    {
        DebugLog(L"[GlassCore][DirectUI] DUI module unloaded: %s (%p)", baseName, hModule);

        {
            auto lock = g_moduleLock.lock_exclusive();
            auto it = std::find(g_duiModules.begin(), g_duiModules.end(), hModule);
            if (it == g_duiModules.end()) return;
            g_duiModules.erase(it);
        }

        if (baseName && _wcsicmp(baseName, L"DUI70.dll") == 0)
            UninstallDui70MapEntry(hModule);
    }

    void OnDirectUIRootHooksAttach(HWND hDuiWnd) noexcept
    {
        GlassCore::Root::AppendDirectUiRootPaintSpecsForHwnd(hDuiWnd);
    }

    HWND GetPaintingDUI() noexcept { return nullptr; }

    bool IsShuttingDown() noexcept
    {
        return g_shutdown.load(std::memory_order_acquire);
    }
}