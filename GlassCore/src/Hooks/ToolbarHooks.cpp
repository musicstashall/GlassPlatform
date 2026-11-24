// Хуки для отрисовки ToolbarWindow32.
//
// Архитектура (PaintDispatcher + per-module g_overallMap):
//
// Toolbar: текст — DrawThemeText на comctl32;
//   glow — DrawTextExW на uxtheme.dll.
// ToolbarWindow32: MaskBlt / BitBlt (gdi32 IAT в comctl32):
//   32bpp+alpha → AlphaBlend; иначе black-key + AlphaBlend.
//   1bpp-маска ImageList: skip после alpha_direct; orig после blackkey.
// Exact TToolBar (VCL): также caller EXE; временная opaque-копия источника
//   и настоящий comctl32!MaskBlt с неизменными маской/ROP. BitBlt — native.
//   Подписи кнопок при caller EXE рисует тот же comctl32 v6: его DrawThemeText
//   (и DrawTextExW uxtheme при glow) берутся на время сессии по имени модуля.
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
#include <Subsystems/OpaqueBitmapSource.h>
#include <Subsystems/Settings.h>
#include <GlassControlKind.h>
#include <GlassBackdropMark.h>
#include <Subsystems/RootSwcaGradient.h>
#include <BitmapUtils.h>
#include <wil.h>
#include <CommCtrl.h>
#include <uxtheme.h>
#include <WinApiUtils.h>
#include <array>
#include <vector>

#pragma comment(lib, "msimg32.lib")

using namespace GlassCore::Hooks;
using namespace std::literals;

namespace GlassCore::ToolbarControl
{
    // =========================================================================
    //  Thread-local: стек paint-сессий (LIFO) — зеркало DC в PaintDispatcher.
    // Симметричный Enable/Disable на каждый Begin/End; глубину IAT ведёт HookDispatcher::hookRef.
    // Guard в телах хуков: !g_modStack.empty() — активная paint-сессия Toolbar (не путать с hookRef).
    // =========================================================================

    struct PaintSession
    {
        HWND hwnd{};
        HMODULE caller{};
        bool vclIcons{};
        // Снимок только реально полученных refs. Даже если второй comctl32
        // загрузится во время paint, End освободит ровно модули из Begin.
        std::array<HMODULE, 2> iconModules{};
        // EXE paint-route VCL: BeginPaint вызван кодом EXE (TWinControl.PaintHandler),
        // а подписи кнопок рисует comctl32 v6. Хуки текста, которые comctl32-маршрут
        // получает по caller, здесь берутся по имени модуля; снимок — для End.
        HMODULE textModule{};   // comctl32 v6: DrawThemeText
        HMODULE glowModule{};   // uxtheme: DrawTextExW (только при glow)
        unsigned texts{};       // подписей, нарисованных хуком в этой сессии
    };
    thread_local std::vector<PaintSession> g_modStack;
    thread_local bool g_inOpaqueIcon = false;

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
            hWnd = GlassCore::PaintDispatcher::GetStackOwnerForDc(hdc);
        if (!hWnd)
            hWnd = GlassCore::PaintDispatcher::GetPaintSessionHostHwnd();
        return IsToolbarGlassSessionAllowed(hWnd);
    }

    [[nodiscard]] static bool IsVclToolbarWindow(HWND hwnd) noexcept
    {
        wchar_t name[64]{};
        return IsWindow(hwnd) && GetClassNameW(hwnd, name, static_cast<int>(_countof(name)))
            && StrEqI(name, L"TToolBar");
    }

    // BWM ctrl + caller EXE: exact TToolBar рисует ImageList через comctl32,
    // хотя BeginPaint вызван VCL-кодом EXE. Не распространять на формы/панели.
    [[nodiscard]] static bool IsVclToolbarIconSession(HWND hwnd, HMODULE caller) noexcept
    {
        const HMODULE exe = GlassCore::ModuleHandler::g_entryPointModule;
        return exe && caller && GlassCore::Root::IsFullInjectionMode() && IsVclToolbarWindow(hwnd)
            && GlassShared::GlassIsBackdropAllowedForHwnd(hwnd)
            && reinterpret_cast<HMODULE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)) == exe
            && (caller == exe || caller == GlassCore::ModuleHandler::g_comctl32New
                || caller == GlassCore::ModuleHandler::g_comctl32Legacy);
    }

    [[nodiscard]] static bool IsVclToolbarIconDc(HDC dc, HMODULE caller) noexcept
    {
        if (g_shutdown.load(std::memory_order_acquire) || g_inOpaqueIcon
            || !dc || g_modStack.empty() || GetMapMode(dc) != MM_TEXT) return false;
        const auto& s = g_modStack.back();
        if (!s.vclIcons || (caller != s.iconModules[0] && caller != s.iconModules[1])
            || !IsVclToolbarIconSession(s.hwnd, s.caller)
            || !GlassCore::PaintDispatcher::IsTopWindowDrawingSession(s.hwnd)
            || !GlassCore::PaintDispatcher::IsPaintStackDcForHwnd(s.hwnd, dc)) return false;
        const HWND owner = WindowFromDC(dc);
        return owner == s.hwnd || (!owner && GetObjectType(dc) == OBJ_MEMDC
            && GlassCore::PaintDispatcher::GetStackOwnerForDc(dc) == s.hwnd);
    }

    [[nodiscard]] static bool TakeVclImageDiag() noexcept
    {
        static thread_local ULONGLONG epoch{};
        static thread_local unsigned budget{};
        const auto now = GetTickCount64();
        if (!epoch || now - epoch >= 1000) { epoch = now; budget = 12; }
        if (!budget) return false;
        --budget; return true;
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

        const int before = it->second.GetRef(index);
        it->second.EnableHook(index, enable);
        // false-возврат EnableHook(true) означает также вложенное увеличение hookRef.
        return enable ? it->second.GetRef(index) > before : it->second.GetRef(index) < before;
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
        {
            ++g_modStack.back().texts;
            return S_OK;
        }

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
    // TToolBar: непрозрачная копия + настоящий MaskBlt, как у BWM. Маска и
    // ROP определяют покрытие, чёрные детали иконки не являются color-key.
    // Остальные toolbar сохраняют ранее принятый alpha/black-key renderer.
    BOOL WINAPI Overall_MyMaskBlt(
        HDC hdcDest, int xDest, int yDest, int width, int height,
        HDC hdcSrc, int xSrc, int ySrc,
        HBITMAP hbmMask, int xMask, int yMask, DWORD rop)
    {
        const DWORD entryError = GetLastError();
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_MASKBLT, decltype(&MaskBlt)>(callerMod);

        if (!orig)
            return MaskBlt(hdcDest, xDest, yDest, width, height,
                           hdcSrc, xSrc, ySrc, hbmMask, xMask, yMask, rop);

        if (g_modStack.empty())
            return orig(hdcDest, xDest, yDest, width, height,
                        hdcSrc, xSrc, ySrc, hbmMask, xMask, yMask, rop);

        if (g_modStack.back().vclIcons)
        {
            const auto native = [&] {
                return orig(hdcDest, xDest, yDest, width, height,
                    hdcSrc, xSrc, ySrc, hbmMask, xMask, yMask, rop);
            };
            if (!IsVclToolbarIconDc(hdcDest, callerMod) || width <= 0 || height <= 0
                || !hdcSrc || hdcSrc == hdcDest
                || GetCurrentObject(hdcSrc, OBJ_BITMAP) == GetCurrentObject(hdcDest, OBJ_BITMAP))
            { SetLastError(entryError); return native(); }
            g_inOpaqueIcon = true;
            auto leave = wil::scope_exit([]() noexcept { g_inOpaqueIcon = false; });
            BOOL result{}; DWORD error{}; bool prepared{};
            {
                GlassCore::GdiUtils::OpaqueBitmapSource image{hdcSrc};
                prepared = static_cast<bool>(image);
                SetLastError(entryError);
                result = native(); // ровно один native draw, в том числе при ошибке подготовки
                error = GetLastError();
            }
            if (TakeVclImageDiag())
                DebugLog(DBG_NORMAL,
                    L"[GlassCore][TToolBar] MaskBlt dc=0x%p src=0x%p dst=[%d,%d,%d,%d] srcXY=[%d,%d] mask=0x%p maskXY=[%d,%d] rop=0x%08X opaqueCopy=%d result=%d",
                    hdcDest, hdcSrc, xDest, yDest, width, height, xSrc, ySrc,
                    hbmMask, xMask, yMask, rop, prepared ? 1 : 0, result);
            SetLastError(error);
            return result;
        }

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

        // VCL-маршрут нормализует только MaskBlt. Его 1bpp/ROP-проходы
        // остаются native, в том числе если этот BitBlt включён внешней сессией.
        if (g_modStack.back().vclIcons)
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

    static void EnableToolbarHooksForModule(HWND hWnd, HMODULE callerMod, bool enable, bool vclIcons)
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

            if (!vclIcons)
            {
                EnableHooksOnModule(g_new, OHK_MASKBLT, enable);
                EnableHooksOnModule(g_new, OHK_BITBLT, enable);
            }
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
        if (g_shutdown.load(std::memory_order_acquire)) return;
        using namespace GlassShared;
        if (GlassGetWindowKind(hWnd) != GlassControlKind::Toolbar)
            return;

        GlassCore::TextGlow::EnsureTextGlowClientRegistered(hWnd);

        if (g_modStack.empty())
            GlassCore::Settings::QueryIconBackgroundColorRemovalContext(
                { L"Root", L"Control", L"Toolbar" }, g_toolbarIconRemoval);

        PaintSession session{};
        session.hwnd = hWnd; session.caller = callerMod;
        // Exact-класс задаёт и native-барьер. Неавторизованная VCL-сессия
        // не должна попасть в generic alpha/black-key через внешний IAT-ref.
        session.vclIcons = IsVclToolbarWindow(hWnd);
        if (session.vclIcons && IsVclToolbarIconSession(hWnd, callerMod))
        {
            const std::array<HMODULE, 2> modules{GlassCore::ModuleHandler::g_comctl32New,
                GlassCore::ModuleHandler::g_comctl32Legacy};
            for (size_t i = 0; i < modules.size(); ++i)
                if (modules[i] && (i == 0 || modules[i] != modules[0])
                    && EnableHooksOnModule(modules[i], OHK_MASKBLT, true))
                    session.iconModules[i] = modules[i];
            // Подписи. Маршрут с caller comctl32 v6 получает те же хуки ниже, в
            // EnableToolbarHooksForModule; второй ref на одну сессию не берётся.
            if (callerMod == GlassCore::ModuleHandler::g_entryPointModule && modules[0]
                && EnableHooksOnModule(modules[0], OHK_DRAWTHEMETEXT, true))
            {
                session.textModule = modules[0];
                HMODULE const ux = GetModuleHandleW(L"uxtheme.dll");
                if (ux && GlassCore::Text::IsInkGlowEffectActiveForPaintSession(hWnd)
                    && EnableHooksOnModule(ux, OHK_DRAWTEXTEX, true))
                    session.glowModule = ux;
            }
        }
        // Даже без полученного MaskBlt-ref запись служит native-барьером:
        // process-wide IAT внешней сессии не авторизует этот paint.
        g_modStack.push_back(session);
        EnableToolbarHooksForModule(hWnd, callerMod, true, session.vclIcons);

        DebugLog(DBG_NORMAL,
            L"[GlassCore][Toolbar] Begin: callerMod=0x%p hWnd=0x%p depth=%zu vclIcons=%d maskModules=[0x%p,0x%p] textModule=0x%p",
            callerMod, hWnd, g_modStack.size(), session.vclIcons ? 1 : 0,
            session.iconModules[0], session.iconModules[1], session.textModule);
    }

    static void OnEndGlobal(HWND hWnd, HMODULE /*callerMod*/)
    {
        if (g_modStack.empty() || g_modStack.back().hwnd != hWnd) return;
        const PaintSession session = g_modStack.back();
        g_modStack.pop_back();
        EnableToolbarHooksForModule(hWnd, session.caller, false, session.vclIcons);
        if (session.glowModule)
            EnableHooksOnModule(session.glowModule, OHK_DRAWTEXTEX, false);
        if (session.textModule)
            EnableHooksOnModule(session.textModule, OHK_DRAWTHEMETEXT, false);
        for (size_t i = session.iconModules.size(); i > 0; --i)
            if (session.iconModules[i-1])
                EnableHooksOnModule(session.iconModules[i-1], OHK_MASKBLT, false);
        if (g_modStack.empty())
            g_toolbarSuppressMaskBitBlt = false;
        DebugLog(DBG_NORMAL, L"[GlassCore][Toolbar] End: hWnd=0x%p depth=%zu text=%u", hWnd, g_modStack.size(), session.texts);
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
