// Хуки для отрисовки ListView (SysListView32) и WinForms ListView (.SysListView32).
//
// Архитектура (PaintDispatcher + per-module g_overallMap):
//
// ListView рисует текст через ExtTextOutW (IAT в comctl32 → gdi32 или gdi32full),
// при themed ListView (comctl32 new) — DrawThemeTextEx на g_new или DrawTextExW на uxtheme
// (взаимоисключение по glow / hUx — см. EnableListViewHooksForModule).
// Фон — через DrawThemeBackground (uxtheme), заливки — FillRect (user32),
// иконки — через BitBlt/StretchBlt/MaskBlt (gdi32 / gdi32full).
// Хуки включаются на callerModule при каждом BeginPaint:
//   callerModule определяется через _ReturnAddress() в хуке PaintDispatcher.
//
// BWM-паттерн: PaintHandler.cpp lines 158-238.
//   SysListView32:
//     g_comctl32New:    ExtTextOutW, DrawTextW, FillRect,
//                       DrawThemeBackground, StretchBlt, BitBlt, MaskBlt
//                       + g_comctl32Legacy BitBlt
//     g_comctl32Legacy: FillRect, DrawTextW, ExtTextOutW, BitBlt
//                       + g_comctl32New BitBlt
//     sysmon.ocx:       DrawTextW, ExtTextOutW, CreateSolidBrush, Polyline, LineTo
//                       (только если callerMod = comctl32 new/legacy — см. EnableListViewHooksForModule)
//     SHELL32.dll:      DrawTextW
//
//   .SysListView32 (WinForms):
//     g_comctl32New:    FillRect, DrawTextW, SetBkColor, BitBlt
//                       + g_comctl32Legacy BitBlt
//     g_comctl32Legacy: CreateSolidBrush
//
// Иконки без альфа-канала:
//   BWM использует GDI+ (GdipCreateBitmapAlpha) — имеет баг потери альфы на 32bpp DIB.
//   Мы используем BitmapUtils::Promise32BPP (чистый GDI) + GdiAlphaBlend.

#include "CorePch.h"

#include <Hooks/ListView.h>
#include <Hooks/ScrollBarDrawTheme.h>
#include <Hooks/ScrollBarTheme.h>
#include <Subsystems/GdiHookUtils.h>
#include <Subsystems/Settings.h>
#include <Subsystems/RenderText.h>
#include <Subsystems/TextGlowClientRegistry.h>
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
#include <unordered_set>
#include <cmath>
#include <string>

// Диагностика ListView снята (финиш 2026-10-13): фон красится кистями Root-CSB.

#pragma comment(lib, "msimg32.lib")

using namespace GlassCore::Hooks;
using namespace std::literals;

namespace GlassCore::ListViewControl
{
    // =========================================================================
    //  LIFO сессий paint (модуль + kind для EnableListViewHooksForModule).
    // =========================================================================

    struct ListViewPaintSession
    {
        HMODULE                       mod{};
        GlassShared::GlassControlKind kind{};
        HWND                          hWnd{};
        bool                          needBlackout{};
    };
    thread_local std::vector<ListViewPaintSession> g_lvSessionStack;
    // Окна, чей фон уже чернили post-BLACKNESS (первый paint либо BkFlip).
    // Чернить каждый paint — стробоскоп при шторме; у BWM такого нет.
    thread_local std::unordered_set<HWND> g_lvBgPrimed;

    // Удаление фона иконок (Root\Control\ListView) — читается на входе внешней paint-сессии;
    // enable=false → black-key по умолчанию (см. GdiUtils::ApplyIconBackgroundAlpha).
    thread_local WndCompat::Menu::IconBackgroundColorRemoval g_lvIconRemoval{};

    static std::atomic<bool> g_shutdown{ false };

    // =========================================================================
    //  Helpers
    // =========================================================================

    static bool StrEq(LPCWSTR a, LPCWSTR b)
    {
        return _wcsicmp(a, b) == 0;
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
        return StrEq(name, base);
    }

    [[nodiscard]] static int GetScrollBandSizeForDpiCached(HDC hdc) noexcept
    {
        // Кэш на поток paint: пересчитываем только при смене DPI, чтобы не утяжелять FillRect.
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

    namespace
    {
        // Штрихи sysmon на SysListView32 — та же альфа, что kMmcWvcChartStrokeAlpha в RootGdiHooks.
        constexpr BYTE kSysmonListViewStrokeAlpha = 236;
    }

    // comctl32 после ListView_SetBkColor(0) часто рисует с GetTextColor(hdc)==0.
    static void EnsureListViewTextColorOnDc(HDC hdc) noexcept
    {
        if (!hdc)
            return;
        if ((GetTextColor(hdc) & 0xFFFFFF) == 0)
            SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
    }

    // BWM-паритет: иконка целиком, без вырезания фона — все пиксели непрозрачные.
    // Promise32BPP для 32bpp-источников сохраняет сырой 4-й байт (у strip-битмапов
    // ImageList там мусор), поэтому альфу принудительно поднимаем в 0xFF.
    static void ForceOpaqueAlpha(HBITMAP hbmp32) noexcept
    {
        DIBSECTION ds{};
        if (!GetObject(hbmp32, sizeof(ds), &ds) || !ds.dsBm.bmBits)
            return;
        auto* pixels = static_cast<DWORD*>(ds.dsBm.bmBits);
        const int count = ds.dsBm.bmWidth * std::abs(ds.dsBm.bmHeight);
        for (int i = 0; i < count; ++i)
            pixels[i] |= 0xFF000000u;
    }

    // sysmon → LV: при memory-DC WindowFromDC=NULL — HWND с вершины стека PaintDispatcher (как Root MMC).
    [[nodiscard]] static HWND HwndForSysmonListViewDraw(HDC hdc) noexcept
    {
        if (!hdc)
            return nullptr;
        HWND w = WindowFromDC(hdc);
        WCHAR cls[128]{};
        if (w)
        {
            if (GetClassNameW(w, cls, _countof(cls)) > 0 && StrEq(cls, L"SysListView32"))
                return w;
            return nullptr;
        }
        w = GlassCore::PaintDispatcher::GetPaintSessionHostHwnd();
        if (!w || !IsWindow(w))
            return nullptr;
        if (GetClassNameW(w, cls, _countof(cls)) > 0 && StrEq(cls, L"SysListView32"))
            return w;
        return nullptr;
    }

    // Black-key / настроенный chroma — общий код в GdiUtils::ApplyIconBackgroundAlpha
    // (Root\Control\ListView\ColorTreatAsTransparent; без ключа — black-key как раньше).

    // =========================================================================
    //  OVERALL DISPATCHER MAP
    // =========================================================================

    int      WINAPI Overall_MyDrawThemeBg(HTHEME, HDC, int, int, LPCRECT, LPCRECT);
    HRESULT  WINAPI Overall_MyDrawThemeTextEx(HTHEME, HDC, int, int, LPCWSTR, int, DWORD, LPRECT, const DTTOPTS*);
    BOOL     WINAPI Overall_MyExtTextOutW(HDC, int, int, UINT, const RECT*, LPCWSTR, UINT, const INT*);
    BOOL     WINAPI Overall_MyFillRect(HDC, const RECT*, HBRUSH);
    int      WINAPI Overall_MyDrawTextW(HDC, LPCWSTR, int, LPRECT, UINT);
    BOOL     WINAPI Overall_MyBitBlt(HDC, int, int, int, int, HDC, int, int, DWORD);
    BOOL     WINAPI Overall_MyStretchBlt(HDC, int, int, int, int, HDC, int, int, int, int, DWORD);
    BOOL     WINAPI Overall_MyMaskBlt(HDC, int, int, int, int, HDC, int, int, HBITMAP, int, int, DWORD);
    COLORREF WINAPI Overall_MySetBkColor(HDC, COLORREF);
    HBRUSH   WINAPI Overall_MyCreateSolidBrush(COLORREF);
    BOOL     WINAPI Overall_MyLineTo(HDC, int, int);
    BOOL     WINAPI Overall_MyPolyline(HDC, const POINT*, int);
    int      WINAPI Overall_MyDrawTextExW(HDC, LPWSTR, int, LPRECT, UINT, LPDRAWTEXTPARAMS);

    enum OverallHookIndex : size_t
    {
        OHK_DRAWTHEMEBG = 0,
        OHK_EXTTEXTOUT,
        OHK_FILLRECT,
        OHK_DRAWTEXT,
        OHK_BITBLT,
        OHK_STRETCHBLT,
        OHK_MASKBLT,
        OHK_SETBKCOLOR,
        OHK_CREATESOLIDBRUSH,
        OHK_POLYLINE,
        OHK_LINETO,
        OHK_DRAWTHEMETEXTEX,
        OHK_DRAWTEXTEX,
        OHK_COUNT
    };

    inline const HookDispatcherDependency<OHK_COUNT, 4> g_overallHookDep
    { {
        // OHK_DRAWTHEMEBG
        std::tuple{ std::array{ "uxtheme.dll"sv, "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv, ""sv, ""sv },
            "DrawThemeBackground", reinterpret_cast<PVOID>(Overall_MyDrawThemeBg) },
        // OHK_EXTTEXTOUT
        std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
            "ExtTextOutW", reinterpret_cast<PVOID>(Overall_MyExtTextOutW) },
        // OHK_FILLRECT
        std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv, ""sv },
            "FillRect", reinterpret_cast<PVOID>(Overall_MyFillRect) },
        // OHK_DRAWTEXT
        std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv, ""sv },
            "DrawTextW", reinterpret_cast<PVOID>(Overall_MyDrawTextW) },
        // OHK_BITBLT
        std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
            "BitBlt", reinterpret_cast<PVOID>(Overall_MyBitBlt) },
        // OHK_STRETCHBLT
        std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
            "StretchBlt", reinterpret_cast<PVOID>(Overall_MyStretchBlt) },
        // OHK_MASKBLT
        std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
            "MaskBlt", reinterpret_cast<PVOID>(Overall_MyMaskBlt) },
        // OHK_SETBKCOLOR
        std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
            "SetBkColor", reinterpret_cast<PVOID>(Overall_MySetBkColor) },
        // OHK_CREATESOLIDBRUSH
        std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
            "CreateSolidBrush", reinterpret_cast<PVOID>(Overall_MyCreateSolidBrush) },
        // OHK_POLYLINE (sysmon.ocx → SysListView32)
        std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
            "Polyline", reinterpret_cast<PVOID>(Overall_MyPolyline) },
        // OHK_LINETO (sysmon.ocx → SysListView32)
        std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
            "LineTo", reinterpret_cast<PVOID>(Overall_MyLineTo) },
        // OHK_DRAWTHEMETEXTEX (themed ListView: взаимоисключение с OHK_DRAWTEXTEX — см. EnableListViewHooksForModule)
        std::tuple{ std::array{ "uxtheme.dll"sv, "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv, ""sv, ""sv },
            "DrawThemeTextEx", reinterpret_cast<PVOID>(Overall_MyDrawThemeTextEx) },
        std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv, ""sv },
            "DrawTextExW", reinterpret_cast<PVOID>(Overall_MyDrawTextExW) },
    } };

    using OverallDispatcher = HookDispatcher<OHK_COUNT, 4>;

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

    // ── Предпрогрев IAT-кеша (только чтение: emplace + CacheHookData, без записей).
    // Холодный скан PE уходит из первого paint-кадра: в Startup (уже загруженные)
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
            DebugLog(DBG_NORMAL, L"[GlassCore][ListView] Startup: cached 0x%p", mod);
        }
    }

    // Только свои модули — чужие не кешируем (карта не раздувается).
    // Имя сравниваем с базовым (регистр не важен); EXE — по хэндлу точки входа.
    static void PrewarmForModule(HMODULE mod, const wchar_t* base) noexcept
    {
        if (!mod || !base) return;
        if (mod == GlassCore::ModuleHandler::g_entryPointModule)
        {
            PrewarmModule(mod);
            return;
        }
        if (_wcsicmp(base, L"comctl32.dll") != 0 && _wcsicmp(base, L"uxtheme.dll") != 0
            && _wcsicmp(base, L"sysmon.ocx") != 0 && _wcsicmp(base, L"SHELL32.dll") != 0)
            return;
        PrewarmModule(mod);
    }

    // ── DisableAllOverallHooks ───────────────────────────────────────────────
    static void DisableAllOverallHooks()
    {
        auto lock = g_overallLock.lock_exclusive();
        for (auto& [addr, dispatcher] : g_overallMap)
            dispatcher.DisableAllHooks();
        g_overallMap.clear();
    }

    // Выгрузка comctl32: снять IAT и выбросить stale-запись (как ComboLBox —
    // держать кеш выгруженного модуля значит use-after-free при следующем Enable).
    void OnComctl32ModuleUnload(HMODULE unloaded) noexcept
    {
        if (!unloaded)
            return;
        try
        {
            auto lock = g_overallLock.lock_exclusive();
            auto it = g_overallMap.find(static_cast<void*>(unloaded));
            if (it != g_overallMap.end())
            {
                it->second.DisableAllHooks();
                g_overallMap.erase(it);
                DebugLog(L"[GlassCore][ListView] comctl32=0x%p unloaded, overall entry dropped", unloaded);
            }
        }
        catch (...) {}
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

    // Модуль собственного образа — детект chain-bounce через IATRegistry:
    // предыдущее звено цепочки вызвало свой orig-трамплин, и _ReturnAddress()
    // лежит в нашем коде, а не у настоящего вызывающего.
    static HMODULE OwnGlassCoreModule() noexcept
    {
        static HMODULE s_own = nullptr;
        if (!s_own)
        {
            HMODULE h = nullptr;
            GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&Overall_MyBitBlt),
                &h);
            s_own = h;
        }
        return s_own;
    }

    // Настоящий comctl-caller сквозь chain-bounce: первый кадр стека вне
    // собственного образа, принадлежащий отслеживаемому comctl32.
    // Кадры наших звеньев (свой модуль) и трамплинов (не модули) пропускаем.
    // Только для различения BWM-связки «сессия legacy / вызовы new» внутри
    // LV-сессии; вне сессий не вызывается.
    [[nodiscard]] static HMODULE FindTrueComctlCaller() noexcept
    {
        void* stack[24]{};
        const USHORT n = CaptureStackBackTrace(
            0, static_cast<DWORD>(std::size(stack)), stack, nullptr);
        auto* g_new = GlassCore::ModuleHandler::g_comctl32New;
        auto* g_legacy = GlassCore::ModuleHandler::g_comctl32Legacy;
        const HMODULE own = OwnGlassCoreModule();
        for (USHORT i = 0; i < n; ++i)
        {
            HMODULE m = nullptr;
            if (!GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(stack[i]), &m) || !m)
                continue;
            if (own && m == own)
                continue;
            if ((g_new && m == g_new) || (g_legacy && m == g_legacy))
                return m;
        }
        return nullptr;
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
    // Тема "ListView" (iPartId < 2, iStateId < 2): PatBlt(BLACKNESS) — прозрачный фон.
    // Дополнительно: iPartId == 1, iStateId == 1 (hot-tracking item) → BLACKNESS.
    // Тема "ScrollBar": делегируем в IAT-цепочку (ComctlScrollBarMyDrawThemeBackground).
    // BWM pattern: OverallHooks.cpp lines 598-601, 627-631.
    int WINAPI Overall_MyDrawThemeBg(
        HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
        LPCRECT pRect, LPCRECT pClipRect)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_DRAWTHEMEBG, decltype(&DrawThemeBackground)>(callerMod);

        if (!orig)
            return DrawThemeBackground(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

        if (g_shutdown.load(std::memory_order_acquire))
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

        WCHAR themeClass[MAX_PATH + 1]{};
        const HRESULT hrThemeClass =
            Shared::ThemeHelper::GetThemeClass(hTheme, themeClass, MAX_PATH);
        if (SUCCEEDED(hrThemeClass) && StrEq(themeClass, L"ScrollBar"))
        {
            auto sbOrig = GlassCore::ScrollBarTheme::GetComctlScrollBarDrawThemeOrig(callerMod);
            auto dtbOrig = sbOrig ? sbOrig : orig;
            if (!dtbOrig)
                return DrawThemeBackground(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

            GlassCore::ScrollBarDrawTheme::LogScrollBarDrawThemeCall(
                L"[GlassCore][ListView]",
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

        if (g_lvSessionStack.empty())
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

        if (SUCCEEDED(hrThemeClass))
        {
            if (pRect && !IsRectEmpty(pRect))
            {
                const RECT* clipRect = pClipRect ? pClipRect : pRect;

                if (StrEq(themeClass, L"ListView"))
                {
                    // Фон контрола и горячая подсветка элемента
                    if ((iPartId < 2 && iStateId < 2) || (iPartId == 1 && iStateId == 1))
                    {
                        PatBlt(hdc,
                            clipRect->left, clipRect->top,
                            clipRect->right  - clipRect->left,
                            clipRect->bottom - clipRect->top,
                            BLACKNESS);
                        return S_OK;
                    }
                }
            }
        }

        return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
    }

    // ── ExtTextOutW ──────────────────────────────────────────────────────────
    // Основной путь отрисовки текста элементов ListView.
    // BWM pattern: ListView::WorkExtTextOutW (WinHooks.cpp 814-873).
    BOOL WINAPI Overall_MyExtTextOutW(
        HDC hdc, int x, int y, UINT options, const RECT* lprect,
        LPCWSTR lpString, UINT c, const INT* lpDx)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_EXTTEXTOUT, decltype(&ExtTextOutW)>(callerMod);

        if (!orig)
            return ExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);

        if (g_lvSessionStack.empty())
            return orig(hdc, x, y, options, lprect, lpString, c, lpDx);

        if (options & ETO_OPAQUE && lprect)
        {
            PatBlt(hdc, lprect->left, lprect->top,
                   lprect->right - lprect->left, lprect->bottom - lprect->top,
                   BLACKNESS);

            COLORREF color = GetBkColor(hdc);
            if (color && color != GetSysColor(COLOR_WINDOW)
                      && !(color == GetSysColor(COLOR_BTNFACE)
                           && (lprect->right - lprect->left) > 1
                           && (lprect->bottom - lprect->top) > 1))
            {
                wil::unique_hbrush brush{ Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(color, 255) };
                if (brush)
                    FillRect(hdc, lprect, brush.get());
            }
        }

        // Как в BWM ListView::WorkExtTextOutW: служебные режимы не вести в DrawTextWithAlpha.
        if (options == ETO_GLYPH_INDEX
            || options == ETO_IGNORELANGUAGE
            || (options == ETO_OPAQUE && !c)
            || !c)
        {
            if (options != ETO_OPAQUE)
                options &= ~static_cast<UINT>(ETO_OPAQUE);
            if (options == ETO_OPAQUE || !c)
                return TRUE;
            return orig(hdc, x, y, options, lprect, lpString, c, lpDx);
        }

        if (options & ETO_GLYPH_INDEX)
            return orig(hdc, x, y, options, lprect, lpString, c, lpDx);

        RECT clip{};
        GetClipBox(hdc, &clip);
        clip.left = x;
        clip.top  = y;
        DebugLog(DBG_IMPORTANT, L"[GlassCore][ListView] ExtTextOutW: text=\"%.*s\" clip=[%d,%d,%d,%d] options=0x%08X callerMod=%p",
			c, lpString ? lpString : L"", clip.left, clip.top, clip.right, clip.bottom, options, callerMod);
        EnsureListViewTextColorOnDc(hdc);
        int result = 0;
        if (!FAILED(GlassCore::Text::DrawTextWithAlpha(
                hdc, lpString, static_cast<int>(c), &clip,
                DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOCLIP, result)))
            return TRUE;

        return orig(hdc, x, y, options, lprect, lpString, c, lpDx);
    }

    // ── FillRect ─────────────────────────────────────────────────────────────
    // Перехват заливки фона: сравниваем ЦВЕТ кисти через LOGBRUSH, а не хэндл.
    // BWM pattern: OverallHooks.cpp FillRect hook.
    //   COLOR_WINDOW → PatBlt(BLACKNESS) (прозрачный фон)
    //   чёрный (0)   → пропускаем (уже прозрачный)
    //   другой цвет  → заменяем кисть на alpha-aware версию
    // Гистограмма заливок снята (финиш 2026-10-13).

    BOOL WINAPI Overall_MyFillRect(HDC hdc, const RECT* lprc, HBRUSH hbr)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_FILLRECT, decltype(&FillRect)>(callerMod);

        if (!orig)
            return FillRect(hdc, lprc, hbr);

        if (g_lvSessionStack.empty())
            return orig(hdc, lprc, hbr);

        if (!lprc || !hbr)
            return orig(hdc, lprc, hbr);

        LOGBRUSH lb{};
        if (GetObject(hbr, sizeof(lb), &lb) && lb.lbStyle == BS_SOLID)
        {
            const int rcW = lprc->right - lprc->left;
            const int rcH = lprc->bottom - lprc->top;
            const int band = GetScrollBandSizeForDpiCached(hdc);

            // Угол пересечения скроллов: квадрат COLOR_SCROLLBAR со стороной полосы прокрутки.
            // Для устойчивости на дробном масштабировании допускаем +/-1 px.
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

            // Фоновый цвет окна → PatBlt(BLACKNESS) для прозрачности на стекле
            if (lb.lbColor == GetSysColor(COLOR_WINDOW))
            {
                PatBlt(hdc,
                    lprc->left, lprc->top,
                    lprc->right  - lprc->left,
                    lprc->bottom - lprc->top,
                    BLACKNESS);
                return TRUE;
            }

            // Чёрный (0) — уже прозрачный, не трогаем
            if (!lb.lbColor)
                return orig(hdc, lprc, hbr);

            // Любой другой цвет — заменяем на alpha-aware кисть
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

    // ── DrawTextW ────────────────────────────────────────────────────────────
    // Путь отрисовки текста групп и заголовков ListView.
    int WINAPI Overall_MyDrawTextW(
        HDC hdc, LPCWSTR lpchText, int cchText, LPRECT lprc, UINT format)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_DRAWTEXT, decltype(&DrawTextW)>(callerMod);

        if (!orig)
            return DrawTextW(hdc, lpchText, cchText, lprc, format);

        if (g_lvSessionStack.empty())
            return orig(hdc, lpchText, cchText, lprc, format);

        if (format & DT_CALCRECT)
            return orig(hdc, lpchText, cchText, lprc, format);

        if (!lprc || IsRectEmpty(lprc) || !lpchText || cchText == 0)
            return orig(hdc, lpchText, cchText, lprc, format);
		DebugLog(DBG_IMPORTANT, L"[GlassCore][ListView] DrawTextW: text=\"%.*s\" rect=[%d,%d,%d,%d] format=0x%08X callerMod=%p",
			cchText, lpchText ? lpchText : L"", lprc->left, lprc->top, lprc->right, lprc->bottom, format, callerMod);
        if (GetBkMode(hdc) == OPAQUE)
        {
            PatBlt(hdc, lprc->left, lprc->top,
                   lprc->right - lprc->left, lprc->bottom - lprc->top, BLACKNESS);
        }

        EnsureListViewTextColorOnDc(hdc);
        int result = 0;
        if (!FAILED(GlassCore::Text::DrawTextWithAlpha(hdc, lpchText, cchText, lprc, format, result)))
            return result;

        return orig(hdc, lpchText, cchText, lprc, format);
    }

    // ── DrawThemeTextEx (themed SysListView32; как TreeView — DTT_COMPOSITED + DrawThemeContent)
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

        if (g_lvSessionStack.empty())
            return orig(hTheme, hdc, iPartId, iStateId, pszText, cchText, dwTextFlags, pRect, pOptions);

        WCHAR themeClass[MAX_PATH + 1]{};
        if (!SUCCEEDED(Shared::ThemeHelper::GetThemeClass(hTheme, themeClass, MAX_PATH))
            || !StrEq(themeClass, L"ListView"))
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

    int WINAPI Overall_MyDrawTextExW(
        HDC hdc, LPWSTR lpchText, int cchText,
        LPRECT lprc, UINT format, LPDRAWTEXTPARAMS lpdtp)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_DRAWTEXTEX, decltype(&DrawTextExW)>(callerMod);
        if (!orig)
            return DrawTextExW(hdc, lpchText, cchText, lprc, format, lpdtp);
        if (g_lvSessionStack.empty())
            return orig(hdc, lpchText, cchText, lprc, format, lpdtp);
        // Замер (DT_CALCRECT) ничего не рисует — альфа-рендер за ~750мкс
        // на вызов здесь чистый перерасход: отдаём оригиналу.
        if (format & DT_CALCRECT)
            return orig(hdc, lpchText, cchText, lprc, format, lpdtp);
        if (lpdtp)
            return orig(hdc, lpchText, cchText, lprc, format, lpdtp);
        EnsureListViewTextColorOnDc(hdc);
        int result = 0;
        if (!FAILED(GlassCore::Text::DrawTextWithAlpha(hdc, lpchText, cchText, lprc, format, result)))
            return result;
        return orig(hdc, lpchText, cchText, lprc, format, lpdtp);
    }

    // =========================================================================
    //  BitBlt / StretchBlt / MaskBlt — отрисовка иконок ListView
    // =========================================================================
    //
    // Иконки без альфа-канала: Promise32BPP конвертирует в 32bpp (alpha=0xFF),
    // затем GdiAlphaBlend рисует с корректным compositing поверх стекла.
    //
    // BWM-паритет ТОЛЬКО для связки «сессия рисования в legacy, вызовы BitBlt
    // из new» (BWM-ветка callerModule == OverallHooks::GetComctl32Module() +
    // WinHooks::HK_BITBLT): иконка целиком, без вырезания фона — opaque 32bpp +
    // оригинальный BitBlt. Настоящий caller сквозь chain-bounce восстанавливает
    // FindTrueComctlCaller (проход стека до первого отслеживаемого comctl32).
    // Вызов через BitBlt (а не AlphaBlend) означает, что надёжной альфы
    // у битмапа нет (настоящую альфу comctl рисует через AlphaBlend — та же
    // логика, что в TreeView Overall_MyMaskBlt): проверять HasAlphaChannel
    // в этой ветке нельзя — в 32bpp strip-битмапах ImageList альфа-позиция
    // мусорная и даёт ложное true → просвечивание через AlphaBlend, а black-key
    // дополнительно выедает тёмные пиксели иконки.
    // Остальные связки идут общим путём ниже (как до правки): обычный BitBlt
    // игнорирует альфу, поэтому там GdiAlphaBlend (AC_SRC_OVER + AC_SRC_ALPHA).

    // ── BitBlt ──────────────────────────────────────────────────────────────
    BOOL WINAPI Overall_MyBitBlt(
        HDC hdc, int x, int y, int cx, int cy,
        HDC hdcSrc, int x1, int y1, DWORD rop)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_BITBLT, decltype(&BitBlt)>(callerMod);

        // Chain-bounce через IATRegistry (голова — ComctlScrollBarMyBitBlt,
        // зарегистрирован позже базового LV-ref и вызывает свой orig-трамплин):
        // _ReturnAddress() лежит в нашем GlassCore.dll, настоящий caller
        // потерян, orig пуст. Настоящий caller восстанавливаем проходом
        // стека до первого отслеживаемого comctl32; не нашли — берём модуль
        // инициатора paint из владеющей LV-сессии (у него запись есть).
        // LV зарегистрирован базовым ref раньше ленивого SB → LV.orig ведёт
        // на настоящий BitBlt, рекурсии нет при любом порядке звеньев
        // (вызов всегда идёт вниз по цепочке к оригиналу).
        if (!orig && !g_lvSessionStack.empty())
        {
            const HMODULE own = OwnGlassCoreModule();
            if (own && callerMod == own)
            {
                HMODULE trueCaller = FindTrueComctlCaller();
                if (!trueCaller)
                    trueCaller = g_lvSessionStack.back().mod;
                callerMod = trueCaller;
                orig = GetOverallOrigSafe<OHK_BITBLT, decltype(&BitBlt)>(callerMod);
            }
        }

        if (!orig)
            return BitBlt(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);

        // BitBlt из comctl32 внутри uxtheme DrawThemeBackground(ScrollBar) при непустом стеке LV
        // ошибочно обрабатывается как иконка (GdiAlphaBlend) — стрелки/ползунок пропадают.
        if (GlassCore::ScrollBarTheme::IsInsideScrollBarDrawThemeOrig())
            return orig(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);

        if (g_lvSessionStack.empty())
            return orig(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);

        // Обрабатываем только SRCCOPY — другие rop-коды не нуждаются в alpha-фиксе
        if (rop != SRCCOPY || !cx || !cy || !hdcSrc)
        {
            /*DebugLog(DBG_NORMAL, L"[GlassCore][ListView] BitBlt SKIP: rop=0x%08X cx=%d cy=%d hdcSrc=%p",
                rop, cx, cy, hdcSrc);*/
            return orig(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);
        }

        // Получаем текущий битмап из источника
        HBITMAP hSrcBitmap = static_cast<HBITMAP>(GetCurrentObject(hdcSrc, OBJ_BITMAP));
        if (!hSrcBitmap || Shared::BitmapUtils::IsOemBitmap(hSrcBitmap))
        {
            /*DebugLog(DBG_NORMAL, L"[GlassCore][ListView] BitBlt SKIP: hSrcBitmap=%p isOem=%d",
                hSrcBitmap, hSrcBitmap ? Shared::BitmapUtils::IsOemBitmap(hSrcBitmap) : -1);*/
            return orig(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);
        }

        // Получаем информацию о битмапе для логирования
        BITMAP bm{};
        GetObject(hSrcBitmap, sizeof(bm), &bm);
        bool hasAlpha = Shared::BitmapUtils::HasAlphaChannel(hSrcBitmap);

        // BWM-паритет (см. шапку секции): ТОЛЬКО точная связка из ветки
        // callerModule == OverallHooks::GetComctl32Module() + WinHooks::HK_BITBLT:
        // сессия рисования в legacy, вызовы BitBlt из new — иконка целиком,
        // без вырезания фона: opaque 32bpp + оригинальный BitBlt.
        // Остальные связки (new/new, legacy/legacy, WinForms, чужие caller)
        // идут общим путём ниже — там фоны были прозрачными и до правки;
        // opaque целиком корректен только в этой связке (фон гарантированно
        // чёрный, как у BWM; поверх темизированных фонов он дал бы боксы).
        auto* g_parityNew = GlassCore::ModuleHandler::g_comctl32New;
        if (g_parityNew &&
            g_lvSessionStack.back().kind == GlassShared::GlassControlKind::ListView &&
            g_lvSessionStack.back().mod == GlassCore::ModuleHandler::g_comctl32Legacy &&
            callerMod == g_parityNew)
        {
            wil::unique_hbitmap hConverted{ Shared::BitmapUtils::Promise32BPP(hSrcBitmap) };
            if (!hConverted)
            {
                return orig(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);
            }

            ForceOpaqueAlpha(hConverted.get());

            HGDIOBJ hOldBitmap = SelectObject(hdcSrc, hConverted.get());
            auto restoreBitmap = wil::scope_exit([&] { SelectObject(hdcSrc, hOldBitmap); });

            return orig(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);
        }

        /*DebugLog(DBG_IMPORTANT,
            L"[GlassCore][ListView] BitBlt: x=%d y=%d cx=%d cy=%d srcX=%d srcY=%d "
            L"bmp=%dx%d bpp=%d hasAlpha=%d callerMod=%p kind=%d",
            x, y, cx, cy, x1, y1,
            bm.bmWidth, bm.bmHeight, bm.bmBitsPixel, hasAlpha,
            callerMod, static_cast<int>(g_lvSessionStack.back().kind));*/

        // Если битмап содержит альфа-канал — рисуем через AlphaBlend
        if (hasAlpha)
        {
            //DebugLog(DBG_NORMAL, L"[GlassCore][ListView] BitBlt → GdiAlphaBlend (hasAlpha)");
            BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            return GdiAlphaBlend(hdc, x, y, cx, cy, hdcSrc, x1, y1, cx, cy, bf);
        }

        // Нет альфа — black-key transparency.
        // Конвертируем в 32bpp, чёрные пиксели (фон ImageList) → alpha=0,
        // цветные пиксели (рисунок) → alpha=255. Затем AlphaBlend.
        wil::unique_hbitmap hConverted{ Shared::BitmapUtils::Promise32BPP(hSrcBitmap) };
        if (!hConverted)
        {
            //DebugLog(DBG_WARNING, L"[GlassCore][ListView] BitBlt: Promise32BPP FAILED, fallback to orig");
            return orig(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);
        }

        GlassCore::GdiUtils::ApplyIconBackgroundAlpha(hConverted.get(), g_lvIconRemoval);

        //DebugLog(DBG_NORMAL, L"[GlassCore][ListView] BitBlt → BlackKeyAlpha + GdiAlphaBlend (bpp=%d)", bm.bmBitsPixel);

        HGDIOBJ hOldBitmap = SelectObject(hdcSrc, hConverted.get());
        auto restoreBitmap = wil::scope_exit([&] { SelectObject(hdcSrc, hOldBitmap); });

        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        return GdiAlphaBlend(hdc, x, y, cx, cy, hdcSrc, x1, y1, cx, cy, bf);
    }

    // ── StretchBlt ──────────────────────────────────────────────────────────
    BOOL WINAPI Overall_MyStretchBlt(
        HDC hdcDest, int xDest, int yDest, int wDest, int hDest,
        HDC hdcSrc, int xSrc, int ySrc, int wSrc, int hSrc, DWORD rop)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_STRETCHBLT, decltype(&StretchBlt)>(callerMod);

        if (!orig)
            return StretchBlt(hdcDest, xDest, yDest, wDest, hDest,
                              hdcSrc, xSrc, ySrc, wSrc, hSrc, rop);

        if (GlassCore::ScrollBarTheme::IsInsideScrollBarDrawThemeOrig())
            return orig(hdcDest, xDest, yDest, wDest, hDest,
                        hdcSrc, xSrc, ySrc, wSrc, hSrc, rop);

        if (g_lvSessionStack.empty())
            return orig(hdcDest, xDest, yDest, wDest, hDest,
                        hdcSrc, xSrc, ySrc, wSrc, hSrc, rop);

        if (rop != SRCCOPY || !wSrc || !hSrc || !wDest || !hDest || !hdcSrc)
        {
            /*DebugLog(DBG_NORMAL, L"[GlassCore][ListView] StretchBlt SKIP: rop=0x%08X src=%dx%d dst=%dx%d",
                rop, wSrc, hSrc, wDest, hDest);*/
            return orig(hdcDest, xDest, yDest, wDest, hDest,
                        hdcSrc, xSrc, ySrc, wSrc, hSrc, rop);
        }

        HBITMAP hSrcBitmap = static_cast<HBITMAP>(GetCurrentObject(hdcSrc, OBJ_BITMAP));
        if (!hSrcBitmap || Shared::BitmapUtils::IsOemBitmap(hSrcBitmap))
        {
            /*DebugLog(DBG_NORMAL, L"[GlassCore][ListView] StretchBlt SKIP: hSrcBitmap=%p isOem=%d",
                hSrcBitmap, hSrcBitmap ? Shared::BitmapUtils::IsOemBitmap(hSrcBitmap) : -1);*/
            return orig(hdcDest, xDest, yDest, wDest, hDest,
                        hdcSrc, xSrc, ySrc, wSrc, hSrc, rop);
        }

        BITMAP bm{};
        GetObject(hSrcBitmap, sizeof(bm), &bm);
        bool hasAlpha = Shared::BitmapUtils::HasAlphaChannel(hSrcBitmap);

        /*DebugLog(DBG_IMPORTANT,
            L"[GlassCore][ListView] StretchBlt: dst=(%d,%d %dx%d) src=(%d,%d %dx%d) "
            L"bmp=%dx%d bpp=%d hasAlpha=%d callerMod=%p kind=%d",
            xDest, yDest, wDest, hDest, xSrc, ySrc, wSrc, hSrc,
            bm.bmWidth, bm.bmHeight, bm.bmBitsPixel, hasAlpha,
            callerMod, static_cast<int>(g_lvSessionStack.back().kind));*/

        if (hasAlpha)
        {
            //DebugLog(DBG_NORMAL, L"[GlassCore][ListView] StretchBlt → GdiAlphaBlend (hasAlpha)");
            BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            return GdiAlphaBlend(hdcDest, xDest, yDest, wDest, hDest,
                                 hdcSrc, xSrc, ySrc, wSrc, hSrc, bf);
        }

        // Нет альфа — black-key transparency + AlphaBlend
        wil::unique_hbitmap hConverted{ Shared::BitmapUtils::Promise32BPP(hSrcBitmap) };
        if (!hConverted)
        {
            //DebugLog(DBG_WARNING, L"[GlassCore][ListView] StretchBlt: Promise32BPP FAILED, fallback to orig");
            return orig(hdcDest, xDest, yDest, wDest, hDest,
                        hdcSrc, xSrc, ySrc, wSrc, hSrc, rop);
        }

        GlassCore::GdiUtils::ApplyIconBackgroundAlpha(hConverted.get(), g_lvIconRemoval);

        //DebugLog(DBG_NORMAL, L"[GlassCore][ListView] StretchBlt → BlackKeyAlpha + GdiAlphaBlend (bpp=%d)", bm.bmBitsPixel);

        HGDIOBJ hOldBitmap = SelectObject(hdcSrc, hConverted.get());
        auto restoreBitmap = wil::scope_exit([&] { SelectObject(hdcSrc, hOldBitmap); });

        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        return GdiAlphaBlend(hdcDest, xDest, yDest, wDest, hDest,
                             hdcSrc, xSrc, ySrc, wSrc, hSrc, bf);
    }

    // ── MaskBlt ─────────────────────────────────────────────────────────────
    // MaskBlt использует битовую маску для определения прозрачности —
    // заменять на GdiAlphaBlend нельзя. Подменяем только source bitmap
    // для обеспечения alpha=0xFF, маска сама определит прозрачность.
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

        if (g_lvSessionStack.empty())
            return orig(hdcDest, xDest, yDest, width, height,
                        hdcSrc, xSrc, ySrc, hbmMask, xMask, yMask, rop);

        if (!width || !height || !hdcSrc)
            return orig(hdcDest, xDest, yDest, width, height,
                        hdcSrc, xSrc, ySrc, hbmMask, xMask, yMask, rop);

        HBITMAP hSrcBitmap = static_cast<HBITMAP>(GetCurrentObject(hdcSrc, OBJ_BITMAP));
        if (!hSrcBitmap || Shared::BitmapUtils::IsOemBitmap(hSrcBitmap))
        {
            /*DebugLog(DBG_NORMAL, L"[GlassCore][ListView] MaskBlt SKIP: hSrcBitmap=%p isOem=%d",
                hSrcBitmap, hSrcBitmap ? Shared::BitmapUtils::IsOemBitmap(hSrcBitmap) : -1);*/
            return orig(hdcDest, xDest, yDest, width, height,
                        hdcSrc, xSrc, ySrc, hbmMask, xMask, yMask, rop);
        }

        BITMAP bm{};
        GetObject(hSrcBitmap, sizeof(bm), &bm);
        bool hasAlpha = Shared::BitmapUtils::HasAlphaChannel(hSrcBitmap);

        BITMAP bmMask{};
        if (hbmMask)
            GetObject(hbmMask, sizeof(bmMask), &bmMask);

        /*DebugLog(DBG_IMPORTANT,
            L"[GlassCore][ListView] MaskBlt: dst=(%d,%d %dx%d) src=(%d,%d) "
            L"bmp=%dx%d bpp=%d hasAlpha=%d mask=%p maskBpp=%d rop=0x%08X callerMod=%p kind=%d",
            xDest, yDest, width, height, xSrc, ySrc,
            bm.bmWidth, bm.bmHeight, bm.bmBitsPixel, hasAlpha,
            hbmMask, bmMask.bmBitsPixel, rop,
            callerMod, static_cast<int>(g_lvSessionStack.back().kind));*/

        // Оригинальный MaskBlt в любом случае — маска обрабатывает прозрачность.
        // Для hasAlpha=true маска корректно работает с альфой.
        // Для hasAlpha=false маска определяет прозрачные области через ROP.
        //DebugLog(DBG_NORMAL, L"[GlassCore][ListView] MaskBlt → orig (hasAlpha=%d, bpp=%d)", hasAlpha, bm.bmBitsPixel);
        return orig(hdcDest, xDest, yDest, width, height,
                    hdcSrc, xSrc, ySrc, hbmMask, xMask, yMask, rop);
    }

    // ── SetBkColor ──────────────────────────────────────────────────────────
    // WinForms ListView (.SysListView32): фон всегда прозрачный (чёрный).
    // BWM pattern: WinHooks.cpp HK_SETBKCOLOR.
    COLORREF WINAPI Overall_MySetBkColor(HDC hdc, COLORREF color)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_SETBKCOLOR, decltype(&SetBkColor)>(callerMod);

        if (!orig)
            return SetBkColor(hdc, color);

        if (g_lvSessionStack.empty())
            return orig(hdc, color);

        // Принудительно чёрный фон — прозрачность на стекле
        return orig(hdc, 0);
    }

    // ── CreateSolidBrush ────────────────────────────────────────────────────
    // Перехват создания кистей — заменяем на alpha-aware версию.
    // BWM pattern: OverallHooks.cpp CreateSolidBrush.
    HBRUSH WINAPI Overall_MyCreateSolidBrush(COLORREF color)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_CREATESOLIDBRUSH, decltype(&CreateSolidBrush)>(callerMod);

        if (!orig)
            return CreateSolidBrush(color);

        if (g_lvSessionStack.empty())
            return orig(color);

        // Чёрный цвет — прозрачный фон, не трогаем
        if (color == 0)
            return orig(0);

        // BWM-паритет (источник чёрного фона LV): comctl создаёт фоновые кисти
        // через CreateSolidBrush — белые/фейсовые сразу отдаём чёрными, чтобы
        // сам comctl красил фон с первого paint (а не post-BLACKNESS).
        // GetSysColor здесь настоящий (чужой сессии нет).
        const COLORREF plain = color & 0xFFFFFF;
        if (plain == 0xFFFFFF || plain == (GetSysColor(COLOR_BTNFACE) & 0xFFFFFF))
            return orig(0);

        // Создаём кисть с альфа-каналом для корректного композитинга
        HBRUSH alphaBrush = Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(color, 255);
        return alphaBrush ? alphaBrush : orig(color);
    }

    // ── LineTo: sysmon.ocx → SysListView32, альфа-перо как Root MMC (LayeredStrokePen) ──
    BOOL WINAPI Overall_MyLineTo(HDC hdc, int x, int y)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_LINETO, decltype(&LineTo)>(callerMod);

        if (!orig)
            return LineTo(hdc, x, y);
        if (g_shutdown.load(std::memory_order_acquire) || g_lvSessionStack.empty())
            return orig(hdc, x, y);
        if (g_lvSessionStack.back().kind != GlassShared::GlassControlKind::ListView)
            return orig(hdc, x, y);
        if (!CallerBaseIs(callerMod, L"sysmon.ocx"))
            return orig(hdc, x, y);

        if (!HwndForSysmonListViewDraw(hdc))
            return orig(hdc, x, y);

        HGDIOBJ penObj = GetCurrentObject(hdc, OBJ_PEN);
        LOGPEN  lp{};
        if (GetObject(penObj, sizeof(lp), &lp) != sizeof(LOGPEN) || lp.lopnStyle == PS_NULL)
            return orig(hdc, x, y);

        POINT lppt{};
        if (!GetCurrentPositionEx(hdc, &lppt))
            return orig(hdc, x, y);

        GdiUtils::LayeredStrokePen layeredStroke =
            GdiUtils::LayeredStrokePen::TryCreateSolid(lp, kSysmonListViewStrokeAlpha);
        wil::unique_hpen drawPen;
        if (!layeredStroke)
            drawPen = GdiUtils::PenFromLogPen(lp);
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

    // ── Polyline: sysmon.ocx в DC SysListView32 во время сессии PaintDispatcher (как DRAWTEXT/ExtTextOut) ──
    BOOL WINAPI Overall_MyPolyline(HDC hdc, const POINT* p, int n)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_POLYLINE, decltype(&Polyline)>(callerMod);

        if (!orig)
            return Polyline(hdc, p, n);
        if (g_shutdown.load(std::memory_order_acquire) || g_lvSessionStack.empty())
            return orig(hdc, p, n);
        if (g_lvSessionStack.back().kind != GlassShared::GlassControlKind::ListView)
            return orig(hdc, p, n);
        if (!CallerBaseIs(callerMod, L"sysmon.ocx"))
            return orig(hdc, p, n);
        if (n <= 0 || !p)
            return orig(hdc, p, n);

        if (!HwndForSysmonListViewDraw(hdc))
            return orig(hdc, p, n);

        HGDIOBJ penObj = GetCurrentObject(hdc, OBJ_PEN);
        LOGPEN  lp{};
        if (GetObject(penObj, sizeof(lp), &lp) != sizeof(LOGPEN) || lp.lopnStyle == PS_NULL)
            return orig(hdc, p, n);

        GdiUtils::LayeredStrokePen layeredStroke =
            GdiUtils::LayeredStrokePen::TryCreateSolid(lp, kSysmonListViewStrokeAlpha);
        wil::unique_hpen drawPen;
        if (!layeredStroke)
            drawPen = GdiUtils::PenFromLogPen(lp);
        HGDIOBJ hStroke = layeredStroke ? static_cast<HGDIOBJ>(layeredStroke.get())
                                        : static_cast<HGDIOBJ>(drawPen.get());
        if (!hStroke)
            return orig(hdc, p, n);

        HGDIOBJ old = SelectObject(hdc, hStroke);
        const BOOL r = orig(hdc, p, n);
        SelectObject(hdc, old);
        return r;
    }

    // =========================================================================
    //  EnableListViewHooksForModule — включить/выключить хуки по типу контрола
    //  и вызывающему модулю
    // =========================================================================
    //
    // BWM PaintHandler.cpp:158-238 — определяет набор хуков по:
    //   1. Класс окна (SysListView32 / .SysListView32)
    //   2. callerModule (g_comctl32New / g_comctl32Legacy)
    //   3. Дополнительные модули (sysmon.ocx, SHELL32.dll)

    static void EnableListViewHooksForModule(
        HWND hWnd,
        HMODULE callerMod,
        GlassShared::GlassControlKind kind,
        bool enable)
    {
        using namespace GlassShared;
        auto* g_new    = GlassCore::ModuleHandler::g_comctl32New;
        auto* g_legacy = GlassCore::ModuleHandler::g_comctl32Legacy;

        if (kind == GlassControlKind::ListView)
        {
            // ── SysListView32 ─────────────────────────────────────────────
            if (callerMod == g_new)
            {
                // Основной набор хуков на новом comctl32
                EnableHooksOnModule(g_new, OHK_EXTTEXTOUT,      enable);
                EnableHooksOnModule(g_new, OHK_DRAWTEXT,        enable);
                EnableHooksOnModule(g_new, OHK_FILLRECT,        enable);
                EnableHooksOnModule(g_new, OHK_STRETCHBLT,      enable);
                EnableHooksOnModule(g_new, OHK_BITBLT,          enable);
                EnableHooksOnModule(g_new, OHK_MASKBLT,         enable);

                // Текст themed ListView: как EnableTreeViewHooksForModule (glow → DrawTextEx на uxtheme; иначе DTTEx на g_new).
                if (hWnd && Shared::WinApiUtils::IsThemed(hWnd, L"ListView"))
                {
                    EnableHooksOnModule(g_new, OHK_DRAWTHEMEBG, enable);
                    const bool glow = GlassCore::Text::IsInkGlowEffectActiveForPaintSession(hWnd);
                    HMODULE hUx = GetModuleHandleW(L"uxtheme.dll");
                    if (!glow || (!hUx && glow))
                        EnableHooksOnModule(g_new, OHK_DRAWTHEMETEXTEX, enable);
                    else
                        EnableHooksOnModule(hUx, OHK_DRAWTEXTEX, enable);
                }

                // Кросс-модуль: BitBlt на legacy comctl32
                if (g_legacy)
                    EnableHooksOnModule(g_legacy, OHK_BITBLT, enable);
            }
            else if (callerMod == g_legacy)
            {
                // Подмножество хуков на legacy comctl32
                EnableHooksOnModule(g_legacy, OHK_FILLRECT,   enable);
                EnableHooksOnModule(g_legacy, OHK_DRAWTEXT,   enable);
                EnableHooksOnModule(g_legacy, OHK_EXTTEXTOUT, enable);
                EnableHooksOnModule(g_legacy, OHK_BITBLT,     enable);

                // Кросс-модуль: BitBlt на новом comctl32
                if (g_new)
                    EnableHooksOnModule(g_new, OHK_BITBLT, enable);
            }

            // sysmon.ocx — как BWM WndList: IAT sysmon только если BeginPaint вызван из comctl32.
            if (PVOID sysmon = GetModuleHandleW(L"sysmon.ocx");
                sysmon && (callerMod == g_new || callerMod == g_legacy))
            {
                EnableHooksOnModule(sysmon, OHK_DRAWTEXT,        enable);
                EnableHooksOnModule(sysmon, OHK_EXTTEXTOUT,      enable);
                EnableHooksOnModule(sysmon, OHK_CREATESOLIDBRUSH, enable);
                EnableHooksOnModule(sysmon, OHK_POLYLINE,       enable);
                EnableHooksOnModule(sysmon, OHK_LINETO,         enable);
            }

            // SHELL32.dll — текст в shell-представлениях
            PVOID shell32 = GetModuleHandleW(L"SHELL32.dll");
            if (shell32)
                EnableHooksOnModule(shell32, OHK_DRAWTEXT, enable);
        }
        else if (kind == GlassControlKind::WinForms_ListView)
        {
            // ── .SysListView32 (WinForms) ─────────────────────────────────
            if (callerMod == g_new)
            {
                EnableHooksOnModule(g_new, OHK_FILLRECT,   enable);
                EnableHooksOnModule(g_new, OHK_DRAWTEXT,   enable);
                EnableHooksOnModule(g_new, OHK_SETBKCOLOR, enable);
                EnableHooksOnModule(g_new, OHK_BITBLT,     enable);

                // Кросс-модуль: BitBlt на legacy comctl32
                if (g_legacy)
                    EnableHooksOnModule(g_legacy, OHK_BITBLT, enable);
            }

            // CreateSolidBrush comctl — только Root (lifetime, ModuleHandler).
        }
    }

    // =========================================================================
    //  PaintDispatcher provider callbacks
    // =========================================================================

    static void OnBeginPaintGlobal(HWND hWnd, HMODULE callerMod)
    {
        using namespace GlassShared;
        auto kind = GlassGetWindowKind(hWnd);
        if (kind != GlassControlKind::ListView && kind != GlassControlKind::WinForms_ListView)
            return;

        // Гарантируем чёрный (прозрачный) фон перед BeginPaint/WM_ERASEBKGND.
        // LVM_* сообщения валидны только для SysListView32, не для SysHeader32.
        // FIX repaint-storm: слать только при смене значения — comctl инвалидирует
        // список на каждый LVM_SETBKCOLOR, превращая paint в самоподдержку (60fps).
        bool diagFlipped = false;
        if (kind == GlassControlKind::ListView)
        {
            // Дефолтный bk сбрасываем в чёрный (прозрачный фон).
            const COLORREF diagBk = ListView_GetBkColor(hWnd);
            const COLORREF diagTBk = ListView_GetTextBkColor(hWnd);
            if (diagBk != 0)
            {
                ListView_SetBkColor(hWnd, 0);
                diagFlipped = true;
            }
            if (diagTBk != 0)
            {
                ListView_SetTextBkColor(hWnd, 0);
                diagFlipped = true;
            }
        }
        GlassCore::ScrollBarTheme::EnsureComctlScrollBarIatIfComctlCaller(hWnd, callerMod);

        GlassCore::TextGlow::EnsureTextGlowClientRegistered(hWnd);

        if (g_lvSessionStack.empty())
            GlassCore::Settings::QueryIconBackgroundColorRemovalContext(
                { L"Root", L"Control", L"ListView" }, g_lvIconRemoval);
        // Гейт post-BLACKNESS: чернить только первый paint окна либо paint
        // после реального BkFlip (дефолт мог просвечивать). Чернить каждый
        // paint — стробоскоп при шторме; у BWM такого механизма нет, фон там
        // красит сам comctl (LVM bk=0 + системные цвета).
        if (g_lvBgPrimed.size() > 512)
            g_lvBgPrimed.clear(); // защита от роста на recycled HWND
        const bool firstPaint = g_lvBgPrimed.insert(hWnd).second;

        g_lvSessionStack.push_back({ callerMod, kind, hWnd, diagFlipped || firstPaint });
        DebugLog(DBG_NORMAL, L"[GlassCore][ListView] BeginPaint: callerMod=0x%p hWnd=0x%p kind=%d depth=%zu",
            callerMod, hWnd, static_cast<int>(kind), g_lvSessionStack.size());
        EnableListViewHooksForModule(hWnd, callerMod, kind, true);
    }

    static void OnEndPaintGlobal(HWND hWnd, HMODULE /*callerMod*/)
    {
        using namespace GlassShared;
        auto kind = GlassGetWindowKind(hWnd);
        if (kind != GlassControlKind::ListView && kind != GlassControlKind::WinForms_ListView)
            return;
        if (g_lvSessionStack.empty()) return;

        const ListViewPaintSession s = g_lvSessionStack.back();
        g_lvSessionStack.pop_back();
        EnableListViewHooksForModule(s.hWnd ? s.hWnd : hWnd, s.mod, s.kind, false);
        DebugLog(DBG_NORMAL, L"[GlassCore][ListView] EndPaint: hWnd=0x%p depth=%zu", hWnd, g_lvSessionStack.size());
    }

    // Post-BeginPaint: разовая зачистка фона (первый paint окна либо после
    // BkFlip). Диспетчер гасит fErase по возврату true. Каждый paint не
    // черним: при шторме это стробоскоп blackout→контент.
    [[nodiscard]] static bool OnListViewAfterBeginPaint(
        HWND hWnd, HDC hdc, LPPAINTSTRUCT ps, HMODULE /*callerMod*/) noexcept
    {
        if (g_shutdown.load(std::memory_order_acquire))
            return false;

        using namespace GlassShared;
        const GlassControlKind kind = GlassGetWindowKind(hWnd);
        if (kind != GlassControlKind::ListView && kind != GlassControlKind::WinForms_ListView)
            return false;

        if (!hdc || !ps || IsRectEmpty(&ps->rcPaint))
            return false;

        if (g_lvSessionStack.empty() || !g_lvSessionStack.back().needBlackout)
            return false; // фон уже чернили: не стробим, fErase не трогаем
        g_lvSessionStack.back().needBlackout = false;

        PatBlt(hdc,
            ps->rcPaint.left, ps->rcPaint.top,
            ps->rcPaint.right - ps->rcPaint.left,
            ps->rcPaint.bottom - ps->rcPaint.top,
            BLACKNESS);
        return true;
    }

    // =========================================================================
    //  Lifecycle
    // =========================================================================

    void Prepare()  {}

    void Startup()
    {
        GlassCore::PaintDispatcher::RegisterPaintProvider({ OnBeginPaintGlobal, OnEndPaintGlobal });
        GlassCore::PaintDispatcher::RegisterPostPaintProvider({ OnListViewAfterBeginPaint });

        PrewarmModule(GlassCore::ModuleHandler::g_comctl32New);
        PrewarmModule(GlassCore::ModuleHandler::g_comctl32Legacy);
        PrewarmModule(GlassCore::ModuleHandler::g_entryPointModule);
        PrewarmModule(GetModuleHandleW(L"uxtheme.dll"));
        PrewarmModule(GetModuleHandleW(L"SHELL32.dll"));
        PrewarmModule(GetModuleHandleW(L"sysmon.ocx"));
        GlassCore::ModuleHandler::RegisterModulePrewarmHandler(PrewarmForModule);

        // BitBlt на comctl32: базовая регистрация слота (ListView — обработчик иконок;
        // ScrollBar: DTB IAT — DirectUIHooks; BitBlt — ScrollBarTheme::ComctlScrollBarMyBitBlt; глубина orig — ScrollBarTheme).
        auto baselineBitBlt = [&](void* mod)
        {
            if (!mod)
                return;
            EnableHooksOnModule(mod, OHK_BITBLT, true);
        };
        baselineBitBlt(GlassCore::ModuleHandler::g_comctl32New);
        baselineBitBlt(GlassCore::ModuleHandler::g_comctl32Legacy);
        DebugLog(DBG_IMPORTANT, L"[GlassCore][ListView] Startup: базовый ref BitBlt на comctl32");
        // CreateSolidBrush comctl — только Root (lifetime, ModuleHandler).
    }

    void Shutdown()
    {
        g_shutdown.store(true, std::memory_order_seq_cst);
        DisableAllOverallHooks();
        g_lvSessionStack.clear();
        g_shutdown.store(false, std::memory_order_seq_cst);
    }
}
