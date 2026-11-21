// Хуки для отрисовки ReBarWindow32.
//
// Архитектура: прямое управление хуками из RebarSubclassProc (WndCompat).
//
// WM_PAINT в сабклассе вызывает OnPaintBegin/OnPaintEnd — они включают/
// выключают IAT DrawThemeBackground на comctl32; текст — StatusBar-паттерн:
// !glow → DrawThemeText (comctl32); glow → DrawTextExW (uxtheme).
// DTB «Rebar» RP_BAND/RP_BACKGROUND → PatBlt(BLACKNESS); gripper/chevron/splitter → orig.
//
// PaintDispatcher НЕ используется для bracket — GetPaintSessionHostHwnd() выставляется
// BeginPaint внутри DefSubclassProc.

#include "CorePch.h"

#include <Hooks/Rebar.h>
#include <Subsystems/RenderText.h>
#include <Subsystems/TextGlowClientRegistry.h>
#include <Subsystems/PaintDispatcher.h>
#include <Helpers/ThemeHelper.h>
#include <Core/Intercept/HookDispatcher.h>
#include <Module/ModuleHandler.h>
#include <GlassControlKind.h>
#include <GlassBackdropMark.h>
#include <Subsystems/RootSwcaGradient.h>
#include <wil.h>
#include <uxtheme.h>
#include <vector>
#include <array>
#include <cstring>

using namespace GlassCore::Hooks;
using namespace std::literals;

#pragma comment(lib, "msimg32.lib")

namespace GlassCore::RebarControl
{
    // =========================================================================
    //  LIFO paint-сессий (сабкласс Rebar, не PaintDispatcher).
    // =========================================================================

    enum class RebarTextHook
    {
        None,
        DrawThemeText,
        DrawTextExW,
    };

    struct RebarPaintSession
    {
        HWND          hwnd{};
        HMODULE       comctlMod{};
        RebarTextHook textHook{ RebarTextHook::None };
        bool          fillHooks{};
    };

    thread_local std::vector<RebarPaintSession> g_rebarSessionStack;

    static std::atomic<bool> g_shutdown{ false };

    // =========================================================================
    //  Helpers
    // =========================================================================

    static bool StrEq(LPCWSTR a, LPCWSTR b)
    {
        return _wcsicmp(a, b) == 0;
    }

    // vsstyle.h REBARPARTS — gripper/chevron/splitter не затираем PatBlt.
    [[nodiscard]] static bool IsRebarThemedChromePart(int iPartId) noexcept
    {
        switch (iPartId)
        {
        case 1: // RP_GRIPPER
        case 2: // RP_GRIPPERVERT
        case 4: // RP_CHEVRON
        case 5: // RP_CHEVRONVERT
        case 7: // RP_SPLITTER
        case 8: // RP_SPLITTERVERT
            return true;
        default:
            return false;
        }
    }

    [[nodiscard]] static bool IsRebarGlassTransparentFillPart(int iPartId) noexcept
    {
        switch (iPartId)
        {
        case 3: // RP_BAND
        case 6: // RP_BACKGROUND
            return true;
        default:
            return false;
        }
    }

    [[nodiscard]] static bool IsRebarGlassSessionAllowed(HWND hWnd) noexcept
    {
        if (!GlassCore::Root::IsFullInjectionMode())
            return false;
        if (!hWnd)
            hWnd = GlassCore::PaintDispatcher::GetPaintSessionHostHwnd();
        if (!hWnd)
            return false;
        if (GlassShared::GlassGetWindowKind(hWnd) != GlassShared::GlassControlKind::Rebar)
            return false;
        return GlassCore::TextGlow::IsInkGlowAllowedForWindow(hWnd);
    }

    [[nodiscard]] static bool IsRebarGlassSessionAllowedForHdc(HDC hdc) noexcept
    {
        HWND hWnd = GlassCore::PaintDispatcher::GetPaintSessionHostHwnd();
        if (!hWnd)
            hWnd = WindowFromDC(hdc);
        return IsRebarGlassSessionAllowed(hWnd);
    }

    static void EnsureRebarTextColorOnDc(HDC hdc) noexcept
    {
        if (!hdc)
            return;
        if ((GetTextColor(hdc) & 0xFFFFFF) == 0)
            SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
    }

    // =========================================================================
    //  Полоса Rebar с прозрачным фоном, но с рамками
    // =========================================================================

    static int ChannelDeltaRebar(BYTE a, BYTE b) noexcept
    {
        return a > b ? static_cast<int>(a) - static_cast<int>(b)
                     : static_cast<int>(b) - static_cast<int>(a);
    }

    // Полное чернение полосы (PatBlt BLACKNESS) убирало её нижнюю линию:
    // рамки рисует та же тема, что и заливку. Поэтому тема рисуется в 32bpp
    // DIB, заливка (цвет центра полосы) становится прозрачной, а рамки и
    // разделители остаются непрозрачными и попадают на экран через
    // GdiAlphaBlend.
    static bool DrawRebarBandWithAlpha(
        decltype(&DrawThemeBackground) orig, HTHEME hTheme, HDC hdc,
        int iPartId, int iStateId, const RECT& rect, const RECT* clip) noexcept
    {
        if (!orig || !hdc)
            return false;
        const int w = rect.right - rect.left;
        const int h = rect.bottom - rect.top;
        if (w <= 0 || h <= 0 || w > 8192 || h > 1024)
            return false;

        BITMAPINFO bi{};
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = w;
        bi.bmiHeader.biHeight      = -h;
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        wil::unique_hbitmap dib{ CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) };
        if (!dib || !bits)
            return false;
        wil::unique_hdc mem{ CreateCompatibleDC(hdc) };
        if (!mem)
            return false;
        const HGDIOBJ oldBmp = SelectObject(mem.get(), dib.get());
        if (!oldBmp || oldBmp == HGDI_ERROR)
            return false;
        auto restoreBmp = wil::scope_exit([&]() noexcept { (void)SelectObject(mem.get(), oldBmp); });

        std::memset(bits, 0, static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);

        RECT memRect{ 0, 0, w, h };
        RECT clipMem{};
        LPCRECT pClipMem = nullptr;
        RECT dst = rect;
        if (clip)
        {
            RECT inter{};
            if (!IntersectRect(&inter, &rect, clip))
                return true; // вне клипа: рисовать нечего, но и не ошибка
            clipMem = RECT{ inter.left - rect.left, inter.top - rect.top,
                inter.right - rect.left, inter.bottom - rect.top };
            pClipMem = &clipMem;
            dst = inter;
        }

        if (FAILED(orig(hTheme, mem.get(), iPartId, iStateId, &memRect, pClipMem)))
            return false;

        auto* pixels = static_cast<DWORD*>(bits);
        const size_t count = static_cast<size_t>(w) * static_cast<size_t>(h);

        // Заливка полосы = самый частый цвет (а не цвет центра: центр может
        // попасть на разделитель/текст, а заливка может быть не однотонной).
        // Квантование по 8 оттенков на канал, затем уточнение средним.
        std::array<unsigned, 32 * 32 * 32> histogram{};
        for (size_t i = 0; i < count; ++i)
        {
            const DWORD c = pixels[i] & 0xFFFFFFu;
            const size_t bin = static_cast<size_t>((c & 0xF8u) >> 3)
                | (static_cast<size_t>((c & 0xF800u) >> 8))
                | (static_cast<size_t>((c & 0xF80000u) >> 13));
            ++histogram[bin];
        }
        size_t bestBin = 0;
        unsigned bestCount = 0;
        for (size_t i = 0; i < histogram.size(); ++i)
        {
            if (histogram[i] > bestCount)
            {
                bestCount = histogram[i];
                bestBin = i;
            }
        }
        // Средний цвет пикселей выбранного бина — точнее центра бина.
        unsigned long long sumR = 0, sumG = 0, sumB = 0;
        for (size_t i = 0; i < count; ++i)
        {
            const DWORD c = pixels[i] & 0xFFFFFFu;
            const size_t bin = static_cast<size_t>((c & 0xF8u) >> 3)
                | (static_cast<size_t>((c & 0xF800u) >> 8))
                | (static_cast<size_t>((c & 0xF80000u) >> 13));
            if (bin != bestBin)
                continue;
            sumR += (c & 0xFFu);
            sumG += (c >> 8) & 0xFFu;
            sumB += (c >> 16) & 0xFFu;
        }
        if (!bestCount)
            return false;
        const COLORREF bg = RGB(
            static_cast<BYTE>(sumR / bestCount),
            static_cast<BYTE>(sumG / bestCount),
            static_cast<BYTE>(sumB / bestCount));

        // Наружу выпускаем только рамку полосы, всё остальное — прозрачно.
        // Так нижняя линия темы сохраняется, а заливка (какой бы она ни была)
        // на экран не попадает: непрозрачным становится лишь то, что реально
        // отличается от заливки и лежит в 2px от края.
        constexpr int kRing = 2;
        constexpr int kThreshold = 8;
        size_t kept = 0;
        size_t keyed = 0;
        for (int y = 0; y < h; ++y)
        {
            DWORD* row = pixels + static_cast<size_t>(y) * static_cast<size_t>(w);
            for (int x = 0; x < w; ++x)
            {
                const COLORREF c = static_cast<COLORREF>(row[x] & 0xFFFFFFu);
                const bool nearFill =
                    ChannelDeltaRebar(GetRValue(c), GetRValue(bg)) <= kThreshold
                    && ChannelDeltaRebar(GetGValue(c), GetGValue(bg)) <= kThreshold
                    && ChannelDeltaRebar(GetBValue(c), GetBValue(bg)) <= kThreshold;
                const bool ring = x < kRing || y < kRing || x >= w - kRing || y >= h - kRing;
                if (ring && !nearFill)
                {
                    row[x] |= 0xFF000000u;
                    ++kept;
                }
                else
                {
                    row[x] = 0;
                    ++keyed;
                }
            }
        }
        // Рамки нет (тема заливает полосу целиком) — пусть вызывающий сделает
        // BLACKNESS: прозрачно и без белой рамки по краю.
        if (!kept || kept * 50 < count)
        {
            DebugLog(DBG_NORMAL,
                L"[GlassCore][Rebar] band edges-reject bg=0x%08X kept=%zu of %zu",
                static_cast<unsigned>(bg), kept, count);
            return false;
        }

        DebugLog(DBG_NORMAL,
            L"[GlassCore][Rebar] band edges bg=0x%08X kept=%zu keyed=%zu of %zu",
            static_cast<unsigned>(bg), kept, keyed, count);

        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        return GdiAlphaBlend(hdc, dst.left, dst.top, dst.right - dst.left, dst.bottom - dst.top,
            mem.get(), dst.left - rect.left, dst.top - rect.top,
            dst.right - dst.left, dst.bottom - dst.top, bf) != FALSE;
    }

    // =========================================================================
    //  OVERALL DISPATCHER MAP
    // =========================================================================

    int WINAPI Overall_MyDrawThemeBg(HTHEME, HDC, int, int, LPCRECT, LPCRECT);
    HRESULT WINAPI Overall_MyDrawThemeText(HTHEME, HDC, int, int, LPCWSTR, int, DWORD, DWORD, LPCRECT);
    int WINAPI Overall_MyDrawTextExW(HDC, LPWSTR, int, LPRECT, UINT, LPDRAWTEXTPARAMS);
    int WINAPI Overall_MyFillRect(HDC, const RECT*, HBRUSH);
    BOOL WINAPI Overall_MyPatBlt(HDC, int, int, int, int, DWORD);
    int WINAPI Overall_MyExtTextOutW(HDC, int, int, UINT, const RECT*, LPCWSTR, UINT, const INT*);

    enum OverallHookIndex : size_t
    {
        OHK_DRAWTHEMEBG = 0,
        OHK_DRAWTHEMETEXT,
        OHK_DRAWTEXTEX,
        OHK_FILLRECT,
        OHK_PATBLT,
        OHK_EXTTEXTOUT,
        OHK_COUNT
    };

    inline const HookDispatcherDependency<OHK_COUNT, 3> g_overallHookDep
    { {
        // OHK_DRAWTHEMEBG
        std::tuple{ std::array{ "uxtheme.dll"sv, "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv, ""sv },
            "DrawThemeBackground", reinterpret_cast<PVOID>(Overall_MyDrawThemeBg) },
        // OHK_DRAWTHEMETEXT (!glow)
        std::tuple{ std::array{ "uxtheme.dll"sv, "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv, ""sv },
            "DrawThemeText", reinterpret_cast<PVOID>(Overall_MyDrawThemeText) },
        // OHK_DRAWTEXTEX (glow: uxtheme → user32!DrawTextExW)
        std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
            "DrawTextExW", reinterpret_cast<PVOID>(Overall_MyDrawTextExW) },
        // OHK_FILLRECT: заливка полосы системным цветом (белый/face) → чёрный.
        std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
            "FillRect", reinterpret_cast<PVOID>(Overall_MyFillRect) },
        // OHK_PATBLT: WHITENESS → BLACKNESS в той же отрисовке.
        std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv },
            "PatBlt", reinterpret_cast<PVOID>(Overall_MyPatBlt) },
        // OHK_EXTTEXTOUT: заливка полосы темой — ExtTextOutW(ETO_OPAQUE, ""),
        // цвет берётся из SetBkColor. Это и есть белая полоса.
        std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv },
            "ExtTextOutW", reinterpret_cast<PVOID>(Overall_MyExtTextOutW) },
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

    int WINAPI Overall_MyDrawThemeBg(
        HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
        LPCRECT pRect, LPCRECT pClipRect)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_DRAWTHEMEBG, decltype(&DrawThemeBackground)>(callerMod);

        if (!orig)
            return DrawThemeBackground(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

        if (g_rebarSessionStack.empty())
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

        if (!GlassCore::Root::IsFullInjectionMode())
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

        const HWND hWnd = WindowFromDC(hdc);
        if (hWnd && !GlassShared::GlassIsBackdropAllowedForHwnd(hWnd))
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

        WCHAR themeClass[MAX_PATH + 1]{};
        if (SUCCEEDED(Shared::ThemeHelper::GetThemeClass(hTheme, themeClass, MAX_PATH))
            && StrEq(themeClass, L"Rebar")
            && pRect && !IsRectEmpty(pRect))
        {
            if (IsRebarThemedChromePart(iPartId))
                return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

            if (IsRebarGlassTransparentFillPart(iPartId))
            {
                // Сначала пробуем оставить рамки полосы (нижняя линия), вырезав
                // только заливку; полное чернение — запасной вариант.
                if (DrawRebarBandWithAlpha(orig, hTheme, hdc, iPartId, iStateId, *pRect, pClipRect))
                {
                    DebugLog(DBG_NORMAL,
                        L"[GlassCore][Rebar] band part=%d state=%d rect=[%ld,%ld,%ld,%ld] alpha-edges",
                        iPartId, iStateId, pRect->left, pRect->top, pRect->right, pRect->bottom);
                    return S_OK;
                }
                const RECT* clipRect = pClipRect ? pClipRect : pRect;
                PatBlt(hdc,
                    clipRect->left, clipRect->top,
                    clipRect->right  - clipRect->left,
                    clipRect->bottom - clipRect->top,
                    BLACKNESS);
                DebugLog(DBG_NORMAL,
                    L"[GlassCore][Rebar] band part=%d state=%d rect=[%ld,%ld,%ld,%ld] black",
                    iPartId, iStateId, pRect->left, pRect->top, pRect->right, pRect->bottom);
                return S_OK;
            }
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

        if (g_rebarSessionStack.empty())
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

    int WINAPI Overall_MyDrawTextExW(
        HDC hdc, LPWSTR lpchText, int cchText,
        LPRECT lprc, UINT format, LPDRAWTEXTPARAMS lpdtp)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_DRAWTEXTEX, decltype(&DrawTextExW)>(callerMod);

        if (!orig)
            return DrawTextExW(hdc, lpchText, cchText, lprc, format, lpdtp);

        if (g_rebarSessionStack.empty())
            return orig(hdc, lpchText, cchText, lprc, format, lpdtp);

        if (!IsRebarGlassSessionAllowedForHdc(hdc))
            return orig(hdc, lpchText, cchText, lprc, format, lpdtp);

        if (lpdtp)
            return orig(hdc, lpchText, cchText, lprc, format, lpdtp);

        EnsureRebarTextColorOnDc(hdc);
        int result = 0;
        if (!FAILED(GlassCore::Text::DrawTextWithAlpha(hdc, lpchText, cchText, lprc, format, result)))
            return result;
        return orig(hdc, lpchText, cchText, lprc, format, lpdtp);
    }

    // =========================================================================
    //  Заливки полосы: белый/face фон → прозрачность (BLACKNESS)
    // =========================================================================

    static thread_local bool g_inRebarFill = false;

    // DC принадлежит окну текущей сессии: иначе это чужая отрисовка.
    static bool IsRebarPaintDc(HDC hdc) noexcept
    {
        if (!hdc || g_rebarSessionStack.empty())
            return false;
        const HWND sessionHwnd = g_rebarSessionStack.back().hwnd;
        if (!sessionHwnd || !IsWindow(sessionHwnd))
            return false;
        return WindowFromDC(hdc) == sessionHwnd;
    }

    static bool IsRebarBackgroundFill(HDC hdc, HBRUSH brush) noexcept
    {
        if (!brush || !IsRebarPaintDc(hdc))
            return false;
        LOGBRUSH lb{};
        if (GetObjectW(brush, sizeof(lb), &lb) != sizeof(lb) || lb.lbStyle != BS_SOLID)
            return false;
        const COLORREF color = lb.lbColor & 0xFFFFFF;
        return color == (GetSysColor(COLOR_WINDOW) & 0xFFFFFF)
            || color == (GetSysColor(COLOR_BTNFACE) & 0xFFFFFF)
            || color == (GetSysColor(COLOR_3DFACE) & 0xFFFFFF)
            || color == RGB(255, 255, 255)
            || color == RGB(0, 0, 0);
    }

    int WINAPI Overall_MyFillRect(HDC hdc, const RECT* rc, HBRUSH brush)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_FILLRECT, decltype(&FillRect)>(callerMod);
        if (!orig)
            return FillRect(hdc, rc, brush);
        if (g_inRebarFill || !rc || !IsRebarBackgroundFill(hdc, brush))
            return orig(hdc, rc, brush);

        // Фон полосы/рамки: тот же прямоугольник, но в прозрачность.
        g_inRebarFill = true;
        const BOOL ok = PatBlt(hdc, rc->left, rc->top,
            rc->right - rc->left, rc->bottom - rc->top, BLACKNESS);
        g_inRebarFill = false;
        DebugLog(DBG_NORMAL,
            L"[GlassCore][Rebar] fill hdc=0x%p rect=[%ld,%ld,%ld,%ld] -> BLACKNESS ok=%d",
            hdc, rc->left, rc->top, rc->right, rc->bottom, ok ? 1 : 0);
        return ok ? TRUE : 0;
    }

    BOOL WINAPI Overall_MyPatBlt(HDC hdc, int x, int y, int cx, int cy, DWORD rop)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_PATBLT, decltype(&PatBlt)>(callerMod);
        if (!orig)
            return PatBlt(hdc, x, y, cx, cy, rop);
        if (g_inRebarFill || rop != WHITENESS || !IsRebarPaintDc(hdc))
            return orig(hdc, x, y, cx, cy, rop);

        g_inRebarFill = true;
        const BOOL ok = orig(hdc, x, y, cx, cy, BLACKNESS);
        g_inRebarFill = false;
        DebugLog(DBG_NORMAL,
            L"[GlassCore][Rebar] patblt hdc=0x%p (%d,%d) %dx%d WHITENESS -> BLACKNESS ok=%d",
            hdc, x, y, cx, cy, ok ? 1 : 0);
        return ok;
    }

    // Тема рисует фон полосы пустым ExtTextOutW с ETO_OPAQUE: цвет берётся из
    // SetBkColor, глифов нет. Раньше такой вызов проходил мимо всех хуков и
    // оставлял непрозрачную светлую полосу.
    int WINAPI Overall_MyExtTextOutW(
        HDC hdc, int x, int y, UINT options, const RECT* rc,
        LPCWSTR text, UINT cch, const INT* dx)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_EXTTEXTOUT, decltype(&ExtTextOutW)>(callerMod);
        if (!orig)
            return ExtTextOutW(hdc, x, y, options, rc, text, cch, dx);
        if (g_inRebarFill || !rc || (options & ETO_OPAQUE) == 0 || cch != 0
            || !IsRebarPaintDc(hdc))
            return orig(hdc, x, y, options, rc, text, cch, dx);

        g_inRebarFill = true;
        const BOOL ok = PatBlt(hdc, rc->left, rc->top,
            rc->right - rc->left, rc->bottom - rc->top, BLACKNESS);
        g_inRebarFill = false;
        DebugLog(DBG_NORMAL,
            L"[GlassCore][Rebar] band-fill hdc=0x%p rect=[%ld,%ld,%ld,%ld] bk=0x%08X -> BLACKNESS ok=%d",
            hdc, rc->left, rc->top, rc->right, rc->bottom,
            static_cast<unsigned>(GetBkColor(hdc)), ok ? 1 : 0);
        return ok ? TRUE : 0;
    }

    // =========================================================================
    //  Публичные функции — прямое управление хуками из WndCompat
    // =========================================================================

    void OnPaintBegin(HWND hWnd)
    {
        if (g_shutdown.load(std::memory_order_acquire)) return;
        if (!GlassCore::Root::IsFullInjectionMode()) return;
        if (!hWnd || !IsWindow(hWnd)) return;

        GlassCore::TextGlow::EnsureTextGlowClientRegistered(hWnd);

        HMODULE callerMod = GlassCore::ModuleHandler::g_comctl32New;
        if (!callerMod)
            callerMod = GlassCore::ModuleHandler::g_comctl32Legacy;
        if (!callerMod) return;

        const bool glow = GlassCore::Text::IsInkGlowEffectActiveForPaintSession(hWnd);
        HMODULE hUx = GetModuleHandleW(L"uxtheme.dll");

        RebarPaintSession session{ hWnd, callerMod, RebarTextHook::None, false };
        g_rebarSessionStack.push_back(session);

        EnableHooksOnModule(callerMod, OHK_DRAWTHEMEBG, true);

        // Заливку полосы может делать и comctl32, и сама VCL (EXE) — белый или
        // face-цвет; тогда красим её в прозрачность, иначе полоса остаётся
        // белой (см. image-46).
        const bool fillHooks =
            EnableHooksOnModule(callerMod, OHK_FILLRECT, true)
            && EnableHooksOnModule(GlassCore::ModuleHandler::g_entryPointModule, OHK_FILLRECT, true);
        if (EnableHooksOnModule(callerMod, OHK_PATBLT, true))
            EnableHooksOnModule(GlassCore::ModuleHandler::g_entryPointModule, OHK_PATBLT, true);
        // Фон полосы рисует uxtheme, поэтому ExtTextOutW вооружается и там.
        if (EnableHooksOnModule(callerMod, OHK_EXTTEXTOUT, true))
        {
            EnableHooksOnModule(GlassCore::ModuleHandler::g_entryPointModule, OHK_EXTTEXTOUT, true);
            if (HMODULE hUx = GetModuleHandleW(L"uxtheme.dll"))
                EnableHooksOnModule(hUx, OHK_EXTTEXTOUT, true);
        }
        g_rebarSessionStack.back().fillHooks = fillHooks;

        if (!glow || !hUx)
        {
            EnableHooksOnModule(callerMod, OHK_DRAWTHEMETEXT, true);
            g_rebarSessionStack.back().textHook = RebarTextHook::DrawThemeText;
        }
        else
        {
            EnableHooksOnModule(hUx, OHK_DRAWTEXTEX, true);
            g_rebarSessionStack.back().textHook = RebarTextHook::DrawTextExW;
        }

        DebugLog(DBG_NORMAL, L"[GlassCore][Rebar] OnPaintBegin: hWnd=0x%p callerMod=0x%p textHook=%d fill=%d depth=%zu",
            hWnd, callerMod, static_cast<int>(g_rebarSessionStack.back().textHook),
            g_rebarSessionStack.back().fillHooks ? 1 : 0, g_rebarSessionStack.size());
    }

    void OnPaintEnd()
    {
        if (!GlassCore::Root::IsFullInjectionMode()) return;
        if (g_rebarSessionStack.empty()) return;

        const RebarPaintSession session = g_rebarSessionStack.back();
        g_rebarSessionStack.pop_back();

        EnableHooksOnModule(session.comctlMod, OHK_DRAWTHEMEBG, false);

        if (session.fillHooks)
        {
            EnableHooksOnModule(session.comctlMod, OHK_FILLRECT, false);
            EnableHooksOnModule(GlassCore::ModuleHandler::g_entryPointModule, OHK_FILLRECT, false);
            EnableHooksOnModule(session.comctlMod, OHK_PATBLT, false);
            EnableHooksOnModule(GlassCore::ModuleHandler::g_entryPointModule, OHK_PATBLT, false);
            EnableHooksOnModule(session.comctlMod, OHK_EXTTEXTOUT, false);
            EnableHooksOnModule(GlassCore::ModuleHandler::g_entryPointModule, OHK_EXTTEXTOUT, false);
            if (HMODULE hUx = GetModuleHandleW(L"uxtheme.dll"))
                EnableHooksOnModule(hUx, OHK_EXTTEXTOUT, false);
        }

        switch (session.textHook)
        {
        case RebarTextHook::DrawThemeText:
            EnableHooksOnModule(session.comctlMod, OHK_DRAWTHEMETEXT, false);
            break;
        case RebarTextHook::DrawTextExW:
            if (HMODULE hUx = GetModuleHandleW(L"uxtheme.dll"))
                EnableHooksOnModule(hUx, OHK_DRAWTEXTEX, false);
            break;
        case RebarTextHook::None:
            break;
        }

        DebugLog(DBG_NORMAL, L"[GlassCore][Rebar] OnPaintEnd: callerMod=0x%p depth=%zu",
            session.comctlMod, g_rebarSessionStack.size());
    }

    void Prepare()  {}

    void Startup()
    {
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
                DebugLog(DBG_NORMAL, L"[GlassCore][Rebar] Startup: cached 0x%p", mod);
            }
        };
        cacheModule(GlassCore::ModuleHandler::g_comctl32New);
        cacheModule(GlassCore::ModuleHandler::g_comctl32Legacy);
        cacheModule(GlassCore::ModuleHandler::g_entryPointModule);
        cacheModule(GetModuleHandleW(L"uxtheme.dll"));
    }

    void Shutdown()
    {
        g_shutdown.store(true, std::memory_order_seq_cst);
        DisableAllOverallHooks();
        g_rebarSessionStack.clear();
        g_shutdown.store(false, std::memory_order_seq_cst);
    }
}
