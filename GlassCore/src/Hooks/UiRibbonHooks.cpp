
#include "CorePch.h"
#include <Hooks/UiRibbonHooks.h>
#include <GlassControlKind.h>
#include <Subsystems/PaintDispatcher.h>
#include <Core/Intercept/HookDispatcher.h>
#include <Core/Intercept/IATRegistry.h>
#include <Module/ModuleHandler.h>
#include <Subsystems/RenderText.h>
#include <Subsystems/GdiHookUtils.h>
#include <Subsystems/Settings.h>
#include <MenuUtils.h>
#include <ColorUtils.h>

#include <atomic>
#include <mutex>
#include <unordered_map>

using namespace GlassCore::Hooks;
using namespace std::literals;

namespace GlassCore::UiRibbon
{
    static std::atomic<bool> g_shutdown{ false };

    static thread_local int  g_netUiPaintDepth = 0;
    // HWND из внешнего BeginPaint NetUI (0→1): для GetSysColorBrush — только под NetUiToolWindow.
    static thread_local HWND s_tlsNetUiPaintSurfaceHwnd = nullptr;
    // Удаление фона иконок ленты (Root\Control\Ribbon) — читается один раз на внешний BeginPaint.
    static thread_local WndCompat::Menu::IconBackgroundColorRemoval s_tlsRibbonIconRemoval{};

    // Номер внешнего BeginPaint (depth 0→1) — корреляция DrawText / BitBlt в одном кадре.
    static std::atomic<uint32_t> g_uiRibPaintSeq{ 0 };

    // Лимиты на один такой кадр (thread_local сбрасывается в OnBeginPaint при depth==0).
    static constexpr unsigned kUiRibDrawTextLogCalcMax = 12;   // DT_CALCRECT — шумная фаза
    static constexpr unsigned kUiRibDrawTextLogDrawMax = 96;   // реальная отрисовка

    struct UiRibDrawTextLogFrame final
    {
        uint32_t paintSeq = 0;
        unsigned calcLogged = 0;
        unsigned drawLogged = 0;
    };

    static thread_local UiRibDrawTextLogFrame s_drawTextLogFrame{};

    // Диагностика GdiGradientFill (BWM KbxLabelHooks): лимит строк в LogViewer.
    static std::atomic<int> g_diagGdiGradientFillLogBudget{ 48 };

    // NetUIHWND и popup ленты (NetUiToolWindow): одна модель сессии IAT в BeginPaint/EndPaint.
    [[nodiscard]] static bool IsNetUiRibbonSurface(HWND hWnd) noexcept
    {
        using namespace GlassShared;
        const GlassControlKind k = GlassGetWindowKind(hWnd);
        return k == GlassControlKind::NetUI || k == GlassControlKind::NetUiToolWindow;
    }

    // Подсказка ленты: NetUIHWND связан с NetUiToolWindow по цепочке родителя и/или GW_OWNER
    // (не полоса команд — у неё нет такого предка/владельца).
    [[nodiscard]] static bool IsNetUiSurfaceUnderToolWindow(HWND hWnd) noexcept
    {
        if (!hWnd)
            return false;
        using namespace GlassShared;
        for (HWND cur = hWnd; cur; cur = GetParent(cur))
        {
            if (GlassGetWindowKind(cur) == GlassControlKind::NetUiToolWindow)
                return true;
            for (HWND own = GetWindow(cur, GW_OWNER); own; own = GetWindow(own, GW_OWNER))
            {
                if (GlassGetWindowKind(own) == GlassControlKind::NetUiToolWindow)
                    return true;
            }
        }
        return false;
    }

    // Активная сессия IAT + поверхность подсказки (GetSysColorBrush / SetDCBrushColor / GdiGradientFill).
    [[nodiscard]] static bool IsNetUiRibbonTooltipPaintSession() noexcept
    {
        return g_netUiPaintDepth > 0
            && s_tlsNetUiPaintSurfaceHwnd
            && IsNetUiSurfaceUnderToolWindow(s_tlsNetUiPaintSurfaceHwnd);
    }

    [[nodiscard]] static HMODULE GetCallerModule(void* ret) noexcept
    {
        HMODULE hMod = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(ret),
            &hMod);
        return hMod;
    }

    // Строгая проверка: базовый адрес должен быть именно UIRibbon.dll (имя файла, без пути).
    [[nodiscard]] static bool ModuleFileBaseNameIs(HMODULE mod, LPCWSTR expectedFile) noexcept
    {
        if (!mod || !expectedFile)
            return false;
        wchar_t path[MAX_PATH]{};
        if (!GetModuleFileNameW(mod, path, MAX_PATH))
            return false;
        const wchar_t* base = wcsrchr(path, L'\\');
        base = base ? base + 1 : path;
        return _wcsicmp(base, expectedFile) == 0;
    }

    [[nodiscard]] static HMODULE ResolveVerifiedUiRibbonModule() noexcept
    {
        HMODULE rib = GetModuleHandleW(L"UIRibbon.dll");
        if (!rib || !ModuleFileBaseNameIs(rib, L"UIRibbon.dll"))
            return nullptr;
        return rib;
    }

    // ── UI Ribbon dispatcher (IAT внутри UIRibbon.dll) ───────────────────────

    int WINAPI UiRib_MyDrawTextW(HDC, LPCWSTR, int, LPRECT, UINT);
    BOOL WINAPI UiRib_MyStretchBlt(HDC, int, int, int, int, HDC, int, int, int, int, DWORD);
    BOOL WINAPI UiRib_MyBitBlt(HDC, int, int, int, int, HDC, int, int, DWORD);
    HGDIOBJ WINAPI UiRib_MyGetStockObject(int);
    HBRUSH WINAPI UiRib_MyGetSysColorBrush(int);
    COLORREF WINAPI UiRib_MySetDCBrushColor(HDC, COLORREF);
    BOOL WINAPI UiRib_MyGdiGradientFill(HDC, PTRIVERTEX, ULONG, PVOID, ULONG, ULONG);

    enum UiRibHookIndex : size_t
    {
        URH_DRAWTEXT = 0,
        URH_STRETCHBLT,
        URH_BITBLT,
        URH_GETSTOCKOBJECT,
        URH_GETSYSCOLORBRUSH,
        URH_SETDCBRUSHCOLOR,
        URH_GDIGRADIENTFILL,
        URH_COUNT
    };

    // GDI: как в Button/Static — на части сборок импорт идёт в gdi32full.dll, не в gdi32.dll;
    // без gdi32full в списке IAT-слот не находится и хук (в т.ч. SetDCBrushColor) не срабатывает.
    inline const HookDispatcherDependency<URH_COUNT, 4> g_uiRibHookDep
    { {
        std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv, ""sv },
            "DrawTextW", reinterpret_cast<PVOID>(UiRib_MyDrawTextW) },
        std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
            "StretchBlt", reinterpret_cast<PVOID>(UiRib_MyStretchBlt) },
        std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
            "BitBlt", reinterpret_cast<PVOID>(UiRib_MyBitBlt) },
        std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
            "GetStockObject", reinterpret_cast<PVOID>(UiRib_MyGetStockObject) },
        std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv, ""sv },
            "GetSysColorBrush", reinterpret_cast<PVOID>(UiRib_MyGetSysColorBrush) },
        std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
            "SetDCBrushColor", reinterpret_cast<PVOID>(UiRib_MySetDCBrushColor) },
        std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
            "GdiGradientFill", reinterpret_cast<PVOID>(UiRib_MyGdiGradientFill) },
    } };

    using UiRibDispatcher = HookDispatcher<URH_COUNT, 4>;

    static wil::srwlock                               g_uiRibLock;
    static std::unordered_map<void*, UiRibDispatcher> g_uiRibMap;

    template <size_t idx, typename T>
    [[nodiscard]] static T GetUiRibOrg(HMODULE callerMod) noexcept
    {
        auto lock = g_uiRibLock.lock_shared();
        auto it = g_uiRibMap.find(static_cast<void*>(callerMod));
        if (it != g_uiRibMap.end())
            return it->second.template GetOrg<idx, T>();
        return nullptr;
    }

    static bool EnableUiRibHook(void* baseAddress, size_t index, bool enable) noexcept
    {
        if (!baseAddress)
            return false;
        if (!ModuleFileBaseNameIs(static_cast<HMODULE>(baseAddress), L"UIRibbon.dll"))
            return false;

        auto lock = g_uiRibLock.lock_exclusive();
        auto it = g_uiRibMap.find(baseAddress);
        if (it == g_uiRibMap.end())
        {
            auto [ins, ok] = g_uiRibMap.emplace(
                std::piecewise_construct,
                std::forward_as_tuple(baseAddress),
                std::forward_as_tuple(g_uiRibHookDep, baseAddress));
            it = ins;
            it->second.CacheHookData();
        }
        return it->second.EnableHook(index, enable);
    }

    static void DisableAllUiRibHooks() noexcept
    {
        auto lock = g_uiRibLock.lock_exclusive();
        for (auto& [_, d] : g_uiRibMap)
            d.DisableAllHooks();
        g_uiRibMap.clear();
    }

    int WINAPI UiRib_MyDrawTextW(
        HDC hdc, LPCWSTR lpchText, int cchText, LPRECT lprc, UINT format)
    {
        const HMODULE callerDraw = GetCallerModule(_ReturnAddress());
        if (g_shutdown.load(std::memory_order_acquire))
        {
            auto o = GetUiRibOrg<URH_DRAWTEXT, decltype(&DrawTextW)>(callerDraw);
            return o ? o(hdc, lpchText, cchText, lprc, format) : DrawTextW(hdc, lpchText, cchText, lprc, format);
        }
        auto orig = GetUiRibOrg<URH_DRAWTEXT, decltype(&DrawTextW)>(callerDraw);
        if (!orig)
        {
            /*DebugLog(DBG_WARNING,
                L"[GlassCore][UiRibbon] MyDrawTextW: org=null hdc=0x%p caller=0x%p depth=%d — IAT не в карте или не включён",
                hdc, callerDraw, g_netUiPaintDepth);*/
            return DrawTextW(hdc, lpchText, cchText, lprc, format);
        }

        // BWM UIRibbonHook::MyDrawTextW
        if ((format & DT_CALCRECT) != 0)
        {
            if (s_drawTextLogFrame.calcLogged < kUiRibDrawTextLogCalcMax)
            {
                ++s_drawTextLogFrame.calcLogged;
                /*DebugLog(DBG_NORMAL,
                    L"[GlassCore][UiRibbon] MyDrawTextW: paint#%u calc#%u DT_CALCRECT hdc=0x%p depth=%d "
                    L"surf=0x%p tooltip=%d caller=0x%p cch=%d fmt=0x%X",
                    s_drawTextLogFrame.paintSeq,
                    s_drawTextLogFrame.calcLogged,
                    hdc,
                    g_netUiPaintDepth,
                    s_tlsNetUiPaintSurfaceHwnd,
                    IsNetUiRibbonTooltipPaintSession() ? 1 : 0,
                    callerDraw,
                    cchText,
                    format);*/
            }
            return orig(hdc, lpchText, cchText, lprc, format);
        }

        if (s_drawTextLogFrame.drawLogged < kUiRibDrawTextLogDrawMax)
        {
            ++s_drawTextLogFrame.drawLogged;
            /*DebugLog(DBG_NORMAL,
                L"[GlassCore][UiRibbon] MyDrawTextW: paint#%u draw#%u hdc=0x%p depth=%d surf=0x%p "
                L"tooltip=%d caller=0x%p cch=%d fmt=0x%X",
                s_drawTextLogFrame.paintSeq,
                s_drawTextLogFrame.drawLogged,
                hdc,
                g_netUiPaintDepth,
                s_tlsNetUiPaintSurfaceHwnd,
                IsNetUiRibbonTooltipPaintSession() ? 1 : 0,
                callerDraw,
                cchText,
                format);*/
        }
        else if (s_drawTextLogFrame.drawLogged == kUiRibDrawTextLogDrawMax)
        {
            ++s_drawTextLogFrame.drawLogged;
            /*DebugLog(DBG_WARNING,
                L"[GlassCore][UiRibbon] MyDrawTextW: paint#%u — достигнут лимит %u логов отрисовки за кадр",
                s_drawTextLogFrame.paintSeq,
                kUiRibDrawTextLogDrawMax);*/
        }

        int result = 0;
        const HRESULT hr = GlassCore::Text::DrawTextWithAlpha(hdc, lpchText, cchText, lprc, format, result);
        if (!FAILED(hr))
            return result;
        /*DebugLog(DBG_WARNING,
            L"[GlassCore][UiRibbon] MyDrawTextW: paint#%u hdc=0x%p DrawTextWithAlpha hr=0x%08X → orig",
            s_drawTextLogFrame.paintSeq,
            hdc,
            hr);*/
        return orig(hdc, lpchText, cchText, lprc, format);
    }

    BOOL WINAPI UiRib_MyStretchBlt(
        HDC hdcDest, int xDest, int yDest, int wDest, int hDest,
        HDC hdcSrc, int xSrc, int ySrc, int wSrc, int hSrc, DWORD rop)
    {
        auto caller = GetCallerModule(_ReturnAddress());
        auto orig = GetUiRibOrg<URH_STRETCHBLT, decltype(&StretchBlt)>(caller);
        if (!orig)
        {
            /*DebugLog(DBG_WARNING,
                L"[GlassCore][UiRibbon] MyStretchBlt: org=null hdcDst=0x%p hdcSrc=0x%p caller=0x%p",
                hdcDest, hdcSrc, caller);*/
            return StretchBlt(hdcDest, xDest, yDest, wDest, hDest, hdcSrc, xSrc, ySrc, wSrc, hSrc, rop);
        }
        // BWM: caller != Api::GetCurrentModule() → BLACKNESS
        if (!g_shutdown.load(std::memory_order_acquire)
            && caller != GlassCore::ModuleHandler::g_entryPointModule)
            rop = BLACKNESS;

        /*DebugLog(DBG_NORMAL,
            L"[GlassCore][UiRibbon] MyStretchBlt: hdcDst=0x%p hdcSrc=0x%p dest=(%d,%d,%d,%d) src=(%d,%d,%d,%d) rop=0x%X caller=0x%p",
            hdcDest,
            hdcSrc,
            xDest,
            yDest,
            wDest,
            hDest,
            xSrc,
            ySrc,
            wSrc,
            hSrc,
            rop,
            caller);*/

        return orig(hdcDest, xDest, yDest, wDest, hDest, hdcSrc, xSrc, ySrc, wSrc, hSrc, rop);
    }

    BOOL WINAPI UiRib_MyBitBlt(
        HDC hdc, int x, int y, int cx, int cy, HDC hdcSrc, int x1, int y1, DWORD rop)
    {
        auto caller = GetCallerModule(_ReturnAddress());
        auto orig = GetUiRibOrg<URH_BITBLT, decltype(&BitBlt)>(caller);
        if (!orig)
        {
            /*DebugLog(DBG_WARNING,
                L"[GlassCore][UiRibbon] MyBitBlt: org=null hdcDst=0x%p hdcSrc=0x%p caller=0x%p",
                hdc, hdcSrc, caller);*/
            return BitBlt(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);
        }
        if (g_shutdown.load(std::memory_order_acquire))
            return orig(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);
        if (caller == GlassCore::ModuleHandler::g_entryPointModule)
            return orig(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);
        if (!cx || !cy || rop != SRCCOPY || !hdcSrc)
            return orig(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);

        HBITMAP hBitmap = static_cast<HBITMAP>(GetCurrentObject(hdcSrc, OBJ_BITMAP));
        if (!hBitmap)
            return orig(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);
        
        PatBlt(hdc, x, y, cx, cy, BLACKNESS);
        
        BITMAP bm{};
        GetObject(hBitmap, sizeof(bm), &bm);
        /*DebugLog(L"[GlassCore][UiRibbon] MyBitBlt: hdcDst=0x%p hdcSrc=0x%p dst=(%d,%d,%d,%d) src=(%d,%d) caller=0x%p hBitmap=0x%p size={%d,%d} bpp=%d",
            hdc,
            hdcSrc,
            x,
            y,
            cx,
            cy,
            x1,
            y1,
            caller,
            hBitmap,
            bm.bmWidth,
            bm.bmHeight,
            bm.bmBitsPixel);*/

        if (bm.bmBitsPixel == 32)
        {
            // Полоса содержимого ленты (32bpp с альфа-каналом).
            // AC_SRC_OVER накладывает поверх старых пикселей — старое состояние
            // кнопки остаётся видимым. PatBlt(BLACKNESS) сначала очищает dst,
            // затем GdiAlphaBlend корректно замещает содержимое.
            
            BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            return GdiAlphaBlend(hdc, x, y, cx, cy, hdcSrc, x1, y1, cx, cy, bf);
        }

        // 24bpp — главная кнопка и рамка: нет альфа-канала,
        // нужна подготовка через PrepareBitmapForAlphaBlend.
        // Root\Control\Ribbon\ColorTreatAsTransparent → chroma-key (как BWM HandleMenuBitmap для ленты).
        GlassCore::GdiUtils::BitmapAlphaPrepareOptions fx{};
        if (s_tlsRibbonIconRemoval.enable)
        {
            fx.chromaKey = true;
            fx.chromaColor = Shared::ColorUtils::ARGBToCOLORREF(s_tlsRibbonIconRemoval.colorTreatAsTransparent);
            fx.chromaThreshold = static_cast<BYTE>(s_tlsRibbonIconRemoval.colorTreatAsTransparentThreshold);
        }

        wil::unique_hbitmap converted{};
        if (!GlassCore::GdiUtils::PrepareBitmapForAlphaBlend(hBitmap, converted, fx))
            return orig(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);

        HGDIOBJ hOldBitmap = SelectObject(hdcSrc, converted.get());
        auto restoreBitmap = wil::scope_exit([&] { SelectObject(hdcSrc, hOldBitmap); });

        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        return GdiAlphaBlend(hdc, x, y, cx, cy, hdcSrc, x1, y1, cx, cy, bf);
    }

    HGDIOBJ WINAPI UiRib_MyGetStockObject(int i)
    {
        const HMODULE callerGs = GetCallerModule(_ReturnAddress());
        auto orig = GetUiRibOrg<URH_GETSTOCKOBJECT, decltype(&GetStockObject)>(callerGs);
        /*if (!orig)
            DebugLog(DBG_WARNING,
                L"[GlassCore][UiRibbon] MyGetStockObject: org=null caller=0x%p obj=%d", callerGs, i);*/
        // BWM: в любой сессии IAT NetUI (BeginPaint на NetUIHWND) — иначе полоса табов уходит в белый непрозрачный фон.
        // Узкая привязка только к подсказке (IsNetUiRibbonTooltipPaintSession) здесь регрессирует ленту.
        if (!g_shutdown.load(std::memory_order_acquire)
            && g_netUiPaintDepth > 0
            && (i == WHITE_BRUSH || i == DC_BRUSH))
            i = BLACK_BRUSH;
        return orig ? orig(i) : GetStockObject(i);
    }

    HBRUSH WINAPI UiRib_MyGetSysColorBrush(int nIndex)
    {
        const HMODULE caller = GetCallerModule(_ReturnAddress());
        auto orig = GetUiRibOrg<URH_GETSYSCOLORBRUSH, decltype(&GetSysColorBrush)>(caller);
        if (!orig)
            return GetSysColorBrush(nIndex);

        if (!g_shutdown.load(std::memory_order_acquire)
            && IsNetUiRibbonTooltipPaintSession()
            && nIndex == COLOR_WINDOW)
        {
            // GetSysColorBrush(COLOR_WINDOW) + FillRect — чёрный фон подсказки.
            return reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        }

        return orig(nIndex);
    }

    COLORREF WINAPI UiRib_MySetDCBrushColor(HDC hdc, COLORREF color)
    {
        const HMODULE caller = GetCallerModule(_ReturnAddress());
        auto orig = GetUiRibOrg<URH_SETDCBRUSHCOLOR, decltype(&SetDCBrushColor)>(caller);
        if (!orig)
            return SetDCBrushColor(hdc, color);
        if (g_shutdown.load(std::memory_order_acquire))
            return orig(hdc, color);
        // Подсказка: после серого (0x767676) UIRibbon выставляет белый (0xFFFFFF) и FillRect
        // с DC_BRUSH закрашивает весь фон — если белый не подменять, визуально «нет эффекта»
        // от подмены серого. Полоса ленты не в tooltipSession — исключение для белого не нужно.
        if (IsNetUiRibbonTooltipPaintSession())
            return orig(hdc, RGB(0, 0, 0));
        return orig(hdc, color);
    }

    // BWM KbxLabelHooks::MyGdiGradientFill — градиент фона тултипа; на NetUIHWND только под сессией подсказки.
    BOOL WINAPI UiRib_MyGdiGradientFill(
        HDC        hdc,
        PTRIVERTEX pVertex,
        ULONG      nVertex,
        PVOID      pMesh,
        ULONG      nCount,
        ULONG      ulMode)
    {
        const HMODULE caller = GetCallerModule(_ReturnAddress());
        auto orig = GetUiRibOrg<URH_GDIGRADIENTFILL, decltype(&GdiGradientFill)>(caller);
        if (!orig)
            return GdiGradientFill(hdc, pVertex, nVertex, pMesh, nCount, ulMode);
        if (g_shutdown.load(std::memory_order_acquire))
            return orig(hdc, pVertex, nVertex, pMesh, nCount, ulMode);

        if (g_diagGdiGradientFillLogBudget.load(std::memory_order_relaxed) > 0
            && g_diagGdiGradientFillLogBudget.fetch_sub(1, std::memory_order_relaxed) > 0)
        {
            DebugLog(DBG_NORMAL,
                L"[GlassCore][UiRibbon] MyGdiGradientFill: hdc=0x%p nVtx=%lu meshElts=%lu mode=0x%X tooltipSession=%d",
                hdc,
                static_cast<unsigned long>(nVertex),
                static_cast<unsigned long>(nCount),
                static_cast<unsigned>(ulMode),
                IsNetUiRibbonTooltipPaintSession() ? 1 : 0);
        }

        if (IsNetUiRibbonTooltipPaintSession())
        {
            RECT clipRect{};
            switch (GetClipBox(hdc, &clipRect))
            {
            case NULLREGION:
            case ERROR:
                return orig(hdc, pVertex, nVertex, pMesh, nCount, ulMode);
            default:
                break;
            }
            const int w = clipRect.right - clipRect.left;
            const int h = clipRect.bottom - clipRect.top;
            if (w <= 0 || h <= 0)
                return orig(hdc, pVertex, nVertex, pMesh, nCount, ulMode);
            PatBlt(hdc, clipRect.left, clipRect.top, w, h, BLACKNESS);
            return TRUE;
        }
        return orig(hdc, pVertex, nVertex, pMesh, nCount, ulMode);
    }

    static void ApplyNetUiRibbonHooks(bool enable) noexcept
    {
        HMODULE rib = ResolveVerifiedUiRibbonModule();
        if (!rib)
        {
            DebugLog(DBG_WARNING,
                L"[GlassCore][UiRibbon] ApplyNetUiRibbonHooks(%d): нет проверенного UIRibbon.dll", enable);
            return;
        }

        const bool okDt = EnableUiRibHook(rib, URH_DRAWTEXT, enable);
        const bool okBb = EnableUiRibHook(rib, URH_BITBLT, enable);
        const bool okSb = EnableUiRibHook(rib, URH_STRETCHBLT, enable);
        const bool okGs = EnableUiRibHook(rib, URH_GETSTOCKOBJECT, enable);
        const bool okGsb = EnableUiRibHook(rib, URH_GETSYSCOLORBRUSH, enable);
        const bool okSdc = EnableUiRibHook(rib, URH_SETDCBRUSHCOLOR, enable);
        const bool okGf = EnableUiRibHook(rib, URH_GDIGRADIENTFILL, enable);
        DebugLog(DBG_IMPORTANT,
            L"[GlassCore][UiRibbon] ApplyNetUiRibbonHooks(%d): rib=0x%p DrawText=%d BitBlt=%d StretchBlt=%d GetStock=%d GetSysColorBr=%d SetDCBrushCol=%d GdiGradientFill=%d",
            enable, rib, okDt, okBb, okSb, okGs, okGsb, okSdc, okGf);
        // GdiGradientFill может отсутствовать в IAT конкретной сборки UIRibbon — не включаем okGf в общий WARN.
        if (!okDt || !okBb || !okSb || !okGs || !okGsb || !okSdc)
            DebugLog(DBG_WARNING,
                L"[GlassCore][UiRibbon] ApplyNetUiRibbonHooks: часть IAT не переключилась (см. флаги выше)");
        if (!okGf)
            DebugLog(DBG_NORMAL,
                L"[GlassCore][UiRibbon] ApplyNetUiRibbonHooks: GdiGradientFill IAT не найден (норма для части сборок)");
    }

    [[nodiscard]] static bool OnPostBeginPaintNetUi(
        HWND hWnd, HDC hdc, LPPAINTSTRUCT lpPaint, HMODULE /*callerModule*/) noexcept
    {
        if (g_shutdown.load(std::memory_order_acquire) || !hWnd || !hdc || !lpPaint)
            return false;
        if (g_netUiPaintDepth != 1)   return false;
        if (!lpPaint->fErase)
            return false;
        if (!IsNetUiRibbonSurface(hWnd))
            return false;
        DebugLog(DBG_IMPORTANT,
            L"[GlassCore][UiRibbon] OnPostBeginPaintNetUi: hWnd=0x%p rcPaint={%d,%d,%d,%d} fErase=%u",
            hWnd,
            lpPaint->rcPaint.left, lpPaint->rcPaint.top, lpPaint->rcPaint.right, lpPaint->rcPaint.bottom,
            static_cast<unsigned>(lpPaint->fErase));
        // Как BWM после BeginPaint — убрать старые пиксели в зоне перерисовки перед UIRibbon.
        const RECT& rc = lpPaint->rcPaint;
        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        if (w <= 0 || h <= 0)
            return false;

        PatBlt(hdc, rc.left, rc.top, w, h, BLACKNESS);
        return true;
    }

    void OnNetUiToolWindowHooksAttach(HWND /*hWnd*/) noexcept
    {
        // IAT UIRibbon включаются в OnBeginPaintGlobal по GlassControlKind::NetUiToolWindow
        // (маркер — NetUISubclass; коллбэки в этом модуле через DropDownEventProvider), как у NetUIHWND.
    }

    void OnNetUiToolWindowAttach(HWND hWnd) noexcept
    {
        if (g_shutdown.load(std::memory_order_acquire) || !IsWindow(hWnd))
            return;

        const bool useDarkMode = (GetSysColor(COLOR_WINDOW) < 0xFFFFFF);
        GlassCore::Settings::WindowBackdropEffectContext backdrop{};
        GlassCore::Settings::BorderContext border{};
        GlassCore::Settings::QueryBackdropEffectContext(L"DropDown", useDarkMode, backdrop);
        GlassCore::Settings::QueryBorderContext(L"DropDown", useDarkMode, border);
        GlassCore::Settings::ApplyEffect(hWnd, useDarkMode, backdrop, border);
    }

    void OnNetUiToolWindowDetach(HWND hWnd) noexcept
    {
        if (g_shutdown.load(std::memory_order_acquire) || !IsWindow(hWnd))
            return;
        GlassCore::Settings::DropEffect(L"DropDown", hWnd);
    }

    static void OnBeginPaintGlobal(HWND hWnd, HMODULE /*callerMod*/) noexcept
    {
        if (g_shutdown.load(std::memory_order_acquire))
        {
            DebugLog(DBG_WARNING, L"[GlassCore][UiRibbon] OnBeginPaint: shutdown hWnd=0x%p", hWnd);
            return;
        }
        if (!GlassCore::ModuleHandler::g_entryPointModule)
        {
            DebugLog(DBG_WARNING,
                L"[GlassCore][UiRibbon] OnBeginPaint: g_entryPointModule=null hWnd=0x%p — IAT-сессия отключена",
                hWnd);
            return;
        }

        using namespace GlassShared;
        const GlassControlKind k = GlassGetWindowKind(hWnd);
        const bool surface = (k == GlassControlKind::NetUI || k == GlassControlKind::NetUiToolWindow);
        if (!surface)
        {
            // Глобальный paint-провайдер GlassCore::UiRibbon — не UIRibbon.dll в процессе.
            // Тихий выход: иначе каждый BeginPaint (mmc, explorer, …) засоряет IMPRT.
            return;
        }

        // IAT UIRibbon до первого GDI после BeginPaint
        const int depthWas = g_netUiPaintDepth;
        if (g_netUiPaintDepth++ == 0)
        {
            s_tlsNetUiPaintSurfaceHwnd = hWnd;
            GlassCore::Settings::QueryIconBackgroundColorRemovalContext(
                { L"Root", L"Control", L"Ribbon" }, s_tlsRibbonIconRemoval);
            s_drawTextLogFrame.paintSeq = g_uiRibPaintSeq.fetch_add(1, std::memory_order_relaxed) + 1;
            s_drawTextLogFrame.calcLogged = 0;
            s_drawTextLogFrame.drawLogged = 0;
            DebugLog(DBG_IMPORTANT,
                L"[GlassCore][UiRibbon] OnBeginPaint: включаем IAT paint#%u hWnd=0x%p kind=%d",
                s_drawTextLogFrame.paintSeq,
                hWnd,
                static_cast<int>(k));
            ApplyNetUiRibbonHooks(true);
        }
        else
        {
            DebugLog(DBG_NORMAL,
                L"[GlassCore][UiRibbon] OnBeginPaint: вложенный paint depth %d→%d hWnd=0x%p kind=%d",
                depthWas, depthWas + 1, hWnd, static_cast<int>(k));
        }
    }

    static void OnEndPaintGlobal(HWND hWnd, HMODULE /*callerMod*/) noexcept
    {
        if (g_shutdown.load(std::memory_order_acquire))
        {
            DebugLog(DBG_WARNING, L"[GlassCore][UiRibbon] OnEndPaint: shutdown hWnd=0x%p", hWnd);
            return;
        }

        using namespace GlassShared;
        const GlassControlKind k = GlassGetWindowKind(hWnd);
        if (k != GlassControlKind::NetUI && k != GlassControlKind::NetUiToolWindow)
            return;

        if (g_netUiPaintDepth <= 0)
        {
            DebugLog(DBG_WARNING,
                L"[GlassCore][UiRibbon] OnEndPaint: дисбаланс depth=%d hWnd=0x%p kind=%d",
                g_netUiPaintDepth, hWnd, static_cast<int>(k));
            return;
        }
        const int depthAfter = --g_netUiPaintDepth;
        if (depthAfter == 0)
        {
            s_tlsNetUiPaintSurfaceHwnd = nullptr;
            DebugLog(DBG_IMPORTANT, L"[GlassCore][UiRibbon] OnEndPaint: выключаем IAT hWnd=0x%p", hWnd);
            ApplyNetUiRibbonHooks(false);
        }
        else
        {
            DebugLog(DBG_NORMAL,
                L"[GlassCore][UiRibbon] OnEndPaint: остаётся depth=%d hWnd=0x%p", depthAfter, hWnd);
        }
    }

    void Startup() noexcept
    {
        // Примечание: отдельные потоки (StretchDIBits / layered без BeginPaint на NetUIHWND) могут обходить
        // эту сессию IAT — исправление только для цепочки BeginPaint→EndPaint на маркированной поверхности.
        GlassCore::PaintDispatcher::RegisterPaintProvider({ OnBeginPaintGlobal, OnEndPaintGlobal });
        DebugLog(DBG_IMPORTANT,
            L"[GlassCore][UiRibbon] Startup: провайдер Begin/EndPaint (IAT UIRibbon.dll только на NetUI/NetUiToolWindow)");
    }

    void Shutdown() noexcept
    {
        g_shutdown.store(true, std::memory_order_release);
        DisableAllUiRibHooks();
        s_tlsNetUiPaintSurfaceHwnd = nullptr;
        g_shutdown.store(false, std::memory_order_release);
    }

} // namespace GlassCore::UiRibbon
