// Хуки для отрисовки ToolbarWindow32.
//
// Архитектура (PaintDispatcher + per-module g_overallMap):
//
// Toolbar: текст — DrawThemeText на comctl32;
//   glow — DrawTextExW на uxtheme.dll.
// Иконки — MaskBlt / BitBlt (gdi32 IAT в comctl32).
//   32bpp+alpha → AlphaBlend; иначе black-key + AlphaBlend.
//   1bpp-маска ImageList: skip после alpha_direct; orig после blackkey.
// Фон erase (F4): Root NM_CUSTOMDRAW CDDS_PREERASE → DrawThemeParentBackground.
// Хуки включаются на callerModule при каждом BeginPaint.

#include "CorePch.h"

#include <Hooks/Toolbar.h>
#include <Subsystems/RenderText.h>
#include <Core/Intercept/HookDispatcher.h>
#include <Module/ModuleHandler.h>
#include <Subsystems/PaintDispatcher.h>
#include <Subsystems/TextGlowClientRegistry.h>
#include <Subsystems/GdiHookUtils.h>
#include <Subsystems/Settings.h>
#include <GlassControlKind.h>
#include <Subsystems/RootSwcaGradient.h>
#include <BitmapUtils.h>
#include <wil.h>
#include <CommCtrl.h>
#include <uxtheme.h>
#include <WinApiUtils.h>
#include <vector>

#pragma comment(lib, "msimg32.lib")

using namespace GlassCore::Hooks;
using namespace std::literals;

namespace GlassCore::ToolbarControl
{
    // =========================================================================
    //  Thread-local: стек HMODULE (LIFO) — зеркало стека DC в PaintDispatcher.
    // Симметричный Enable/Disable на каждый Begin/End; глубину IAT ведёт HookDispatcher::hookRef.
    // Guard в телах хуков: !g_modStack.empty() — активная paint-сессия Toolbar (не путать с hookRef).
    // =========================================================================

    thread_local std::vector<HMODULE> g_modStack;

    // Удаление фона иконок Toolbar: Root\Control\Toolbar (presence color = chroma; иначе black-key).
    // Читаем один раз на внешнюю paint-сессию (g_modStack пуст), как ListViewHooks.
    thread_local WndCompat::Menu::IconBackgroundColorRemoval g_toolbarIconRemoval{};

    // После alpha_direct маска ImageList (1bpp) портит иконку — skip.
    // После blackkey (MMC MaskBlt) маска нужна как orig — не skip.
    thread_local bool g_toolbarSuppressMaskBitBlt{ false };

    static std::atomic<bool> g_shutdown{ false };

    // =========================================================================
    //  Helpers
    // =========================================================================

    static bool StrEqI(LPCWSTR a, LPCWSTR b) noexcept
    {
        return a && b && _wcsicmp(a, b) == 0;
    }

    [[nodiscard]] static bool CallerBaseIs(HMODULE caller, LPCWSTR base) noexcept
    {
        if (!caller || !base)
            return false;
        wchar_t path[MAX_PATH]{};
        if (!GetModuleFileNameW(caller, path, MAX_PATH))
            return false;
        const wchar_t* p = wcsrchr(path, L'\\');
        const wchar_t* name = p ? p + 1 : path;
        return StrEqI(name, base);
    }

    // PopupOnly: кастомный Toolbar только у явно помеченных ToolbarWindow32 (ViewControl popup).
    [[nodiscard]] static bool IsToolbarGlassSessionAllowed(HWND hWnd) noexcept
    {
        if (GlassCore::Root::IsFullInjectionMode())
            return true;
        return hWnd
            && GlassShared::GlassGetWindowKind(hWnd) == GlassShared::GlassControlKind::Toolbar;
    }

    [[nodiscard]] static bool IsToolbarGlassSessionAllowedForHdc(HDC hdc) noexcept
    {
        HWND hWnd = WindowFromDC(hdc);
        if (!hWnd)
            hWnd = GlassCore::PaintDispatcher::GetPaintSessionHostHwnd();
        return IsToolbarGlassSessionAllowed(hWnd);
    }

    static void EnsureToolbarTextColorOnDc(HDC hdc) noexcept
    {
        if (!hdc)
            return;
        if ((GetTextColor(hdc) & 0xFFFFFF) == 0)
            SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
    }

    // =========================================================================
    //  OVERALL DISPATCHER MAP
    // =========================================================================

    HRESULT  WINAPI Overall_MyDrawThemeTextEx(HTHEME, HDC, int, int, LPCWSTR, int, DWORD, LPRECT, const DTTOPTS*);
    HRESULT  WINAPI Overall_MyDrawThemeText(HTHEME, HDC, int, int, LPCWSTR, int, DWORD, DWORD, LPCRECT);
    int      WINAPI Overall_MyDrawTextExW(HDC, LPWSTR, int, LPRECT, UINT, LPDRAWTEXTPARAMS);
    BOOL     WINAPI Overall_MyMaskBlt(HDC, int, int, int, int, HDC, int, int, HBITMAP, int, int, DWORD);
    BOOL     WINAPI Overall_MyBitBlt(HDC, int, int, int, int, HDC, int, int, DWORD);
    HBRUSH   WINAPI Overall_MyCreateSolidBrush(COLORREF);

    enum OverallHookIndex : size_t
    {
        OHK_DRAWTHEMETEXTEX = 0,
        OHK_DRAWTHEMETEXT,
        OHK_MASKBLT,
        OHK_BITBLT,
        OHK_CREATESOLIDBRUSH,
        OHK_DRAWTEXTEX,
        OHK_COUNT
    };

    inline const HookDispatcherDependency<OHK_COUNT, 3> g_overallHookDep
    { {
        // OHK_DRAWTHEMETEXTEX
        std::tuple{ std::array{ "uxtheme.dll"sv, "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv, ""sv },
            "DrawThemeTextEx", reinterpret_cast<PVOID>(Overall_MyDrawThemeTextEx) },
        // OHK_DRAWTHEMETEXT
        std::tuple{ std::array{ "uxtheme.dll"sv, "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv, ""sv },
            "DrawThemeText", reinterpret_cast<PVOID>(Overall_MyDrawThemeText) },
        // OHK_MASKBLT
        std::tuple{ std::array{ "gdi32.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
            "MaskBlt", reinterpret_cast<PVOID>(Overall_MyMaskBlt) },
        // OHK_BITBLT
        std::tuple{ std::array{ "gdi32.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
            "BitBlt", reinterpret_cast<PVOID>(Overall_MyBitBlt) },
        // OHK_CREATESOLIDBRUSH (sysmon.ocx при сессии ToolbarWindow32 — свой диспетчер, IATRegistry цепь с ListView)
        std::tuple{ std::array{ "gdi32.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
            "CreateSolidBrush", reinterpret_cast<PVOID>(Overall_MyCreateSolidBrush) },
        // OHK_DRAWTEXTEX (glow: IAT comctl32 или uxtheme → user32!DrawTextExW)
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

    // ── DrawThemeTextEx ──────────────────────────────────────────────────────
    // BWM pattern: WinHooks.cpp MyDrawThemeTextEx.
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

        if (g_modStack.empty())
            return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions);

        if (!IsToolbarGlassSessionAllowedForHdc(hdc))
            return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions);

        if (!pRect)
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

        // Places-bar: не уводим в plain orig — альфа нужна и на сером буфере
        // (серый снимается на compose BitBlt).
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

    // ── DrawThemeText ────────────────────────────────────────────────────────
    // Старое API без DTTOPTS. Конвертируем вызов в DrawThemeTextEx с DTT_COMPOSITED
    // через DrawThemeContent для корректного alpha-канала.
    // BWM pattern: WinHooks.cpp MyDrawThemeText.
    HRESULT WINAPI Overall_MyDrawThemeText(
        HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
        LPCWSTR pszText, int cchText, DWORD dwTextFlags,
        DWORD dwTextFlags2, LPCRECT pRect)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_DRAWTHEMETEXT, decltype(&DrawThemeText)>(callerMod);

        if (!orig)
            return DrawThemeText(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, dwTextFlags2, pRect);

        if (g_modStack.empty())
            return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, dwTextFlags2, pRect);

        if (!IsToolbarGlassSessionAllowedForHdc(hdc))
            return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, dwTextFlags2, pRect);

        if (!pRect)
            return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, dwTextFlags2, pRect);

        if (dwTextFlags & DT_CALCRECT)
            return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, dwTextFlags2, pRect);

        EnsureToolbarTextColorOnDc(hdc);
        int result = 0;
        RECT rc = *pRect;
        const HRESULT hrAlpha = GlassCore::Text::DrawTextWithAlpha(
            hdc, pszText, cchText, &rc, dwTextFlags, result);
        if (!FAILED(hrAlpha))
            return S_OK;

        return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, dwTextFlags2, pRect);
    }

    // ── DrawTextExW (IAT comctl32 → user32): glow при активной сессии Toolbar
    int WINAPI Overall_MyDrawTextExW(
        HDC hdc, LPWSTR lpchText, int cchText,
        LPRECT lprc, UINT format, LPDRAWTEXTPARAMS lpdtp)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_DRAWTEXTEX, decltype(&DrawTextExW)>(callerMod);

        if (!orig)
            return DrawTextExW(hdc, lpchText, cchText, lprc, format, lpdtp);

        if (g_modStack.empty())
            return orig(hdc, lpchText, cchText, lprc, format, lpdtp);

        if (!IsToolbarGlassSessionAllowedForHdc(hdc))
            return orig(hdc, lpchText, cchText, lprc, format, lpdtp);

        if (lpdtp)
            return orig(hdc, lpchText, cchText, lprc, format, lpdtp);

        EnsureToolbarTextColorOnDc(hdc);
        int result = 0;
        if (!FAILED(GlassCore::Text::DrawTextWithAlpha(hdc, lpchText, cchText, lprc, format, result)))
            return result;
        return orig(hdc, lpchText, cchText, lprc, format, lpdtp);
    }

    // ── MaskBlt ─────────────────────────────────────────────────────────────
    // Toolbar использует MaskBlt для рисования иконок через ImageList.
    // Если MaskBlt вызван с маской — hasAlpha ненадёжен, всегда BlackKeyAlpha.
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

        if (g_modStack.empty())
            return orig(hdcDest, xDest, yDest, width, height,
                        hdcSrc, xSrc, ySrc, hbmMask, xMask, yMask, rop);

        if (!IsToolbarGlassSessionAllowedForHdc(hdcDest))
            return orig(hdcDest, xDest, yDest, width, height,
                        hdcSrc, xSrc, ySrc, hbmMask, xMask, yMask, rop);

        if (!width || !height || !hdcSrc)
            return orig(hdcDest, xDest, yDest, width, height,
                        hdcSrc, xSrc, ySrc, hbmMask, xMask, yMask, rop);

        HBITMAP hSrcBitmap = static_cast<HBITMAP>(GetCurrentObject(hdcSrc, OBJ_BITMAP));
        if (!hSrcBitmap || Shared::BitmapUtils::IsOemBitmap(hSrcBitmap))
        {
            return orig(hdcDest, xDest, yDest, width, height,
                        hdcSrc, xSrc, ySrc, hbmMask, xMask, yMask, rop);
        }

        BITMAP bm{};
        GetObject(hSrcBitmap, sizeof(bm), &bm);
        bool hasAlpha = Shared::BitmapUtils::HasAlphaChannel(hSrcBitmap);

        // 1bpp/4bpp — маска ImageList, не цветная иконка.
        if (bm.bmBitsPixel > 0 && bm.bmBitsPixel < 8)
        {
            return orig(hdcDest, xDest, yDest, width, height,
                        hdcSrc, xSrc, ySrc, hbmMask, xMask, yMask, rop);
        }

        // Если маска есть — альфа-канал в битмапе ненадёжен (мусор в 32bpp strip).
        // Без маски + hasAlpha → настоящая альфа, можно AlphaBlend напрямую.
        if (!hbmMask && hasAlpha)
        {
            g_toolbarSuppressMaskBitBlt = true;
            BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            return GdiAlphaBlend(hdcDest, xDest, yDest, width, height,
                                 hdcSrc, xSrc, ySrc, width, height, bf);
        }

        // Маска есть (или нет альфы) — black-key transparency.
        wil::unique_hbitmap hConverted{ Shared::BitmapUtils::Promise32BPP(hSrcBitmap) };
        if (!hConverted)
        {
            return orig(hdcDest, xDest, yDest, width, height,
                        hdcSrc, xSrc, ySrc, hbmMask, xMask, yMask, rop);
        }

        g_toolbarSuppressMaskBitBlt = false;
        GlassCore::GdiUtils::ApplyIconBackgroundAlpha(hConverted.get(), g_toolbarIconRemoval);
        HGDIOBJ hOldBitmap = SelectObject(hdcSrc, hConverted.get());
        auto restoreBitmap = wil::scope_exit([&] { SelectObject(hdcSrc, hOldBitmap); });

        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        return GdiAlphaBlend(hdcDest, xDest, yDest, width, height,
                             hdcSrc, xSrc, ySrc, width, height, bf);
    }

    // ── BitBlt ──────────────────────────────────────────────────────────────
    // comctl32: ImageList иконки (SRCCOPY) → AlphaBlend / BlackKeyAlpha.
    // 1bpp-маска после alpha_direct — skip; после blackkey (MMC) — orig.
    BOOL WINAPI Overall_MyBitBlt(
        HDC hdc, int x, int y, int cx, int cy,
        HDC hdcSrc, int x1, int y1, DWORD rop)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_BITBLT, decltype(&BitBlt)>(callerMod);

        if (!orig)
            return BitBlt(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);

        if (g_modStack.empty())
            return orig(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);

        if (!IsToolbarGlassSessionAllowedForHdc(hdc))
            return orig(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);

        if (!cx || !cy || !hdcSrc)
            return orig(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);

        HBITMAP hSrcBitmap = static_cast<HBITMAP>(GetCurrentObject(hdcSrc, OBJ_BITMAP));
        if (!hSrcBitmap || Shared::BitmapUtils::IsOemBitmap(hSrcBitmap))
            return orig(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);

        BITMAP bm{};
        GetObject(hSrcBitmap, sizeof(bm), &bm);

        // 1bpp/4bpp маска ImageList — любой ROP.
        // Skip только после alpha_direct (dialog); после blackkey (MMC) — orig.
        if (bm.bmBitsPixel > 0 && bm.bmBitsPixel < 8)
        {
            if (g_toolbarSuppressMaskBitBlt)
                return TRUE;
            return orig(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);
        }

        if (rop != SRCCOPY)
            return orig(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);

        bool hasAlpha = Shared::BitmapUtils::HasAlphaChannel(hSrcBitmap);

        if (hasAlpha)
        {
            g_toolbarSuppressMaskBitBlt = true;
            BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            return GdiAlphaBlend(hdc, x, y, cx, cy, hdcSrc, x1, y1, cx, cy, bf);
        }

        wil::unique_hbitmap hConverted{ Shared::BitmapUtils::Promise32BPP(hSrcBitmap) };
        if (!hConverted)
            return orig(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);

        g_toolbarSuppressMaskBitBlt = false;
        GlassCore::GdiUtils::ApplyIconBackgroundAlpha(hConverted.get(), g_toolbarIconRemoval);

        HGDIOBJ hOldBitmap = SelectObject(hdcSrc, hConverted.get());
        auto restoreBitmap = wil::scope_exit([&] { SelectObject(hdcSrc, hOldBitmap); });

        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        return GdiAlphaBlend(hdc, x, y, cx, cy, hdcSrc, x1, y1, cx, cy, bf);
    }

    // ── CreateSolidBrush (sysmon.ocx) ───────────────────────────────────────
    // Только сессия Toolbar + caller sysmon; иначе orig. Не использовать стек/хуки ListView.
    HBRUSH WINAPI Overall_MyCreateSolidBrush(COLORREF color)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_CREATESOLIDBRUSH, decltype(&CreateSolidBrush)>(callerMod);

        if (!orig)
            return CreateSolidBrush(color);

        if (g_modStack.empty())
            return orig(color);

        if (!IsToolbarGlassSessionAllowed(GlassCore::PaintDispatcher::GetPaintSessionHostHwnd()))
            return orig(color);

        if (CallerBaseIs(callerMod, L"sysmon.ocx") || CallerBaseIs(callerMod, L"wdc.dll"))
            if ((0xFFFFFF & color) == GetSysColor(COLOR_WINDOW) || (0xFFFFFF & color) == GetSysColor(COLOR_BTNFACE))
                color = RGB(0, 0, 0);

        if (color == 0)
            return orig(0);

        HBRUSH alphaBrush = Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(color, 255);
        return alphaBrush ? alphaBrush : orig(color);
    }

    // =========================================================================
    //  EnableToolbarHooksForModule — включить/выключить хуки по callerModule
    // =========================================================================
    //
    // g_comctl32New: DrawThemeText, MaskBlt, BitBlt (иконки);
    // uxtheme: glow DrawTextExW;
    // sysmon CreateSolidBrush.

    static void EnableToolbarHooksForModule(HWND hWnd, HMODULE callerMod, bool enable)
    {
        auto* g_new    = GlassCore::ModuleHandler::g_comctl32New;
        auto* g_legacy = GlassCore::ModuleHandler::g_comctl32Legacy;

        if (callerMod == g_new)
        {
            const bool glow = GlassCore::Text::IsInkGlowEffectActiveForPaintSession(hWnd);
            HMODULE hUx = GetModuleHandleW(L"uxtheme.dll");

            EnableHooksOnModule(callerMod, OHK_DRAWTHEMETEXT, enable);
            if (glow && hUx)
                EnableHooksOnModule(hUx, OHK_DRAWTEXTEX, enable);

            EnableHooksOnModule(g_new, OHK_MASKBLT, enable);
            EnableHooksOnModule(g_new, OHK_BITBLT, enable);
        }

        if (callerMod == g_new || callerMod == g_legacy)
        {
            if (PVOID sysmon = GetModuleHandleW(L"sysmon.ocx"))
                EnableHooksOnModule(sysmon, OHK_CREATESOLIDBRUSH, enable);
        }
    }

    // =========================================================================
    //  PaintDispatcher provider callbacks
    // =========================================================================
    //
    // Вложенные Begin/End: на каждый Begin — push + Enable(true), на каждый End — pop + Enable(false).
    // HookDispatcher::hookRef не даёт снять IAT, пока есть внешняя сессия.

    static void OnBeginGlobal(HWND hWnd, HMODULE callerMod)
    {
        using namespace GlassShared;
        if (GlassGetWindowKind(hWnd) != GlassControlKind::Toolbar)
            return;

        GlassCore::TextGlow::EnsureTextGlowClientRegistered(hWnd);

        if (g_modStack.empty())
            GlassCore::Settings::QueryIconBackgroundColorRemovalContext(
                { L"Root", L"Control", L"Toolbar" }, g_toolbarIconRemoval);

        g_modStack.push_back(callerMod);
        EnableToolbarHooksForModule(hWnd, callerMod, true);

        DebugLog(DBG_NORMAL, L"[GlassCore][Toolbar] Begin: callerMod=0x%p hWnd=0x%p depth=%zu",
            callerMod, hWnd, g_modStack.size());
    }

    static void OnEndGlobal(HWND hWnd, HMODULE /*callerMod*/)
    {
        using namespace GlassShared;
        if (GlassGetWindowKind(hWnd) != GlassControlKind::Toolbar) return;
        if (g_modStack.empty()) return;

        const HMODULE mod = g_modStack.back();
        g_modStack.pop_back();
        EnableToolbarHooksForModule(hWnd, mod, false);
        if (g_modStack.empty())
            g_toolbarSuppressMaskBitBlt = false;
        DebugLog(DBG_NORMAL, L"[GlassCore][Toolbar] End: hWnd=0x%p depth=%zu", hWnd, g_modStack.size());
    }

    // =========================================================================
    //  Lifecycle
    // =========================================================================

    void Prepare()  {}

    void Startup()
    {
        GlassCore::PaintDispatcher::RegisterPaintProvider({ OnBeginGlobal, OnEndGlobal });

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
                DebugLog(DBG_NORMAL, L"[GlassCore][Toolbar] Startup: cached 0x%p", mod);
            }
        };
        cacheModule(GlassCore::ModuleHandler::g_comctl32New);
        cacheModule(GlassCore::ModuleHandler::g_comctl32Legacy);
        cacheModule(GlassCore::ModuleHandler::g_entryPointModule);
        cacheModule(GetModuleHandleW(L"sysmon.ocx"));
        cacheModule(GetModuleHandleW(L"uxtheme.dll"));
    }

    void Shutdown()
    {
        g_shutdown.store(true, std::memory_order_seq_cst);
        DisableAllOverallHooks();
        g_modStack.clear();
        g_shutdown.store(false, std::memory_order_seq_cst);
    }
}
