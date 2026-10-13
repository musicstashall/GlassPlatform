// Хуки для отрисовки TreeView (SysTreeView32).
//
// Архитектура (PaintDispatcher + per-module g_overallMap):
//
// TreeView: текст — ExtTextOutW; при themed — DTTEx (comctl32) или DrawTextExW (uxtheme) в зависимости от Glow,
// фон — DrawThemeBackground, иконки — MaskBlt.
// Хуки включаются на callerModule при каждом BeginPaint:
//   callerModule определяется через _ReturnAddress() в хуке PaintDispatcher.
//
// BWM-паттерн: PaintHandler.cpp TreeView:
//   g_comctl32New:    ExtTextOutW, MaskBlt
//                     + если IsThemed → DrawThemeBackground и взаимоисключающе:
//                       Glow → только DrawTextExW (IAT uxtheme.dll);
//                       без Glow → только DrawThemeTextEx (IAT comctl32).
//   g_comctl32Legacy: ExtTextOutW
//
// Иконки без альфа-канала:
//   ApplyBlackKeyAlpha (аналог BWM GdipCreateBitmapAlpha) + GdiAlphaBlend.

#include "CorePch.h"

#include <Hooks/TreeView.h>
#include <Hooks/ScrollBarDrawTheme.h>
#include <Hooks/ScrollBarTheme.h>
#include <Subsystems/GdiHookUtils.h>
#include <Subsystems/Settings.h>
#include <Subsystems/RenderText.h>
#include <Helpers/ThemeHelper.h>
#include <Core/Intercept/HookDispatcher.h>
#include <Module/ModuleHandler.h>
#include <Subsystems/PaintDispatcher.h>
#include <GlassControlKind.h>
#include <BitmapUtils.h>
#include <wil.h>
#include <CommCtrl.h>
#include <uxtheme.h>
#include <WinApiUtils.h>
#include <vector>
#include <cmath>

#pragma comment(lib, "msimg32.lib")

using namespace GlassCore::Hooks;
using namespace std::literals;

namespace GlassCore::TreeViewControl
{
    // =========================================================================
    //  LIFO сессий paint (модуль + HWND для симметричного EndPaint).
    // =========================================================================
    //
    // Взаимоисключение Glow vs DTTEx задаётся внутри EnableTreeViewHooksForModule(enable):
    // те же условия при enable=true и enable=false — счётчики HookDispatcher согласуют вложенность.

    struct TreeViewPaintSession { HMODULE mod; HWND hWnd; };
    thread_local std::vector<TreeViewPaintSession> g_treeSessionStack;

    // Удаление фона иконок (Root\Control\TreeView) — читается на входе внешней paint-сессии;
    // enable=false → black-key по умолчанию (см. GdiUtils::ApplyIconBackgroundAlpha).
    thread_local WndCompat::Menu::IconBackgroundColorRemoval g_treeIconRemoval{};

    static std::atomic<bool> g_shutdown{ false };

    // =========================================================================
    //  Helpers
    // =========================================================================

    static bool StrEq(LPCWSTR a, LPCWSTR b)
    {
        return _wcsicmp(a, b) == 0;
    }

    [[nodiscard]] static int GetScrollBandSizeForDpiCached(HDC hdc) noexcept
    {
        static thread_local UINT s_cachedDpi{ 0 };
        static thread_local int  s_cachedBand{ 0 };

        HWND hwnd = WindowFromDC(hdc);
        if (!hwnd || !IsWindow(hwnd))
            hwnd = GlassCore::PaintDispatcher::GetPaintSessionHostHwnd();

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

    // Black-key / настроенный chroma — общий код в GdiUtils::ApplyIconBackgroundAlpha
    // (Root\Control\TreeView\ColorTreatAsTransparent; без ключа — black-key как раньше).

    // =========================================================================
    //  OVERALL DISPATCHER MAP
    // =========================================================================

    int      WINAPI Overall_MyDrawThemeBg(HTHEME, HDC, int, int, LPCRECT, LPCRECT);
    HRESULT  WINAPI Overall_MyDrawThemeTextEx(HTHEME, HDC, int, int, LPCWSTR, int, DWORD, LPRECT, const DTTOPTS*);
    BOOL     WINAPI Overall_MyExtTextOutW(HDC, int, int, UINT, const RECT*, LPCWSTR, UINT, const INT*);
    BOOL     WINAPI Overall_MyFillRect(HDC, const RECT*, HBRUSH);
    BOOL     WINAPI Overall_MyMaskBlt(HDC, int, int, int, int, HDC, int, int, HBITMAP, int, int, DWORD);
    int      WINAPI Overall_MyDrawTextExW(HDC, LPWSTR, int, LPRECT, UINT, LPDRAWTEXTPARAMS);

    enum OverallHookIndex : size_t
    {
        OHK_DRAWTHEMEBG = 0,
        OHK_EXTTEXTOUT,
        OHK_FILLRECT,
        OHK_MASKBLT,
        OHK_DRAWTHEMETEXTEX,
        OHK_DRAWTEXTEX,
        OHK_COUNT
    };

    inline const HookDispatcherDependency<OHK_COUNT, 3> g_overallHookDep
    { {
        // OHK_DRAWTHEMEBG
        std::tuple{ std::array{ "uxtheme.dll"sv, "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv, ""sv },
            "DrawThemeBackground", reinterpret_cast<PVOID>(Overall_MyDrawThemeBg) },
        // OHK_EXTTEXTOUT
        std::tuple{ std::array{ "gdi32.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
            "ExtTextOutW", reinterpret_cast<PVOID>(Overall_MyExtTextOutW) },
        // OHK_FILLRECT
        std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, ""sv },
            "FillRect", reinterpret_cast<PVOID>(Overall_MyFillRect) },
        // OHK_MASKBLT
        std::tuple{ std::array{ "gdi32.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
            "MaskBlt", reinterpret_cast<PVOID>(Overall_MyMaskBlt) },
        // OHK_DRAWTHEMETEXTEX
        std::tuple{ std::array{ "uxtheme.dll"sv, "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv, ""sv },
            "DrawThemeTextEx", reinterpret_cast<PVOID>(Overall_MyDrawThemeTextEx) },
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

    thread_local int g_menuThemedTextDrawDepth = 0;

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
    // Тема "TreeView" (iPartId < 2, iStateId < 2): PatBlt(BLACKNESS) — прозрачный фон.
    // Тема "ScrollBar": политика PatBlt / SIZEBOX / glass — здесь (примитивы ScrollBarDrawTheme).
    // Иначе цепочка TreeView→ListView при пустом стеке LV сразу уходит в uxtheme и обходит политику ScrollBarDrawTheme.
    // Если IAT ScrollBar не ставился — sbOrig==nullptr, используем chainOrig (как раньше без карты).
    // BWM pattern: OverallHooks.cpp line 593-596.
    int WINAPI Overall_MyDrawThemeBg(
        HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
        LPCRECT pRect, LPCRECT pClipRect)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_DRAWTHEMEBG, decltype(&DrawThemeBackground)>(callerMod);

        // IATRegistry chain: _ReturnAddress() может указывать на другой хук GlassCore,
        // а не на целевой модуль → callerMod не в нашей карте → orig == nullptr
        if (!orig)
            return DrawThemeBackground(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

        if (g_shutdown.load(std::memory_order_acquire))
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

        if (g_treeSessionStack.empty())
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

        WCHAR themeClass[MAX_PATH + 1]{};

        if (SUCCEEDED(Shared::ThemeHelper::GetThemeClass(hTheme, themeClass, MAX_PATH)))
        {
            DebugLog(DBG_NORMAL, L"[GlassCore][TreeView] Overall_MyDrawThemeBg: caller=%p themeClass=\"%ls\" part=%d state=%d",
                callerMod, themeClass, iPartId, iStateId);
            if (StrEq(themeClass, L"TreeView")
            && (iPartId < 2 && iStateId < 2)
            && pRect && !IsRectEmpty(pRect))
            {
                const RECT* clipRect = pClipRect ? pClipRect : pRect;
                PatBlt(hdc,
                    clipRect->left, clipRect->top,
                    clipRect->right - clipRect->left,
                    clipRect->bottom - clipRect->top,
                    BLACKNESS);
                return S_OK;
            }

            if (StrEq(themeClass, L"ScrollBar"))
            {
                auto sbOrig = GlassCore::ScrollBarTheme::GetComctlScrollBarDrawThemeOrig(callerMod);
                auto dtbOrig = sbOrig ? sbOrig : orig;
                if (!dtbOrig)
                    return DrawThemeBackground(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

                GlassCore::ScrollBarDrawTheme::LogScrollBarDrawThemeCall(
                    L"[GlassCore][TreeView]",
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
                return static_cast<int>(dtbOrig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect));
            }
        }

        return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
    }

    // ── DrawThemeTextEx (themed TreeView; DTT не перехватываем) ──────────────
    HRESULT WINAPI Overall_MyDrawThemeTextEx(
        HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
        LPCWSTR pszText, int cchText, DWORD dwTextFlags,
        LPRECT pRect, const DTTOPTS* pOptions)
    {
        if (g_shutdown.load(std::memory_order_acquire))
            return DrawThemeTextEx(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions);

        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_DRAWTHEMETEXTEX, decltype(&DrawThemeTextEx)>(callerMod);

        if (!orig)
            return DrawThemeTextEx(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions);

        if (g_treeSessionStack.empty())
            return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions);

        WCHAR themeClass[MAX_PATH + 1]{};
        if (!SUCCEEDED(Shared::ThemeHelper::GetThemeClass(hTheme, themeClass, MAX_PATH))
            || !StrEq(themeClass, L"TreeView"))
            return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions);

        if (!pRect || IsRectEmpty(pRect))
            return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions);

        const DTTOPTS* effectiveOpts = pOptions;
        DTTOPTS        defaultOpts{ sizeof(DTTOPTS) };
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

    // ── ExtTextOutW ──────────────────────────────────────────────────────────
    // Основной путь отрисовки текста TreeView.
    // ETO_OPAQUE: PatBlt(BLACKNESS) + FillRect для подсветки выделения (COLOR_HIGHLIGHT).
    // Текст: DrawTextWithAlpha для корректного alpha-рендеринга.
    // BWM pattern: ListView::WorkExtTextOutW (WinHooks.cpp 814-873).
    BOOL WINAPI Overall_MyExtTextOutW(
        HDC hdc, int x, int y, UINT options, const RECT* lprect,
        LPCWSTR lpString, UINT c, const INT* lpDx)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_EXTTEXTOUT, decltype(&ExtTextOutW)>(callerMod);

        if (!orig)
            return ExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);

        if (g_treeSessionStack.empty())
            return orig(hdc, x, y, options, lprect, lpString, c, lpDx);

        DebugLog(DBG_NORMAL,
            L"[GlassCore][TreeView] ExtTextOutW: options=0x%X bkColor=0x%08X textColor=0x%08X "
            L"x=%d y=%d c=%u rect={%d,%d,%d,%d}",
            options, GetBkColor(hdc), GetTextColor(hdc),
            x, y, c,
            lprect ? lprect->left : -1, lprect ? lprect->top : -1,
            lprect ? lprect->right : -1, lprect ? lprect->bottom : -1);

        if (lprect)
        {
            PatBlt(hdc, lprect->left, lprect->top,
                lprect->right - lprect->left, lprect->bottom - lprect->top,
                BLACKNESS);

            if (options & ETO_OPAQUE)
            {
                COLORREF color = GetBkColor(hdc);
                if (color && color != GetSysColor(COLOR_WINDOW)
                    && color != GetSysColor(COLOR_BTNFACE))
                {
                    wil::unique_hbrush brush{ Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(color, 255) };
                    if (brush)
                        FillRect(hdc, lprect, brush.get());
                }
            }
        }

        if (options == ETO_GLYPH_INDEX || !c)
            return orig(hdc, x, y, options & ~ETO_OPAQUE, lprect, lpString, c, lpDx);

        if (options & ETO_GLYPH_INDEX)
            return orig(hdc, x, y, options, lprect, lpString, c, lpDx);

        RECT clip{};
        GetClipBox(hdc, &clip);
        clip.left = x;
        clip.top  = y;

        int result = 0;
        if (!FAILED(GlassCore::Text::DrawTextWithAlpha(
                hdc, lpString, static_cast<int>(c), &clip,
                DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOCLIP, result)))
            return TRUE;

        return orig(hdc, x, y, options, lprect, lpString, c, lpDx);
    }

    // ── FillRect ─────────────────────────────────────────────────────────────
    // Угол пересечения скроллов в TreeView может приходить как FillRect(COLOR_SCROLLBAR), минуя DTB(SIZEBOX).
    BOOL WINAPI Overall_MyFillRect(HDC hdc, const RECT* lprc, HBRUSH hbr)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_FILLRECT, decltype(&FillRect)>(callerMod);

        if (!orig)
            return FillRect(hdc, lprc, hbr);
        if (g_treeSessionStack.empty())
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
        }

        return orig(hdc, lprc, hbr);
    }

    int WINAPI Overall_MyDrawTextExW(
        HDC hdc, LPWSTR lpchText, int cchText,
        LPRECT lprc, UINT format, LPDRAWTEXTPARAMS lpdtp)
    {
        if (g_shutdown.load(std::memory_order_acquire))
            return DrawTextExW(hdc, lpchText, cchText, lprc, format, lpdtp);

        // Слот DrawTextExW патчим на базе uxtheme.dll; orig берём из диспетчера uxtheme
        // (_ReturnAddress() может быть не в uxtheme — не использовать только caller).
        HMODULE hUx = GetModuleHandleW(L"uxtheme.dll");
        auto orig = hUx ? GetOverallOrigSafe<OHK_DRAWTEXTEX, decltype(&DrawTextExW)>(hUx) : nullptr;
        if (!orig)
            return DrawTextExW(hdc, lpchText, cchText, lprc, format, lpdtp);
        if (g_treeSessionStack.empty())
            return orig(hdc, lpchText, cchText, lprc, format, lpdtp);

        if (lpdtp)
            return orig(hdc, lpchText, cchText, lprc, format, lpdtp);

        int result = 0;
        if (!FAILED(GlassCore::Text::DrawTextWithAlpha(hdc, lpchText, cchText, lprc, format, result)))
            return result;
        return orig(hdc, lpchText, cchText, lprc, format, lpdtp);
    }

    // ── MaskBlt ─────────────────────────────────────────────────────────────
    // TreeView использует MaskBlt для рисования иконок через ImageList.
    // ВАЖНО: если вызван MaskBlt с маской (hbmMask != NULL), значит ImageList
    // использует маску для прозрачности, а НЕ альфа-канал битмапа.
    // HasAlphaChannel может ложно вернуть true (мусорные байты в альфа-позиции
    // 32bpp strip-битмапа). Если бы альфа была настоящей, ImageList рисовал бы
    // через AlphaBlend, а не MaskBlt.
    // Поэтому при наличии маски — ВСЕГДА BlackKeyAlpha + GdiAlphaBlend.
    // Без маски + hasAlpha → GdiAlphaBlend напрямую (настоящая альфа).
    BOOL WINAPI Overall_MyMaskBlt(
        HDC hdcDest, int xDest, int yDest, int width, int height,
        HDC hdcSrc, int xSrc, int ySrc,
        HBITMAP hbmMask, int xMask, int yMask, DWORD rop)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_MASKBLT, decltype(&MaskBlt)>(callerMod);

        if (!orig)
            return MaskBlt(hdcDest, xDest, yDest, width, height,
                           hdcSrc, xSrc, ySrc, hbmMask, xMask, yMask, rop);

        if (g_treeSessionStack.empty())
            return orig(hdcDest, xDest, yDest, width, height,
                        hdcSrc, xSrc, ySrc, hbmMask, xMask, yMask, rop);

        if (!width || !height || !hdcSrc)
            return orig(hdcDest, xDest, yDest, width, height,
                        hdcSrc, xSrc, ySrc, hbmMask, xMask, yMask, rop);

        HBITMAP hSrcBitmap = static_cast<HBITMAP>(GetCurrentObject(hdcSrc, OBJ_BITMAP));
        if (!hSrcBitmap || Shared::BitmapUtils::IsOemBitmap(hSrcBitmap))
        {
            /*DebugLog(DBG_NORMAL, L"[GlassCore][TreeView] MaskBlt SKIP: hSrcBitmap=%p isOem=%d",
                hSrcBitmap, hSrcBitmap ? Shared::BitmapUtils::IsOemBitmap(hSrcBitmap) : -1);*/
            return orig(hdcDest, xDest, yDest, width, height,
                        hdcSrc, xSrc, ySrc, hbmMask, xMask, yMask, rop);
        }

        BITMAP bm{};
        GetObject(hSrcBitmap, sizeof(bm), &bm);
        bool hasAlpha = Shared::BitmapUtils::HasAlphaChannel(hSrcBitmap);

        /*DebugLog(DBG_IMPORTANT,
            L"[GlassCore][TreeView] MaskBlt: dst=(%d,%d %dx%d) src=(%d,%d) "
            L"bmp=%dx%d bpp=%d hasAlpha=%d mask=%p rop=0x%08X callerMod=%p",
            xDest, yDest, width, height, xSrc, ySrc,
            bm.bmWidth, bm.bmHeight, bm.bmBitsPixel, hasAlpha,
            hbmMask, rop, callerMod);*/

        // Если маска есть — альфа-канал в битмапе ненадёжен (мусор в 32bpp strip).
        // Без маски + hasAlpha → настоящая альфа, можно AlphaBlend напрямую.
        if (!hbmMask && hasAlpha)
        {
            //DebugLog(DBG_NORMAL, L"[GlassCore][TreeView] MaskBlt → GdiAlphaBlend (noMask + hasAlpha)");
            BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            return GdiAlphaBlend(hdcDest, xDest, yDest, width, height,
                                 hdcSrc, xSrc, ySrc, width, height, bf);
        }

        // Маска есть (или нет альфы) — black-key transparency.
        // Конвертируем в 32bpp, чёрные пиксели (фон ImageList) → alpha=0,
        // цветные пиксели (рисунок) → alpha=255. Затем AlphaBlend.
        wil::unique_hbitmap hConverted{ Shared::BitmapUtils::Promise32BPP(hSrcBitmap) };
        if (!hConverted)
        {
            //DebugLog(DBG_WARNING, L"[GlassCore][TreeView] MaskBlt: Promise32BPP FAILED, fallback to orig");
            return orig(hdcDest, xDest, yDest, width, height,
                        hdcSrc, xSrc, ySrc, hbmMask, xMask, yMask, rop);
        }

        GlassCore::GdiUtils::ApplyIconBackgroundAlpha(hConverted.get(), g_treeIconRemoval);

        /*DebugLog(DBG_NORMAL, L"[GlassCore][TreeView] MaskBlt → BlackKeyAlpha + GdiAlphaBlend (bpp=%d hasMask=%d)",
            bm.bmBitsPixel, !!hbmMask);*/

        HGDIOBJ hOldBitmap = SelectObject(hdcSrc, hConverted.get());
        auto restoreBitmap = wil::scope_exit([&] { SelectObject(hdcSrc, hOldBitmap); });

        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        return GdiAlphaBlend(hdcDest, xDest, yDest, width, height,
                             hdcSrc, xSrc, ySrc, width, height, bf);
    }

    // =========================================================================
    //  EnableTreeViewHooksForModule — включить/выключить хуки по callerModule
    // =========================================================================
    //
    // BWM PaintHandler.cpp TreeView:
    //   g_comctl32New:    ExtTextOutW, MaskBlt
    //                     + если IsThemed → DTB и ровно одна текстовая ветка:
    //                       Glow → DrawTextExW (uxtheme); иначе → DrawThemeTextEx (comctl32).
    //   g_comctl32Legacy: ExtTextOutW

    static void EnableTreeViewHooksForModule(
        HWND hWnd,
        HMODULE callerMod,
        bool enable)
    {
        auto* g_new    = GlassCore::ModuleHandler::g_comctl32New;
        auto* g_legacy = GlassCore::ModuleHandler::g_comctl32Legacy;

        // Вызывать может g_legacy, но рисует g_new
        if (g_new)
        {
            // Основной набор хуков на новом comctl32: ExtTextOutW + MaskBlt всегда
            EnableHooksOnModule(g_new, OHK_EXTTEXTOUT, enable);
            EnableHooksOnModule(g_new, OHK_FILLRECT,   enable);
            EnableHooksOnModule(g_new, OHK_MASKBLT,    enable);

            
        }
        if (callerMod == g_legacy)
        {
            // Legacy comctl32: ExtTextOutW + FillRect
            EnableHooksOnModule(g_legacy, OHK_EXTTEXTOUT, enable);
            EnableHooksOnModule(g_legacy, OHK_FILLRECT,   enable);
        }

        if (Shared::WinApiUtils::IsThemed(hWnd, L"TreeView"))
        {
            const bool glow = GlassCore::Text::IsInkGlowEffectActiveForPaintSession(hWnd);
            HMODULE hUx = GetModuleHandleW(L"uxtheme.dll");

            EnableHooksOnModule(g_new, OHK_DRAWTHEMEBG, enable);
            if (!glow || (!hUx && glow))
            {
                EnableHooksOnModule(callerMod, OHK_DRAWTHEMETEXTEX, enable);
            }
            else
                EnableHooksOnModule(hUx, OHK_DRAWTEXTEX, enable);
        }
    }

    // =========================================================================
    //  PaintDispatcher provider callbacks
    // =========================================================================

    static void OnBeginPaintGlobal(HWND hWnd, HMODULE callerMod)
    {
        using namespace GlassShared;
        if (GlassGetWindowKind(hWnd) != GlassControlKind::TreeView) return;

        // Гарантируем чёрный (прозрачный) фон перед BeginPaint/WM_ERASEBKGND.
        // TreeView_SetBkColor в TreeView_Attach может быть перезаписан shell-инициализацией.
        TreeView_SetBkColor(hWnd, 0);

        // IAT ScrollBar на comctl32 иначе может не включиться: GetDCEx часто с HWND ≠ SysTreeView32.
        GlassCore::ScrollBarTheme::EnsureComctlScrollBarIatIfComctlCaller(hWnd, callerMod);

        if (g_treeSessionStack.empty())
            GlassCore::Settings::QueryIconBackgroundColorRemovalContext(
                { L"Root", L"Control", L"TreeView" }, g_treeIconRemoval);

        g_treeSessionStack.push_back({ callerMod, hWnd });
        /*DebugLog(DBG_NORMAL, L"[GlassCore][TreeView] BeginPaint: callerMod=0x%p hWnd=0x%p depth=%zu",
            callerMod, hWnd, g_treeSessionStack.size());*/
        EnableTreeViewHooksForModule(hWnd, callerMod, true);
    }

    static void OnEndPaintGlobal(HWND hWnd, HMODULE /*callerMod*/)
    {
        using namespace GlassShared;
        if (GlassGetWindowKind(hWnd) != GlassControlKind::TreeView) return;
        if (g_treeSessionStack.empty()) return;

        const TreeViewPaintSession s = g_treeSessionStack.back();
        g_treeSessionStack.pop_back();
        EnableTreeViewHooksForModule(s.hWnd, s.mod, false);
        //DebugLog(DBG_NORMAL, L"[GlassCore][TreeView] EndPaint: hWnd=0x%p depth=%zu", hWnd, g_treeSessionStack.size());
    }

    // =========================================================================
    //  Lifecycle
    // =========================================================================

    void Prepare()  {}

    // Предпрогрев IAT-кеша (только чтение, без записей).
    // Холодный скан PE уходит из paint-кадра: в Startup (уже загруженные)
    // и в подписку ModuleHandler (поздняя подгрузка, под loader-lock безопасно).
    static void PrewarmModule(void* mod) noexcept
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
            DebugLog(DBG_NORMAL, L"[GlassCore][TreeView] Startup: cached 0x%p", mod);
        }
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
        if (_wcsicmp(base, L"comctl32.dll") != 0 && _wcsicmp(base, L"uxtheme.dll") != 0)
            return;
        PrewarmModule(mod);
    }

    void Startup()
    {
        GlassCore::PaintDispatcher::RegisterPaintProvider({ OnBeginPaintGlobal, OnEndPaintGlobal });

        PrewarmModule(GlassCore::ModuleHandler::g_comctl32New);
        PrewarmModule(GlassCore::ModuleHandler::g_comctl32Legacy);
        PrewarmModule(GlassCore::ModuleHandler::g_entryPointModule);
        PrewarmModule(GetModuleHandleW(L"uxtheme.dll"));
        GlassCore::ModuleHandler::RegisterModulePrewarmHandler(PrewarmForModule);

    }

    void Shutdown()
    {
        g_shutdown.store(true, std::memory_order_seq_cst);
        DisableAllOverallHooks();
        g_treeSessionStack.clear();
        g_shutdown.store(false, std::memory_order_seq_cst);
    }

    int MenuThemedTextDrawSessionDepth() noexcept
    {
        return g_menuThemedTextDrawDepth;
    }

    void MenuThemedTextDrawSessionPush() noexcept
    {
        ++g_menuThemedTextDrawDepth;
    }

    void MenuThemedTextDrawSessionPop() noexcept
    {
        if (g_menuThemedTextDrawDepth > 0)
            --g_menuThemedTextDrawDepth;
    }
}
