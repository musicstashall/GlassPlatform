// Хуки для ComboLBox / ComboBox — полная реализация по образцу BWM.
//
// Архитектура (единое 3D поле — g_overallMap):
//
// Все перехваты идут через одну систему — g_overallMap (per-module HookDispatcher).
// g_comboDispatcher (2D) полностью удалён — он конкурировал с g_overallMap за
// одни и те же IAT-слоты comctl32, второй патч перетирал первый.
//
// Хуки на comctl32.dll включаются через EnableHooksOnModule на ОБА экземпляра
// (g_comctl32Module + g_comctl32ModuleLegacy: WinSxS v6 + legacy, см. EnsureComctl32Ready).
// Хуки на COMDLG32, MFC и пр. — через EnableOverallModuleHooks().
//
// DrawThemeBackground: тема ScrollBar — отдельная ветка (полоса/стекло). Тема Combobox на ComboLBox —
// только оригинальный DrawThemeBackground (без DrawThemeBackgroundWithConstantAlpha / GdiAlphaBlend):
// список дропдауна не входит в сценарий GlassDrawThemeConstantAlpha для кнопок/вкладок/комбобокса на клиенте.
// Исключение: ComboBoxEx32 «пирог» msctls_progress32 — только orig.
//
 // ExtTextOutW: список ComboLBox рисуется из comctl32 через GDI32 — в тесте с DebugDetour виден вызов
 // ExtTextOutW, DrawThemeTextEx при этом не вызывается (нет IAT uxtheme в comctl32 на Win11). IAT на
 // ExtTextOutW ставим на ОБА известных экземпляра comctl32 через EnableComboHooks —
 // callerMod из PaintDispatcher (модуль, вызвавший BeginPaint/GetDC/CreateCompatibleDC)
 // при этом всегда покрыт (см. ForEachKnownComctlModule).
//
// AnimateWindow: comctl32.dll не импортирует user32!AnimateWindow в IAT (вызов не через слот импорта),
// поэтому перехват — Detour на user32 (см. EnsureUser32AnimateDetour), а не EnableHooksOnModule(comctl32).
//

#include "CorePch.h"

#include <Hooks/ComboLBox.h>
#include <Hooks/ScrollBarDrawTheme.h>
#include <Hooks/ScrollBarTheme.h>
#include <Subsystems/PaintDispatcher.h>
#include <Subsystems/RenderText.h>
#include <Helpers/ThemeHelper.h>
#include <Core/Intercept/HookDispatcher.h>
#include <Module/ModuleHandler.h>
#include <GlassControlKind.h>
#include <DropDownContext.h>
#include <wil.h>
#include <uxtheme.h>
#include <Subsystems/Settings.h>
#include <Subsystems/Animation.h>
#include <Subsystems/FlyoutFrame.h>
#include <Core/Utils.h>
#include <WinApiUtils.h>
#include <Hooks/ComboAddressPieSkip.h>
#include <Core/Intercept/HookHelper.h>
#include <Core/Intercept/IATRegistry.h>
#include <Hooks/RootGdiHooks.h>
#if defined(_DEBUG)
#include <Core/Intercept/DebugDetourHooks.h>
#endif

#include <unordered_map>
#include <algorithm>
#include "../../../WndCompat/Tracker/WindowClassUtils.h"

using namespace GlassCore::Hooks;
using namespace std::literals;

namespace GlassCore::ComboLBox
{
    // =========================================================================
    //  Thread-local guards
    // =========================================================================

    // Рефкаунты активных сессий в данном GUI-потоке.
    // Инкрементируются в PaintDispatcher-провайдерах Begin, декрементируются в End.
    // Служат guard-флагами в hook-функциях (IsActive): чужой поток видит 0.
    thread_local int  g_paintRef  = 0;  // BeginPaint / EndPaint
    thread_local int  g_hotRef    = 0;  // GetDC / ReleaseDC
    thread_local int  g_scrollRef = 0;  // CreateCompatibleDC / DeleteDC

    // Модули, для которых в данный момент включены IAT-хуки (один per-тип).
    // Защита от двойного Enable если PaintDispatcher вызовет onBegin дважды.
    thread_local HMODULE g_activePaintMod  = nullptr;
    thread_local HMODULE g_activeHotMod    = nullptr;
    thread_local HMODULE g_activeScrollMod = nullptr;

    static std::atomic<bool> g_shutdown{ false };
    HWND h_combolbox = nullptr;

    // Базы comctl32.dll для IAT: в mmc живут два экземпляра (WinSxS v6 + legacy).
    // Один g_comctl32Module покрывал только первый попавшийся (предпочтение New) —
    // список через legacy-экземпляр рисовался мимо хуков. Держим оба слота явно:
    // g_comctl32Module — v6 (или первый известный), g_comctl32ModuleLegacy — второй.
    // GetModuleHandleW(L"comctl32.dll") здесь запрещён: возвращает один, неизвестно какой.
    static void* g_comctl32Module{ nullptr };
    static void* g_comctl32ModuleLegacy{ nullptr };

    // Перебор известных экземпляров comctl32 (дедупликация на случай совпадения).
    template <typename Fn>
    static void ForEachKnownComctlModule(Fn&& fn) noexcept
    {
        void* n = g_comctl32Module;
        void* l = g_comctl32ModuleLegacy;
        if (n) fn(n);
        if (l && l != n) fn(l);
    }

    // user32!AnimateWindow — глобальный Detour (comctl32 не имеет IAT на AnimateWindow).
    static decltype(&::AnimateWindow) g_trueUser32AnimateWindow = ::AnimateWindow;
    static std::atomic<bool>         g_comboAnimateArmed{ false };
    static bool                      g_user32AnimateDetourInstalled{ false };
    static BOOL WINAPI Detour_User32AnimateWindow(HWND, DWORD, DWORD);

    static inline bool IsActive() noexcept
    {
        return g_paintRef > 0 || g_hotRef > 0 || g_scrollRef > 0;
    }

    // =========================================================================
    //  Helpers
    // =========================================================================

    static bool StrEq(LPCWSTR a, LPCWSTR b)
    {
        return _wcsicmp(a, b) == 0;
    }

    bool IsModuleName(PVOID hModule, std::wstring_view moduleName)
    {
        WCHAR szModuleName[MAX_PATH];
        GetModuleFileName((HMODULE)hModule, szModuleName, MAX_PATH);

        return (!_wcsicmp(PathFindFileName(szModuleName), moduleName.data()));
    }

    static const uint8_t kHatchPatterns[6][8] =
    {
        { 0x00,0x00,0x00,0xFF,0x00,0x00,0x00,0x00 }, // HS_HORIZONTAL
        { 0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08 }, // HS_VERTICAL
        { 0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80 }, // HS_FDIAGONAL
        { 0x80,0x40,0x20,0x10,0x08,0x04,0x02,0x01 }, // HS_BDIAGONAL
        { 0x08,0x08,0x08,0xFF,0x08,0x08,0x08,0x08 }, // HS_CROSS
        { 0x81,0x42,0x24,0x18,0x18,0x24,0x42,0x81 }, // HS_DIAGCROSS
    };



    // =========================================================================
    //  OVERALL DISPATCHER MAP — единственная система перехватов
    // =========================================================================

    // Forward declarations
    int      WINAPI Overall_MyDrawTextW(HDC, LPCWSTR, int, LPRECT, UINT);
    int      WINAPI Overall_MyDrawTextExW(HDC, LPWSTR, int, LPRECT, UINT, LPDRAWTEXTPARAMS);
    BOOL     WINAPI Overall_MyExtTextOutW(HDC, int, int, UINT, const RECT*, LPCWSTR, UINT, const INT*);
    BOOL     WINAPI Overall_MyTextOutW(HDC, int, int, LPCWSTR, int);
    COLORREF WINAPI Overall_MySetBkColor(HDC, COLORREF);
    HBRUSH   WINAPI Overall_MyCreateSolidBrush(COLORREF);
    HGDIOBJ  WINAPI Overall_MyGetStockObject(int);
    HGDIOBJ  WINAPI Overall_MySelectObject(HDC, HGDIOBJ);
    BOOL     WINAPI Overall_MyBitBlt(HDC, int, int, int, int, HDC, int, int, DWORD);
    HRESULT  WINAPI Overall_MyDrawThemeBg(HTHEME, HDC, int, int, LPCRECT, LPCRECT);
    HRESULT  WINAPI Overall_MyDrawThemeTextEx(
        HTHEME, HDC, int, int, LPCWSTR, int, DWORD, LPRECT, const DTTOPTS*);
    BOOL WINAPI Overall_MyAnimateWindow(HWND, DWORD, DWORD);
    HBRUSH WINAPI Overall_MyCreateHatchBrush(int fnStyle, COLORREF color);
    BOOL WINAPI Overall_MyFillRect(HDC, const RECT*, HBRUSH);
    BOOL WINAPI Overall_MyPatBlt(HDC, int, int, int, int, DWORD);
    HBRUSH WINAPI Overall_MyGetSysColorBrush(int);

    enum OverallHookIndex : size_t
    {
        OHK_DRAWTEXT = 0,
        OHK_DRAWTEXTEX,
        OHK_EXTTEXTOUT,
        OHK_TEXTOUT,
        OHK_SETBKCOLOR,
        OHK_CREATESOLIDBRUSH,
        OHK_GETSTOCKOBJECT,
        OHK_SELECTOBJECT,
        OHK_BITBLT,
        OHK_DRAWTHEMEBG,
        OHK_DRAWTHEMETEXTEX,
        OHK_ANIMATEWINDOW,
        OHK_CREATEHATCHBRUSH,
        OHK_FILLRECT,
        OHK_PATBLT,
        OHK_GETSYSCOLORBRUSH,
        OHK_COUNT
    };

    inline const HookDispatcherDependency<OHK_COUNT, 3> g_overallHookDep
    { {
            // OHK_DRAWTEXT
            std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
                "DrawTextW", reinterpret_cast<PVOID>(Overall_MyDrawTextW) },
                // OHK_DRAWTEXTEX
                std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
                    "DrawTextExW", reinterpret_cast<PVOID>(Overall_MyDrawTextExW) },
                    // OHK_EXTTEXTOUT
                    std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv },
                        "ExtTextOutW", reinterpret_cast<PVOID>(Overall_MyExtTextOutW) },
                        // OHK_TEXTOUT
                        std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv },
                            "TextOutW", reinterpret_cast<PVOID>(Overall_MyTextOutW) },
                            // OHK_SETBKCOLOR
                            std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv },
                                "SetBkColor", reinterpret_cast<PVOID>(Overall_MySetBkColor) },
                                // OHK_CREATESOLIDBRUSH
                                std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv },
                                    "CreateSolidBrush", reinterpret_cast<PVOID>(Overall_MyCreateSolidBrush) },
                                    // OHK_GETSTOCKOBJECT
                                    std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv },
                                        "GetStockObject", reinterpret_cast<PVOID>(Overall_MyGetStockObject) },
                                        // OHK_SELECTOBJECT
                                        std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv },
                                            "SelectObject", reinterpret_cast<PVOID>(Overall_MySelectObject) },
                                            // OHK_BITBLT
                                            std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv },
                                                "BitBlt", reinterpret_cast<PVOID>(Overall_MyBitBlt) },
                                                // OHK_DRAWTHEMEBG
                                                std::tuple{ std::array{ "uxtheme.dll"sv, "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv, ""sv },
                                                    "DrawThemeBackground", reinterpret_cast<PVOID>(Overall_MyDrawThemeBg) },
                                                    // OHK_DRAWTHEMETEXTEX
                                                    std::tuple{ std::array{ "uxtheme.dll"sv, "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv, ""sv },
                                                        "DrawThemeTextEx", reinterpret_cast<PVOID>(Overall_MyDrawThemeTextEx) },
                                                    // OHK_ANIMATEWINDOW
                                                   std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
                                                       "AnimateWindow", reinterpret_cast<PVOID>(Overall_MyAnimateWindow) },
                                                       // OHK_CREATEHATCHBRUSH
                                                       std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv },
                                                           "CreateHatchBrush", reinterpret_cast<PVOID>(Overall_MyCreateHatchBrush) },
                                                       // OHK_FILLRECT — COMDLG32 owner-draw ComboLBox (file dialog)
                                                       std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
                                                           "FillRect", reinterpret_cast<PVOID>(Overall_MyFillRect) },
                                                       // OHK_PATBLT — comctl32 PATCOPY с COLOR_WINDOW
                                                       std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv },
                                                           "PatBlt", reinterpret_cast<PVOID>(Overall_MyPatBlt) },
                                                       // OHK_GETSYSCOLORBRUSH — COMDLG32 COLOR_WINDOW → BLACK_BRUSH
                                                       std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
                                                           "GetSysColorBrush", reinterpret_cast<PVOID>(Overall_MyGetSysColorBrush) },
                                            } };

    using OverallDispatcher = HookDispatcher<OHK_COUNT, 3>;

    static std::unordered_map<void*, OverallDispatcher> g_overallMap;
    static wil::srwlock g_overallLock;

    // ---- EnableHooksOnModule ------------------------------------------------
    static bool EnableHooksOnModule(void* baseAddress, size_t index, bool enable)
    {
        //DebugLog(L"[GlassCore][EnableHooksOnModule] baseAddress: %p index: %d enable: %d", baseAddress, index, enable);
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

        (void)it->second.EnableHook(index, enable);
        return true;
    }

    // ---- DisableAllOverallHooks ---------------------------------------------
    static void DisableAllOverallHooks()
    {
        auto lock = g_overallLock.lock_exclusive();
        for (auto& [addr, dispatcher] : g_overallMap)
            dispatcher.DisableAllHooks();
        g_overallMap.clear();
    }

    // ---- Предпрогрев IAT-кеша (только чтение, без записей) ------------------
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
            && _wcsicmp(base, L"MFC42u.dll") != 0 && _wcsicmp(base, L"dmdskmgr.dll") != 0
            && _wcsicmp(base, L"dmview.ocx") != 0 && _wcsicmp(base, L"sysmon.ocx") != 0)
            return;
        PrewarmModule(mod);
    }

    // ---- GetCallerModule ----------------------------------------------------
    static HMODULE GetCallerModule(void* returnAddress)
    {
        HMODULE hMod = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(returnAddress),
            &hMod);
        return hMod;
    }

    // ---- GetOverallOrigSafe -------------------------------------------------
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
    //  Overall hook implementations
    // =========================================================================

    static HBRUSH CreateAlphaHatchBrush(int fnStyle, COLORREF color)
    {
        if (fnStyle < 0 || fnStyle > 5)
            return Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(color, 255);

        // Раскладываем цвет штриховки (premultiplied не нужен — alpha=255 у foreground)
        const BYTE r = GetRValue(color);
        const BYTE g = GetGValue(color);
        const BYTE b = GetBValue(color);

        // Создаём 8×8 32bpp DIBSection
        BITMAPV4HEADER bmi{};
        bmi.bV4Size = sizeof(BITMAPV4HEADER);
        bmi.bV4Width = 8;
        bmi.bV4Height = -8; // top-down
        bmi.bV4Planes = 1;
        bmi.bV4BitCount = 32;
        bmi.bV4V4Compression = BI_BITFIELDS;
        bmi.bV4RedMask = 0x00FF0000;
        bmi.bV4GreenMask = 0x0000FF00;
        bmi.bV4BlueMask = 0x000000FF;
        bmi.bV4AlphaMask = 0xFF000000;

        void* bits = nullptr;
        HBITMAP hbm = CreateDIBSection(nullptr,
            reinterpret_cast<BITMAPINFO*>(&bmi),
            DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!hbm) return CreateHatchBrush(fnStyle, color);

        auto* px = static_cast<DWORD*>(bits);
        const DWORD fg = (0xFF000000u)        // alpha=255
            | (static_cast<DWORD>(r) << 16)
            | (static_cast<DWORD>(g) << 8)
            | b;
        const DWORD bg = 0x00000000u;         // alpha=0, прозрачный

        const auto& rows = kHatchPatterns[fnStyle];
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                px[y * 8 + x] = (rows[y] & (0x80 >> x)) ? fg : bg;

        HBRUSH hbr = CreatePatternBrush(hbm);
        DeleteObject(hbm); // brush владеет копией
        return hbr;
    }

    HBRUSH WINAPI Overall_MyCreateHatchBrush(int fnStyle, COLORREF color)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_CREATEHATCHBRUSH, decltype(&CreateHatchBrush)>(callerMod);

        if (IsActive())
            return CreateAlphaHatchBrush(fnStyle, color);

        return orig ? orig(fnStyle, color) : CreateHatchBrush(fnStyle, color);
    }

    // ── Overall: MyGetSysColorBrush — file dialog owner-draw (COMDLG32) ─────
    HBRUSH WINAPI Overall_MyGetSysColorBrush(int nIndex)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_GETSYSCOLORBRUSH, decltype(&GetSysColorBrush)>(callerMod);

        if (IsActive() && nIndex == COLOR_WINDOW)
            return GetStockBrush(BLACK_BRUSH);

        return orig ? orig(nIndex) : GetSysColorBrush(nIndex);
    }

    // Кисть «оконного» фона (COLOR_WINDOW / BTNFACE / белый) — под стекло → BLACKNESS.
    static bool IsGlassClearBrush(HBRUSH hbr) noexcept
    {
        if (!hbr)
            return false;
        if (hbr == GetSysColorBrush(COLOR_WINDOW) || hbr == GetSysColorBrush(COLOR_BTNFACE))
            return true;

        LOGBRUSH lb{};
        if (!GetObject(hbr, sizeof(lb), &lb) || lb.lbStyle != BS_SOLID)
            return false;

        const COLORREF rgb = lb.lbColor & 0xFFFFFF;
        return rgb == (GetSysColor(COLOR_WINDOW) & 0xFFFFFF)
            || rgb == (GetSysColor(COLOR_BTNFACE) & 0xFFFFFF)
            || rgb == 0xFFFFFF;
    }

    static bool IsHighlightBrush(HBRUSH hbr) noexcept
    {
        if (!hbr)
            return false;
        if (hbr == GetSysColorBrush(COLOR_HIGHLIGHT))
            return true;

        LOGBRUSH lb{};
        if (!GetObject(hbr, sizeof(lb), &lb) || lb.lbStyle != BS_SOLID)
            return false;
        return (lb.lbColor & 0xFFFFFF) == (GetSysColor(COLOR_HIGHLIGHT) & 0xFFFFFF);
    }

    // ── Overall: MyFillRect — COMDLG32 owner-draw ComboLBox (file dialog) ───
    BOOL WINAPI Overall_MyFillRect(HDC hdc, const RECT* lprc, HBRUSH hbr)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_FILLRECT, decltype(&FillRect)>(callerMod);

        if (!orig)
            return FillRect(hdc, lprc, hbr);

        if (!IsActive() || !lprc || !hbr)
            return orig(hdc, lprc, hbr);

        if (IsGlassClearBrush(hbr))
        {
            PatBlt(hdc,
                lprc->left, lprc->top,
                lprc->right - lprc->left, lprc->bottom - lprc->top,
                BLACKNESS);
            return TRUE;
        }

        if (IsHighlightBrush(hbr))
        {
            HBRUSH alphaBrush = Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(
                GetSysColor(COLOR_HIGHLIGHT), 255);
            if (alphaBrush)
            {
                const BOOL res = orig(hdc, lprc, alphaBrush);
                DeleteObject(alphaBrush);
                return res;
            }
        }

        return orig(hdc, lprc, hbr);
    }

    // ── Overall: MyPatBlt — comctl32 PATCOPY с COLOR_WINDOW ─────────────────
    BOOL WINAPI Overall_MyPatBlt(HDC hdc, int x, int y, int cx, int cy, DWORD rop)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_PATBLT, decltype(&PatBlt)>(callerMod);

        if (!orig)
            return PatBlt(hdc, x, y, cx, cy, rop);

        if (!IsActive() || rop != PATCOPY)
            return orig(hdc, x, y, cx, cy, rop);

        HGDIOBJ const hbrObj = GetCurrentObject(hdc, OBJ_BRUSH);
        if (hbrObj && IsGlassClearBrush(static_cast<HBRUSH>(hbrObj)))
            return PatBlt(hdc, x, y, cx, cy, BLACKNESS);

        return orig(hdc, x, y, cx, cy, rop);
    }

    // ── Overall: MyDrawTextW ─────────────────────────────────────────────────
    int WINAPI Overall_MyDrawTextW(
        HDC hdc, LPCWSTR lpchText, int cchText, LPRECT lprc, UINT format)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_DRAWTEXT, decltype(&DrawTextW)>(callerMod);
        //DebugLog(L"[GlassCore][Overall_MyDrawTextW]");

        if (!IsActive())
            return orig ? orig(hdc, lpchText, cchText, lprc, format)
            : DrawTextW(hdc, lpchText, cchText, lprc, format);

        if (format & DT_CALCRECT)
            return orig ? orig(hdc, lpchText, cchText, lprc, format)
            : DrawTextW(hdc, lpchText, cchText, lprc, format);

        // Выделение — непрозрачная заливка; фон — PatBlt(BLACKNESS) → alpha=0
        COLORREF color = GetBkColor(hdc);
        if (color == GetSysColor(COLOR_HIGHLIGHT))
        {
            HBRUSH brush = Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(color, 255);
            if (brush)
            {
                FillRect(hdc, lprc, brush);
                DeleteObject(brush);
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

        return orig ? orig(hdc, lpchText, cchText, lprc, format)
            : DrawTextW(hdc, lpchText, cchText, lprc, format);
    }

    // ── Overall: MyDrawTextExW ───────────────────────────────────────────────
    int WINAPI Overall_MyDrawTextExW(
        HDC hdc, LPWSTR lpchText, int cchText,
        LPRECT lprc, UINT format, LPDRAWTEXTPARAMS lpdtp)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_DRAWTEXTEX, decltype(&DrawTextExW)>(callerMod);
        //DebugLog(L"[GlassCore][ComboLBox] Overall_MyDrawTextExW");

        if (!IsActive())
            return orig(hdc, lpchText, cchText, lprc, format, lpdtp);

        if (format & DT_CALCRECT)
            return orig(hdc, lpchText, cchText, lprc, format, lpdtp);

        if (lpdtp)
            return orig(hdc, lpchText, cchText, lprc, format, lpdtp);

        int result = 0;
        if (!FAILED(GlassCore::Text::DrawTextWithAlpha(hdc, lpchText, cchText, lprc, format, result)))
            return result;

        return orig(hdc, lpchText, cchText, lprc, format, lpdtp);
    }

    // ── Overall: MyExtTextOutW ───────────────────────────────────────────────
    // Owner-draw COMDLG32: ETO_OPAQUE + COLOR_WINDOW (или SetBkColor→0) не должен
    // уходить в orig — иначе белый/чёрный непрозрачный text-rect. Как ListBox:
    // BLACKNESS + DrawTextWithAlpha с DT_* (не TA_* из GetTextAlign).
    BOOL WINAPI Overall_MyExtTextOutW(
        HDC hdc, int x, int y, UINT options, const RECT* lprect,
        LPCWSTR lpString, UINT c, const INT* lpDx)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_EXTTEXTOUT, decltype(&ExtTextOutW)>(callerMod);

        if (!IsActive())
            return orig ? orig(hdc, x, y, options, lprect, lpString, c, lpDx)
            : ExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);


        if ((options & ETO_CLIPPED) && lprect)
        {
            RECT client{};
            if (h_combolbox)
                GetClientRect(h_combolbox, &client);
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
            RECT client{};
            if (h_combolbox)
                GetClientRect(h_combolbox, &client);
            const bool mfcInset = IsModuleName(callerMod, L"MFC42u.dll")
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
            // WINDOW / white / 0 без MFC-inset — без opaque fill (паритет BWM)
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
            const bool lbTextOk = lpString && !FAILED(GlassCore::Text::DrawTextWithAlpha(
                    hdc, lpString, static_cast<int>(c), &textRect, dwTextFlags, result));
            if (lbTextOk)
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

        // Fallback: не звать orig с ETO_OPAQUE на оконном/нулевом фоне
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

    // ── Overall: MyTextOutW ──────────────────────────────────────────────────
    BOOL WINAPI Overall_MyTextOutW(HDC hdc, int x, int y, LPCWSTR lpString, int c)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_TEXTOUT, decltype(&TextOutW)>(callerMod);
        //DebugLog(L"[GlassCore][Overall_MyTextOutW]");

        if (!IsActive())
            return orig ? orig(hdc, x, y, lpString, c)
            : TextOutW(hdc, x, y, lpString, c);

        int result = 0;
        RECT rc{};
        GetClipBox(hdc, &rc);
        rc.left = x;
        rc.top = y;
        if (!FAILED(GlassCore::Text::DrawTextWithAlpha(hdc, lpString, c, &rc, DT_EXPANDTABS | DT_NOPREFIX, result)))
            return result;

        return orig ? orig(hdc, x, y, lpString, c)
            : TextOutW(hdc, x, y, lpString, c);
    }

    // ── Overall: MySetBkColor ────────────────────────────────────────────────
    COLORREF WINAPI Overall_MySetBkColor(HDC hdc, COLORREF color)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_SETBKCOLOR, decltype(&SetBkColor)>(callerMod);
        //DebugLog(L"[GlassCore][Overall_MySetBkColor]");

        if (!orig)
            return SetBkColor(hdc, color);

        if (IsActive())
        {
            if (color == GetSysColor(COLOR_WINDOW) || color == GetSysColor(COLOR_BTNFACE))
                color = 0;
        }

        return orig ? orig(hdc, color) : SetBkColor(hdc, color);
    }

    // ── Overall: MyCreateSolidBrush ──────────────────────────────────────────
    HBRUSH WINAPI Overall_MyCreateSolidBrush(COLORREF color)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_CREATESOLIDBRUSH, decltype(&CreateSolidBrush)>(callerMod);
        //DebugLog(L"[GlassCore][Overall_MyCreateSolidBrush]");

        if (IsActive())
        {
            // Кисть выделения должна быть непрозрачной — иначе hover-highlight прозрачный
            if (color == GetSysColor(COLOR_HIGHLIGHT))
                return Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(color, 255);

            // Системные фоновые цвета → прозрачный чёрный
            if (color == GetSysColor(COLOR_WINDOW) || color == 0xffffff ||
                color == GetSysColor(COLOR_BTNFACE))
                color = 0;
        }

        return orig ? orig(color) : CreateSolidBrush(color);
    }

    // ── Overall: MyGetStockObject ────────────────────────────────────────────
    HGDIOBJ WINAPI Overall_MyGetStockObject(int i)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_GETSTOCKOBJECT, decltype(&GetStockObject)>(callerMod);

        if (IsActive())
        {
            if (i == WHITE_BRUSH)
                i = BLACK_BRUSH;
        }

        return orig ? orig(i) : GetStockObject(i);
    }

    // ── Overall: MySelectObject ──────────────────────────────────────────────
    HGDIOBJ WINAPI Overall_MySelectObject(HDC hdc, HGDIOBJ h)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_SELECTOBJECT, decltype(&SelectObject)>(callerMod);
        //DebugLog(L"[GlassCore][Overall_MySelectObject]");

        if (IsActive())
        {
            LOGPEN pn{};
            if (GetObject(h, sizeof(pn), &pn))
            {
                if (pn.lopnColor == GetSysColor(COLOR_WINDOW) ||
                    pn.lopnColor == GetSysColor(COLOR_BTNFACE))
                    return 0;
            }
        }

        return orig ? orig(hdc, h) : SelectObject(hdc, h);
    }

    // ── Overall: MyBitBlt ────────────────────────────────────────────────────
    BOOL WINAPI Overall_MyBitBlt(
        HDC hdc, int x, int y, int cx, int cy,
        HDC hdcSrc, int x1, int y1, DWORD rop)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_BITBLT, decltype(&BitBlt)>(callerMod);

        if (IsActive())
        {
            if (rop == SRCCOPY || rop == WHITENESS)
            {
                PatBlt(hdc, x, y, cx, cy, BLACKNESS);
                return TRUE;
            }
        }

        return orig ? orig(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop)
            : BitBlt(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);
    }

    // ── Overall: MyDrawThemeBg — тема ScrollBar: orig из карты ScrollBar (GetComctlScrollBarDrawThemeOrig), не chainOrig Overall.
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

        if (!IsActive())
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

        WCHAR themeClass[MAX_PATH + 1]{};
        const bool haveClass = SUCCEEDED(
            Shared::ThemeHelper::GetThemeClass(hTheme, themeClass, MAX_PATH));

        if (haveClass && StrEq(themeClass, L"ScrollBar"))
        {
            auto sbOrig = GlassCore::ScrollBarTheme::GetComctlScrollBarDrawThemeOrig(callerMod);
            auto dtbOrig  = sbOrig ? sbOrig : orig;
            if (!dtbOrig)
                return DrawThemeBackground(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

            GlassCore::ScrollBarDrawTheme::LogScrollBarDrawThemeCall(
                L"[GlassCore][ComboLBox]",
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
            if (!pRect || IsRectEmpty(pRect))
                return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

            if (GlassCore::ComboAddressPieSkip::IsAddressPieComboBoxEx32(h_combolbox))
                return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
        }

        return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
    }

    // ── Overall: MyDrawThemeTextEx — IAT uxtheme!DrawThemeTextEx (список ListBox / поле Combobox)
    HRESULT WINAPI Overall_MyDrawThemeTextEx(
        HTHEME hTheme,
        HDC hdc,
        int iPartId,
        int iStateId,
        LPCWSTR pszText,
        int cchText,
        DWORD dwTextFlags,
        LPRECT pRect,
        const DTTOPTS* pOptions)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_DRAWTHEMETEXTEX, decltype(&DrawThemeTextEx)>(callerMod);

        if (!IsActive())
            return orig ? orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions)
                : DrawThemeTextEx(
                    hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions);

        if (!pRect || IsRectEmpty(pRect))
            return orig ? orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions)
                : DrawThemeTextEx(
                    hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions);

        if (pOptions && (pOptions->dwFlags & DTT_CALCRECT))
            return orig ? orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions)
                : DrawThemeTextEx(
                    hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions);

        WCHAR themeClassName[MAX_PATH + 1]{};
        if (FAILED(Shared::ThemeHelper::GetThemeClass(hTheme, themeClassName, MAX_PATH)))
            return orig ? orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions)
                : DrawThemeTextEx(
                    hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions);

        if (!StrEq(themeClassName, L"Combobox") && !StrEq(themeClassName, L"ListBox"))
            return orig ? orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions)
                : DrawThemeTextEx(
                    hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions);

        if (StrEq(themeClassName, L"Combobox") && GlassCore::ComboAddressPieSkip::IsAddressPieComboBoxEx32(h_combolbox))
            return orig ? orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions)
                : DrawThemeTextEx(
                    hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions);

        RECT lpRect = *pRect;
        DTTOPTS opts{};
        if (pOptions)
        {
            opts = *pOptions;
            if (opts.dwSize < sizeof(DTTOPTS))
                opts.dwSize = sizeof(DTTOPTS);
        }
        else
            opts.dwSize = sizeof(DTTOPTS);
        opts.dwFlags |= DTT_COMPOSITED;

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
                    &lpRect,
                    &opts);
            });
    }

    // Общая логика: IAT (редко, shell32 и т.д.) и Detour на user32!AnimateWindow.
    static BOOL ComboLBox_ProcessAnimateWindow(
        HWND hWnd,
        DWORD dwTime,
        DWORD dwFlags,
        BOOL(WINAPI* pfnTrue)(HWND, DWORD, DWORD),
        bool detourPath,
        HMODULE callerModForIat)
    {
        wchar_t cls[64]{};
        GetClassNameW(hWnd, cls, 64);

        const bool allowCombo =
            (StrEq(cls, L"ComboLBox") || StrEq(cls, L"ATL:LISTBOX"))
            && (!detourPath || g_comboAnimateArmed.load(std::memory_order_acquire));

        WCHAR callerFile[MAX_PATH]{};
        const wchar_t* callerShort = L"?";
        if (callerModForIat && GetModuleFileNameW(callerModForIat, callerFile, _countof(callerFile)))
            callerShort = PathFindFileNameW(callerFile);

        DebugLog(DBG_IMPORTANT,
            L"[GlassCore][ComboLBox] AnimateWindow: detour=%d class=\"%s\" time=%lu flags=0x%X caller=\"%s\" armed=%d",
            detourPath ? 1 : 0,
            cls,
            dwTime,
            dwFlags,
            callerShort,
            detourPath && g_comboAnimateArmed.load(std::memory_order_acquire) ? 1 : 0);

        if (allowCombo)
        {
            DebugLog(DBG_IMPORTANT, L"[GlassCore][ComboLBox] AnimateWindow: branch ComboLBox/ATL → ShowWindow");
            ShowWindow(hWnd, SW_SHOW);
            if (detourPath)
                g_comboAnimateArmed.store(false, std::memory_order_release);
            else if (callerModForIat)
                EnableHooksOnModule(callerModForIat, OHK_ANIMATEWINDOW, false);
            return TRUE;
        }

        BOOL result = pfnTrue ? pfnTrue(hWnd, dwTime, dwFlags) : FALSE;

        DebugLog(DBG_IMPORTANT,
            L"[GlassCore][ComboLBox] AnimateWindow: branch other class=\"%s\" result=%d",
            cls,
            static_cast<int>(result));

        if (detourPath)
        {
            if (!StrEq(cls, L"FloatNotifySink"))
                g_comboAnimateArmed.store(false, std::memory_order_release);
        }
        else if (!StrEq(cls, L"FloatNotifySink") && callerModForIat)
            EnableHooksOnModule(callerModForIat, OHK_ANIMATEWINDOW, false);

        return result;
    }

    static BOOL WINAPI Detour_User32AnimateWindow(HWND hWnd, DWORD dwTime, DWORD dwFlags)
    {
        if (!g_comboAnimateArmed.load(std::memory_order_acquire))
            return g_trueUser32AnimateWindow(hWnd, dwTime, dwFlags);

        HMODULE const callerMod = GetCallerModule(_ReturnAddress());
        return ComboLBox_ProcessAnimateWindow(
            hWnd,
            dwTime,
            dwFlags,
            g_trueUser32AnimateWindow,
            true,
            callerMod);
    }

    static void EnsureUser32AnimateDetour() noexcept
    {
        if (g_user32AnimateDetourInstalled)
            return;

        const HRESULT hr = Hooks::Detours::Write([&] {
            Hooks::Detours::Attach(
                reinterpret_cast<void**>(&g_trueUser32AnimateWindow),
                reinterpret_cast<void*>(&Detour_User32AnimateWindow));
        });
        if (FAILED(hr))
        {
            DebugLog(DBG_WARNING, L"[GlassCore][ComboLBox] user32 AnimateWindow Detour attach hr=0x%08X", hr);
            return;
        }

        g_user32AnimateDetourInstalled = true;
        DebugLog(DBG_IMPORTANT, L"[GlassCore][ComboLBox] user32 AnimateWindow Detour installed");
    }

    BOOL WINAPI Overall_MyAnimateWindow(HWND hWnd, DWORD dwTime, DWORD dwFlags)
    {
        HMODULE const callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_ANIMATEWINDOW, decltype(&AnimateWindow)>(callerMod);
        BOOL(WINAPI* pfnTrue)(HWND, DWORD, DWORD) = orig ? orig : &::AnimateWindow;
        return ComboLBox_ProcessAnimateWindow(hWnd, dwTime, dwFlags, pfnTrue, false, callerMod);
    }

    // Один экземпляр comctl32: полный набор ComboLBox-хуков. В BWM эта роль —
    // ComboBoxHooks на v6 (WinSxS), а legacy-экземпляр шёл через OverallHooks;
    // в Glass карта g_overallMap единая, поэтому набор одинаков для обоих экземпляров.
    static void EnableComboHooksOnModule(void* mod, bool enable)
    {
        // Standard path (non-themed): DrawTextExW, ExtTextOutW
        EnableHooksOnModule(mod, OHK_DRAWTEXTEX, enable);
        EnableHooksOnModule(mod, OHK_EXTTEXTOUT, enable);
        // DrawThemeText / DrawThemeTextEx: в comctl32 нет статического IAT на uxtheme для DTTEx на Win11;
        // список — ExtTextOutW (хуки стоят на обоих экземплярах — callerMod покрыт в любом случае).
        //EnableHooksOnModule(mod, OHK_DRAWTHEMETEXT, enable);
        EnableHooksOnModule(mod, OHK_DRAWTHEMEBG, enable);
        // Общие: фоновые операции и перья
        EnableHooksOnModule(mod, OHK_SELECTOBJECT, enable);
        EnableHooksOnModule(mod, OHK_SETBKCOLOR, enable);
        EnableHooksOnModule(mod, OHK_GETSTOCKOBJECT, enable);
        EnableHooksOnModule(mod, OHK_CREATESOLIDBRUSH, enable);
        // File dialog ComboLBox: comctl32 PatBlt(PATCOPY) с COLOR_WINDOW
        EnableHooksOnModule(mod, OHK_PATBLT, enable);
        EnableHooksOnModule(mod, OHK_GETSYSCOLORBRUSH, enable);
    }

    // На ComboLBox::Startup comctl32 может ещё не быть загружен.
    // ModuleHandler заполняет g_comctl32New/g_comctl32Legacy после LdrDllNotification;
    // повторяем резолв при первом Paint/DropDown. Кешируем ОБА экземпляра сразу:
    // поздняя подгрузка второго экземпляра (первый attach его не застал) докачивается
    // через OnComctl32ModuleLoaded и здесь (если primary уже был, а legacy пришёл позже).
    //
    // Флаг активной сессии — определение здесь (выше Arm/Disarm): OnComctl32ModuleLoaded
    // читает его, чтобы докачать хуки на поздний экземпляр без расбаланса ref-count.
    thread_local bool g_hooksArmed{ false };

    // true — модуль закэширован только что (нужно для докачки хуков при поздней подгрузке).
    static bool CacheComctlModule(void* mod) noexcept
    {
        if (!mod)
            return false;
        auto lock = g_overallLock.lock_exclusive();
        if (g_overallMap.find(mod) != g_overallMap.end())
            return false;
        auto [it, inserted] = g_overallMap.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(mod),
            std::forward_as_tuple(g_overallHookDep, mod));
        it->second.CacheHookData();
        DebugLog(L"[GlassCore][ComboLBox] comctl32=0x%p, overall hooks cached", mod);
        return true;
    }

    static void EnsureComctl32Ready() noexcept
    {
        // Подтягиваем оба глобальных handle (могли появиться после Startup).
        if (!g_comctl32Module)
        {
            if (GlassCore::ModuleHandler::g_comctl32New)
                g_comctl32Module = GlassCore::ModuleHandler::g_comctl32New;
            else if (GlassCore::ModuleHandler::g_comctl32Legacy)
                g_comctl32Module = GlassCore::ModuleHandler::g_comctl32Legacy;
        }
        if (!g_comctl32ModuleLegacy)
        {
            HMODULE legacy = GlassCore::ModuleHandler::g_comctl32Legacy;
            if (legacy && legacy != g_comctl32Module)
                g_comctl32ModuleLegacy = legacy;
            else
            {
                // New мог подгрузиться позже primary (primary тогда был legacy).
                HMODULE fresh = GlassCore::ModuleHandler::g_comctl32New;
                if (fresh && fresh != g_comctl32Module)
                    g_comctl32ModuleLegacy = fresh;
            }
        }
        if (!g_comctl32Module && !g_comctl32ModuleLegacy)
            return;

        // Блокировку берёт CacheComctlModule; здесь — без лока (только чтение указателей).
        void* n = g_comctl32Module;
        void* l = g_comctl32ModuleLegacy;
        if (n) CacheComctlModule(n);
        if (l && l != n) CacheComctlModule(l);
    }

    // Поздняя подгрузка comctl32 (второй экземпляр): ModuleHandler зовёт после AddComctl32.
    // Если сессия уже armed на этом потоке — включаем хуки только на докачанных
    // экземплярах (позже Disarm снимет со всех известных — баланс ref-count сохранён).
    void OnComctl32ModuleLoaded() noexcept
    {
        if (g_shutdown.load(std::memory_order_acquire))
            return;
        void* beforePrimary = g_comctl32Module;
        void* beforeLegacy = g_comctl32ModuleLegacy;
        EnsureComctl32Ready();
        if (!g_hooksArmed)
            return;
        void* after[2]{ g_comctl32Module, g_comctl32ModuleLegacy };
        void* armedNow = nullptr;
        for (void* mod : after)
        {
            if (!mod || mod == beforePrimary || mod == beforeLegacy || mod == armedNow)
                continue;
            armedNow = mod;
            EnableComboHooksOnModule(mod, true);
        }
    }

    // Выгрузка comctl32: снять IAT и выбросить stale-запись (IATRegistry владеет слотами;
    // держать кеш выгруженного модуля — use-after-free при следующем Enable).
    void OnComctl32ModuleUnload(HMODULE unloaded) noexcept
    {
        if (!unloaded)
            return;
        if (g_comctl32Module == unloaded)
            g_comctl32Module = nullptr;
        if (g_comctl32ModuleLegacy == unloaded)
            g_comctl32ModuleLegacy = nullptr;
        auto lock = g_overallLock.lock_exclusive();
        auto it = g_overallMap.find(static_cast<void*>(unloaded));
        if (it != g_overallMap.end())
        {
            it->second.DisableAllHooks();
            g_overallMap.erase(it);
            DebugLog(L"[GlassCore][ComboLBox] comctl32=0x%p unloaded, overall entry dropped", unloaded);
        }
    }

    // =========================================================================
    //  EnableComboHooks — хуки на comctl32 через g_overallMap
    // =========================================================================

    static void EnableComboHooks(bool enable)
    {
        if (enable)
            EnsureComctl32Ready();
        // Хуки на ОБА экземпляра comctl32: ref-count в HookDispatcher независим
        // на каждый модуль, Arm/Disarm всегда трогают один и тот же набор — баланс.
        // (Поздняя подгрузка второго экземпляра при активной сессии докачивается
        // в OnComctl32ModuleLoaded; страховка от расбаланса — guard в HookDispatcher::EnableHook.)
        ForEachKnownComctlModule([enable](void* mod) {
            EnableComboHooksOnModule(mod, enable);
        });
    }

    // =========================================================================
    //  EnableOverallModuleHooks — хуки на COMDLG32, MFC и пр.
    // =========================================================================

    static void EnableOverallModuleHooks(bool enable)
    {
        // COMDLG32.dll — диалоги файлов (OwnerDraw ComboLBox)
        if (PVOID base = GetModuleHandleW(L"COMDLG32.dll"))
        {
            EnableHooksOnModule(base, OHK_DRAWTEXTEX, enable);
            EnableHooksOnModule(base, OHK_DRAWTEXT, enable);
            EnableHooksOnModule(base, OHK_TEXTOUT, enable);
            EnableHooksOnModule(base, OHK_CREATESOLIDBRUSH, enable);
            EnableHooksOnModule(base, OHK_EXTTEXTOUT, enable);
            EnableHooksOnModule(base, OHK_SETBKCOLOR, enable);
            EnableHooksOnModule(base, OHK_FILLRECT, enable);
            EnableHooksOnModule(base, OHK_GETSYSCOLORBRUSH, enable);
        }

        // MFC42u.dll
        if (PVOID base = GetModuleHandleW(L"MFC42u.dll"))
            EnableHooksOnModule(base, OHK_EXTTEXTOUT, enable);

        // dmdskmgr.dll
        if (PVOID base = GetModuleHandleW(L"dmdskmgr.dll"))
        {
            EnableHooksOnModule(base, OHK_EXTTEXTOUT, enable);
            EnableHooksOnModule(base, OHK_CREATEHATCHBRUSH, enable);
            EnableHooksOnModule(base, OHK_FILLRECT, enable);
        }

        // dmview.ocx — Disk Management view control
        if (PVOID base = GetModuleHandleW(L"dmview.ocx"))
        {
            EnableHooksOnModule(base, OHK_EXTTEXTOUT, enable);
            EnableHooksOnModule(base, OHK_FILLRECT, enable);
        }

        // charmap.exe
        if (PVOID base = GetModuleHandleW(L"charmap.exe"))
            EnableHooksOnModule(base, OHK_BITBLT, enable);

        // sysmon.ocx
        if (PVOID base = GetModuleHandleW(L"sysmon.ocx"))
        {
            EnableHooksOnModule(base, OHK_CREATESOLIDBRUSH, enable);
            EnableHooksOnModule(base, OHK_GETSTOCKOBJECT, enable);
        }

        // EntryPointModule — EXE хост-процесса
        if (PVOID base = GetModuleHandleW(nullptr))
            EnableHooksOnModule(base, OHK_EXTTEXTOUT, enable);
    }

    // =========================================================================
    //  Session IAT — только на idle↔active (как BWM hookRef; без thrash на CompatDC)
    // =========================================================================

    static void ArmSessionHooks(HWND /*hWnd*/, HMODULE /*callerMod*/) noexcept
    {
        // DEBUG diskmgmt: баланс arm/disarm — снять после диагностики.
        if (g_hooksArmed)
            return;
        EnableComboHooks(true);
        EnableOverallModuleHooks(true);
        g_hooksArmed = true;
    }

    static void DisarmSessionHooks(HWND /*hWnd*/, HMODULE /*callerMod*/) noexcept
    {
        // DEBUG diskmgmt: баланс arm/disarm — снять после диагностики.
        if (!g_hooksArmed || IsActive())
            return;
        EnableComboHooks(false);
        EnableOverallModuleHooks(false);
        g_hooksArmed = false;
    }

    // =========================================================================
    //  PaintDispatcher provider callbacks (static, вызываются из Detours-хуков)
    // =========================================================================
    //
    // Три провайдера — каждый срабатывает только при соответствующем типе DC:
    //   OnPaintBeginGlobal   ← HookBeginPaint         → g_paintProviders
    //   OnHotBeginGlobal     ← HookGetDC               → g_getDCProviders
    //   OnScrollBeginGlobal  ← HookCreateCompatibleDC  → g_compatDCProviders
    // Проверяет SetProp-маркер окна вместо временного depth-флага из SubclassProc.

    static void OnPaintBeginGlobal(HWND hWnd, HMODULE callerMod)
    {
        using namespace GlassShared;
        if (GlassGetWindowKind(hWnd) != GlassControlKind::ComboLBox || g_activePaintMod) return;
        GlassCore::ScrollBarTheme::EnsureComctlScrollBarIatIfComctlCaller(hWnd, callerMod);
        g_activePaintMod = callerMod;
        h_combolbox = hWnd;
        ++g_paintRef;
        ArmSessionHooks(hWnd, callerMod);
    }

    static void OnPaintEndGlobal(HWND hWnd, HMODULE callerMod)
    {
        if (!g_activePaintMod) return;
        g_activePaintMod = nullptr;
        if (--g_paintRef < 0) g_paintRef = 0;
        DisarmSessionHooks(hWnd, callerMod);
        if (!IsActive()) h_combolbox = nullptr;
    }

    static void OnHotBeginGlobal(HWND hWnd, HMODULE callerMod)
    {
        using namespace GlassShared;
        if (GlassGetWindowKind(hWnd) != GlassControlKind::ComboLBox || g_activeHotMod) return;
        GlassCore::ScrollBarTheme::EnsureComctlScrollBarIatIfComctlCaller(hWnd, callerMod);
        g_activeHotMod = callerMod;
        h_combolbox = hWnd;
        ++g_hotRef;
        ArmSessionHooks(hWnd, callerMod);
    }

    static void OnHotEndGlobal(HWND hWnd, HMODULE callerMod)
    {
        if (!g_activeHotMod) return;
        g_activeHotMod = nullptr;
        if (--g_hotRef < 0) g_hotRef = 0;
        // FIX owner-draw (dmdskmgr): гасим и на этом канале, иначе окно остаётся
        // armed-but-inactive — неактивная голова отдаёт orig в трамплин, а хвост
        // цепочки (Root) слепнет по caller и уходит в сырой API мимо стекла.
        // Disarm внутри сам проверяет !IsActive, thrash ограничен refcount-гейтом.
        DisarmSessionHooks(hWnd, callerMod);
        if (!IsActive()) h_combolbox = nullptr;
        // DEBUG diskmgmt: баланс refs — снять после диагностики.
    }

    static void OnScrollBeginGlobal(HWND hWnd, HMODULE callerMod)
    {
        using namespace GlassShared;
        if (GlassGetWindowKind(hWnd) != GlassControlKind::ComboLBox || g_activeScrollMod) return;
        GlassCore::ScrollBarTheme::EnsureComctlScrollBarIatIfComctlCaller(hWnd, callerMod);
        g_activeScrollMod = callerMod;
        h_combolbox = hWnd;
        ++g_scrollRef;
        ArmSessionHooks(hWnd, callerMod);
    }

    static void OnScrollEndGlobal(HWND hWnd, HMODULE callerMod)
    {
        if (!g_activeScrollMod) return;
        g_activeScrollMod = nullptr;
        if (--g_scrollRef < 0) g_scrollRef = 0;
        DisarmSessionHooks(hWnd, callerMod);
        if (!IsActive()) h_combolbox = nullptr;
        // DEBUG diskmgmt: баланс refs — снять после диагностики.
    }

    // =========================================================================
    //  Public API — WM_PRINTCLIENT: прямое включение/выключение хуков.
    //  PaintDispatcher не перехватывает DC переданный через wParam напрямую,
    //  поэтому хуки здесь включаются явно, минуя BeginPaint/GetDC/CreateCompatibleDC.
    // =========================================================================

    void OnComboLBoxPrintClientBegin(HWND hWnd)
    {
        if (g_shutdown.load(std::memory_order_acquire)) return;
        if (g_activePaintMod) return;  // уже активно
        HMODULE printCallerMod = GlassCore::ModuleHandler::g_comctl32New
            ? GlassCore::ModuleHandler::g_comctl32New
            : GlassCore::ModuleHandler::g_comctl32Legacy;
        if (printCallerMod)
        {
            // ScrollBar IAT — на оба экземпляра: PrintClient может идти через legacy.
            EnsureComctl32Ready();
            ForEachKnownComctlModule([hWnd](void* mod) {
                GlassCore::ScrollBarTheme::EnsureComctlScrollBarIatIfComctlCaller(
                    hWnd, static_cast<HMODULE>(mod));
            });
        }
        h_combolbox = hWnd;
        ++g_paintRef;
        ArmSessionHooks(hWnd, printCallerMod);
    }

    void OnComboLBoxPrintClientEnd(HWND hWnd)
    {
        // printCallerMod здесь не нужен: Disarm снимает сессию с обоих экземпляров.
        HMODULE printCallerMod = nullptr;
        if (--g_paintRef < 0) g_paintRef = 0;
        DisarmSessionHooks(hWnd, printCallerMod);
        if (!IsActive()) h_combolbox = nullptr;
    }

    namespace
    {
        thread_local bool  s_applyingComboLBoxFlyout = false;
        thread_local HWND  s_comboLBoxAppliedRoot    = nullptr;
        thread_local DWORD s_comboLBoxAppliedTick      = 0;

        void FillDropDownContext(WndCompat::DropDownContext& ctx) noexcept
        {
            ctx = {};
            ctx.useDarkMode = Shared::ThemeHelper::ResolveEffectiveDarkMode();
            GlassCore::Settings::QueryBackdropEffectContext(L"DropDown", ctx.useDarkMode, ctx.backdropEffect);
            GlassCore::Settings::QueryBorderContext(L"DropDown", ctx.useDarkMode, ctx.border);
            GlassCore::Settings::QueryFlyoutAnimationContext(L"DropDown", ctx.animation);
        }

        void ApplyComboLBoxFlyoutAtShow(HWND hWnd, bool isComboBoxAnimation) noexcept
        {
            if (!hWnd || !::IsWindow(hWnd))
                return;

            if (s_applyingComboLBoxFlyout)
                return;

            s_applyingComboLBoxFlyout = true;
            auto clearApplying = wil::scope_exit([] { s_applyingComboLBoxFlyout = false; });

            auto& ctx = WndCompat::g_dropDownCtx;
            FillDropDownContext(ctx);

            const bool schedulePopupIn =
                (isComboBoxAnimation && !ctx.animation.enable) || ctx.animation.enable;

            if (schedulePopupIn)
            {
                GlassCore::FlyoutFrame::SetHostBorderDeferred(hWnd, true);
                GlassCore::FlyoutFrame::SyncPrimeFlyoutNcBlack(hWnd);
            }

            GlassCore::Settings::ApplyPopupSwcaEffect(
                hWnd, ctx.useDarkMode, ctx.backdropEffect, ctx.border, false);

            if (!schedulePopupIn)
            {
                GlassCore::FlyoutFrame::PaintHostBorder(
                    hWnd, ctx.useDarkMode, ctx.border, ctx.backdropEffect.effectType);
            }

            DebugLog(DBG_IMPORTANT,
                L"[GlassCore][ComboLBox] ApplyComboLBoxFlyoutAtShow: hwnd=0x%p cornerType=%d anim=%d schedulePopupIn=%d",
                hWnd,
                static_cast<int>(ctx.border.cornerType),
                static_cast<int>(ctx.animation.enable),
                schedulePopupIn ? 1 : 0);

            s_comboLBoxAppliedRoot = hWnd;
            s_comboLBoxAppliedTick = ::GetTickCount();

            if (isComboBoxAnimation && !ctx.animation.enable)
            {
                const HRESULT hr = GlassCore::Animation::CreateDropDownPopupIn(
                    hWnd,
                    static_cast<float>(50) / 100.f,
                    std::chrono::milliseconds{ 250 },
                    std::chrono::milliseconds{ 87 },
                    0,
                    ctx.animation.immediateInterupting);
                DebugLog(L"[GlassCore][ComboLBox] CreateDropDownPopupIn (sys anim) hr=0x%08X hwnd=0x%p", hr, hWnd);
            }
            else if (ctx.animation.enable)
            {
                const HRESULT hr = GlassCore::Animation::CreateDropDownPopupIn(
                    hWnd,
                    static_cast<float>(ctx.animation.startRatio) / 100.f,
                    std::chrono::milliseconds{ ctx.animation.popInTime },
                    std::chrono::milliseconds{ ctx.animation.fadeInTime },
                    ctx.animation.popInStyle,
                    ctx.animation.immediateInterupting);
                DebugLog(L"[GlassCore][ComboLBox] CreateDropDownPopupIn hr=0x%08X hwnd=0x%p", hr, hWnd);
            }
        }
    }

    void OnComboLBoxShow(HWND hWnd, bool isComboBoxAnimation)
    {
        if (!hWnd || !::IsWindow(hWnd))
            return;

        const DWORD tick = ::GetTickCount();

        if (GlassCore::FlyoutFrame::IsHostBorderDeferred(hWnd))
        {
            DebugLog(L"[GlassCore][ComboLBox] OnComboLBoxShow: skip (defer active) hwnd=0x%p", hWnd);
            return;
        }

        if (s_applyingComboLBoxFlyout)
        {
            DebugLog(L"[GlassCore][ComboLBox] OnComboLBoxShow: skip (apply in progress) hwnd=0x%p", hWnd);
            return;
        }

        if (hWnd == s_comboLBoxAppliedRoot && (tick - s_comboLBoxAppliedTick) < 5000u)
        {
            DebugLog(L"[GlassCore][ComboLBox] OnComboLBoxShow: skip (already applied) hwnd=0x%p", hWnd);
            return;
        }

        DebugLog(L"[GlassCore][ComboLBox] OnComboLBoxShow: hwnd=0x%p isComboBoxAnim=%d",
            hWnd, static_cast<int>(isComboBoxAnimation));

        ApplyComboLBoxFlyoutAtShow(hWnd, isComboBoxAnimation);
    }

    void OnComboLBoxDetach(HWND hWnd)
    {
        DebugLog(L"[GlassCore][ComboLBox] OnComboLBoxDetach: hwnd=0x%p", hWnd);

        if (hWnd == s_comboLBoxAppliedRoot)
        {
            s_comboLBoxAppliedRoot = nullptr;
            s_comboLBoxAppliedTick = 0;
        }

        GlassCore::FlyoutFrame::ClearHostBorderDeferred(hWnd);
        GlassCore::Settings::DropEffect(L"DropDown", hWnd);
        COLORREF noColor = DWMWCP_DEFAULT;
        DwmSetWindowAttribute(hWnd, DWMWA_WINDOW_CORNER_PREFERENCE,
            &noColor, sizeof(noColor));
    }

    void OnComboBoxDropDown(HWND hComboBox)
    {
        EnsureUser32AnimateDetour();
        g_comboAnimateArmed.store(true, std::memory_order_release);


        DebugLog(DBG_IMPORTANT,
            L"[GlassCore][ComboLBox] OnComboBoxDropDown: AnimateWindow detour armed, combobox=0x%p",
            hComboBox);
    }
    // =========================================================================
    //  Startup / Shutdown
    // =========================================================================

    void Prepare() {}

    void Startup()
    {
        // Явные g_comctl32New/g_comctl32Legacy вместо неоднозначного
        // GetModuleHandleW(L"comctl32.dll") (возвращает один, неизвестно какой).
        EnsureComctl32Ready();
        PrewarmModule(g_comctl32Module);
        PrewarmModule(g_comctl32ModuleLegacy);
        PrewarmForModule(GetModuleHandleW(L"COMDLG32.dll"), L"COMDLG32.dll");
        PrewarmForModule(GetModuleHandleW(L"MFC42u.dll"), L"MFC42u.dll");
        PrewarmForModule(GetModuleHandleW(L"dmdskmgr.dll"), L"dmdskmgr.dll");
        PrewarmForModule(GetModuleHandleW(L"dmview.ocx"), L"dmview.ocx");
        PrewarmForModule(GetModuleHandleW(L"sysmon.ocx"), L"sysmon.ocx");
        GlassCore::ModuleHandler::RegisterModulePrewarmHandler(PrewarmForModule);

        if (!g_comctl32Module && !g_comctl32ModuleLegacy)
        {
            DebugLog(
                L"[GlassCore][ComboLBox] comctl32.dll ещё не в процессе — кеш IAT при первом Paint/DropDown");
        }

        // Регистрируем три провайдера в PaintDispatcher.
        // Paint   (BeginPaint/EndPaint)         → WM_PAINT, WM_KEYDOWN
        // GetDC   (GetDC/ReleaseDC)              → WM_MOUSEMOVE, WM_KEYDOWN (hotitem)
        // CompatDC (CreateCompatibleDC/DeleteDC) → WM_VSCROLL, WM_MOUSEWHEEL (scroll)
        GlassCore::PaintDispatcher::RegisterPaintProvider  ({ OnPaintBeginGlobal,  OnPaintEndGlobal  });
        GlassCore::PaintDispatcher::RegisterGetDCProvider  ({ OnHotBeginGlobal,    OnHotEndGlobal    });
        GlassCore::PaintDispatcher::RegisterCompatDCProvider({ OnScrollBeginGlobal, OnScrollEndGlobal });

        DebugLog(DBG_IMPORTANT, L"[GlassCore][ComboLBox] Startup: PaintDispatcher providers registered");

        EnsureUser32AnimateDetour();
    }

    void Shutdown()
    {
        g_shutdown.store(true, std::memory_order_seq_cst);

        // Ждём завершения активных paint/hot/scroll сессий (thread_local — примерная оценка).
        for (int i = 0; i < 100 && IsActive(); ++i)
            Sleep(1);

        if (g_user32AnimateDetourInstalled)
        {
            const HRESULT hr = Hooks::Detours::Write([&] {
                Hooks::Detours::Detach(
                    reinterpret_cast<void**>(&g_trueUser32AnimateWindow),
                    reinterpret_cast<void*>(&Detour_User32AnimateWindow));
            });
            if (FAILED(hr))
                DebugLog(DBG_WARNING, L"[GlassCore][ComboLBox] AnimateWindow Detour detach hr=0x%08X", hr);
            g_user32AnimateDetourInstalled = false;
        }

#if defined(_DEBUG)
        if (const HRESULT hrProbe = GlassCore::Hooks::DebugDetourHooks::SetEnabled(
                GlassCore::Hooks::DebugDetourHooks::DDH_ComboLBoxPaintProbe,
                false);
            FAILED(hrProbe))
            DebugLog(DBG_WARNING, L"[GlassCore][ComboLBox] DebugDetour ComboLBoxPaintProbe(off) hr=0x%08X", hrProbe);
#endif

        DisableAllOverallHooks();

        // Сбрасываем TLS-состояние текущего потока (shutdown всегда вызывается из GUI-потока)
        g_paintRef = g_hotRef = g_scrollRef = 0;
        g_activePaintMod = g_activeHotMod = g_activeScrollMod = nullptr;
        h_combolbox = nullptr;

        g_shutdown.store(false, std::memory_order_seq_cst);
    }

} // namespace GlassCore::ComboLBox