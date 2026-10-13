// GlassCore/src/Hooks/RootGdiHooks.cpp — GDI-диспетчер для root-окон (тела IAT-хуков).
// Классификация окон, кеш по HWND, провайдеры PaintDispatcher — в RootWindowHooks.cpp.
//
// ═══════════════════════════════════════════════════════════════════════════════
//  Роль этого файла (дополнение к комментарию в RootWindowHooks.cpp)
// ═══════════════════════════════════════════════════════════════════════════════
//
//  Здесь объявлены и реализованы Root_My* — реальные подмены API (DrawText*, ExtTextOut,
//  BitBlt, …), которые вызываются только когда для пары (модуль, OHK_*) включён слот
//  через EnableRootHook. Включение/выключение по сессиям делает RootWindowHooks:
//  Root_OnBeginPaint / Root_OnGetDC / Root_OnCreateCompatDC читают стек и списки из
//  s_rootCache и вызывают EnableSpecs → EnableRootHook (ref-count в HookDispatcher).
//
//  DisableAllRootHooks — точка shutdown: снимает все root-слоты с IAT (через IATRegistry).
//  Локальный g_shutdown координируется с RootWindowHooks::Shutdown
//  (там же SetRootGdiShutdown).
//
//  Вспомогательная политика внутри хуков (например IsWvcChartPaintSurface) — это не
//  дублирование ClassifyRootWindow: классификация решает «какие слоты вообще включать
//  в сессии», а здесь — узкие проверки в теле конкретного Root_My*.
// ═══════════════════════════════════════════════════════════════════════════════

#include "CorePch.h"
#include <Hooks/RootGdiHooks.h>
#include <Hooks/RootWindowHooks.h>
#include <Core/Intercept/HookDispatcher.h>
#include <Core/Intercept/IATRegistry.h>
#include <Module/ModuleHandler.h>
#include <Subsystems/RenderText.h>
#include <Subsystems/PaintDispatcher.h>
#include <Subsystems/GdiHookUtils.h>
#include <WinApiUtils.h>
#include <BitmapUtils.h>
#include <DebugLog.h>


#include <string>
#include <unordered_map>
#include <unordered_set>

using namespace GlassCore::Hooks;
using namespace std::literals;

#pragma comment(lib, "msimg32.lib")

namespace GlassCore::Root
{
        namespace
        {
            // MMC wvc график на 32bpp DIB: штрихи и заливки с лёгкой прозрачностью для DWM/«стекла».
            constexpr BYTE kMmcWvcChartStrokeAlpha = 236;
            constexpr BYTE kMmcWvcChartFillAlpha = 248;
            constexpr BYTE kRarextGradientFillAlpha = 255;

            // После GradientFill на 32bpp DIB: цветные пиксели → alpha=255, чёрный фон → прозрачный.
            static void RarextSetOpaqueAlphaOnDib(void* bits, int w, int h) noexcept
            {
                if (!bits || w <= 0 || h <= 0)
                    return;
                auto* px = static_cast<DWORD*>(bits);
                const int count = w * h;
                for (int i = 0; i < count; ++i)
                {
                    const DWORD rgb = px[i] & 0x00FFFFFFu;
                    px[i] = rgb ? (rgb | 0xFF000000u) : 0u;
                }
            }

            [[nodiscard]] static COLORREF ColorRefFromTriVertex(const TRIVERTEX& v) noexcept
            {
                return RGB(
                    static_cast<BYTE>((v.Red >> 8) & 0xFF),
                    static_cast<BYTE>((v.Green >> 8) & 0xFF),
                    static_cast<BYTE>((v.Blue >> 8) & 0xFF));
            }

            [[nodiscard]] static bool IsRarextSysLikeColor(COLORREF rgb) noexcept
            {
                rgb &= 0xFFFFFF;
                return !rgb
                    || rgb == (GetSysColor(COLOR_WINDOW) & 0xFFFFFF)
                    || rgb == (GetSysColor(COLOR_BTNFACE) & 0xFFFFFF);
            }

            [[nodiscard]] static bool RarextGradientAllSysColors(PTRIVERTEX pVertex, ULONG nVertex) noexcept
            {
                if (!pVertex || !nVertex)
                    return true;
                for (ULONG i = 0; i < nVertex; ++i)
                {
                    if (!IsRarextSysLikeColor(ColorRefFromTriVertex(pVertex[i])))
                        return false;
                }
                return true;
            }

            // Область заливки по mesh/вершинам — не весь clip (иначе чёрный offscreen блендится поверх графика).
            [[nodiscard]] static bool RarextComputeGradientDrawRect(
                PTRIVERTEX pVertex,
                ULONG      nVertex,
                PVOID      pMesh,
                ULONG      nCount,
                ULONG      ulMode,
                const RECT& clipRect,
                RECT&       outDraw) noexcept
            {
                LONG minX = LONG_MAX;
                LONG minY = LONG_MAX;
                LONG maxX = LONG_MIN;
                LONG maxY = LONG_MIN;

                auto includePoint = [&](LONG x, LONG y) noexcept
                {
                    minX = (std::min)(minX, x);
                    minY = (std::min)(minY, y);
                    maxX = (std::max)(maxX, x);
                    maxY = (std::max)(maxY, y);
                };

                if (pMesh && nCount && (ulMode & GRADIENT_FILL_TRIANGLE))
                {
                    for (ULONG i = 0; i < nVertex; ++i)
                        includePoint(pVertex[i].x, pVertex[i].y);
                }
                else if (pMesh && nCount)
                {
                    const auto* rects = static_cast<const GRADIENT_RECT*>(pMesh);
                    for (ULONG i = 0; i < nCount; ++i)
                    {
                        if (rects[i].UpperLeft >= nVertex || rects[i].LowerRight >= nVertex)
                            continue;
                        const TRIVERTEX& a = pVertex[rects[i].UpperLeft];
                        const TRIVERTEX& b = pVertex[rects[i].LowerRight];
                        includePoint(a.x, a.y);
                        includePoint(b.x, b.y);
                    }
                }
                else
                {
                    for (ULONG i = 0; i < nVertex; ++i)
                        includePoint(pVertex[i].x, pVertex[i].y);
                }

                if (minX >= maxX || minY >= maxY)
                    return false;

                outDraw.left = (std::max)(minX, clipRect.left);
                outDraw.top = (std::max)(minY, clipRect.top);
                outDraw.right = (std::min)(maxX, clipRect.right);
                outDraw.bottom = (std::min)(maxY, clipRect.bottom);
                return outDraw.left < outDraw.right && outDraw.top < outDraw.bottom;
            }
        }

        static std::atomic<bool> g_shutdown{ false };

        // ── Утилиты ──────────────────────────────────────────────────────────

        static bool StrEqI(LPCWSTR a, LPCWSTR b) noexcept
        {
            return a && b && _wcsicmp(a, b) == 0;
        }

        [[nodiscard]] static std::wstring BasenamePtr(LPCWSTR path) noexcept
        {
            if (!path || !path[0])
                return {};
            const wchar_t* p = wcsrchr(path, L'\\');
            return std::wstring(p ? p + 1 : path);
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

        [[nodiscard]] static bool CallerBaseIs(HMODULE caller, LPCWSTR base) noexcept
        {
            if (!caller || !base)
                return false;
            wchar_t path[MAX_PATH]{};
            if (!GetModuleFileNameW(caller, path, MAX_PATH))
                return false;
            return StrEqI(BasenamePtr(path).c_str(), base);
        }

        [[nodiscard]] static bool HasPrefixI(LPCWSTR s, LPCWSTR pref) noexcept
        {
            if (!s || !pref)
                return false;
            return _wcsnicmp(s, pref, wcslen(pref)) == 0;
        }

        static void FillWndAndParentClass(
            HWND hWnd, wchar_t* cls, size_t clsCch, wchar_t* parentCls, size_t parentCch) noexcept
        {
            cls[0] = L'\0';
            parentCls[0] = L'\0';
            if (!hWnd)
                return;
            GetClassNameW(hWnd, cls, static_cast<int>(clsCch));
            HWND p = GetParent(hWnd);
            if (p)
                GetClassNameW(p, parentCls, static_cast<int>(parentCch));
        }

        // Поверхность графика MMC: ChartGraphClass / дочерний ATL:*. Caller: wvc, ATL или sysmon.ocx
        // (сам снимок «Производительность» рисует контейнер графика из sysmon, не из wvc.dll).
        [[nodiscard]] static bool IsWvcChartPaintSurface(
            LPCWSTR cls, LPCWSTR parentCls, HMODULE caller) noexcept
        {
            const bool chartTree = StrEqI(cls, L"ChartGraphClass") || StrEqI(parentCls, L"ChartGraphClass");
            if (!chartTree)
                return false;
            return CallerBaseIs(caller, L"wvc.dll")
                || CallerBaseIs(caller, L"ATL.DLL")
                || CallerBaseIs(caller, L"sysmon.ocx")
                || HasPrefixI(cls, L"ATL:");
        }

        [[nodiscard]] static HWND HwndFromDc(HDC hdc) noexcept
        {
            if (!hdc)
                return nullptr;
            return WindowFromDC(hdc);
        }

        // D2: memory-DC (CreateCompatibleDC) не резолвятся через WindowFromDC.
        // Для MMC-модулей подставляем HWND активной paint-сессии того же потока,
        // но только attach'нутую (аналог BWM DataWndList[CompatibleDC] + IsDataAttach).
        // Затем обязательна проверка класса в теле хука.
        // Сначала владелец из стека своего потока: закрывает buffered-рисование
        // вне активной сессии (обратный поиск hdc→hWnd, только свой поток).
        [[nodiscard]] static HWND HwndFromDcOrMmcSession(HDC hdc, HMODULE caller) noexcept
        {
            HWND w = HwndFromDc(hdc);
            if (w)
                return w;
            if (HWND stackOwner = GlassCore::PaintDispatcher::GetStackOwnerForDc(hdc))
            {
                return stackOwner;
            }
            if (!caller)
                return nullptr;
            /*if (!CallerBaseIs(caller, L"dmview.ocx") && !CallerBaseIs(caller, L"dmdskmgr.dll")
                && !CallerBaseIs(caller, L"MFC42u.dll") && !CallerBaseIs(caller, L"comctl32.dll"))
                return nullptr;*/
            HWND s = GlassCore::PaintDispatcher::GetPaintSessionHostHwnd();
            const bool sAlive = s && ::IsWindow(s);
            HWND host = (sAlive && GlassCore::Root::HasRootHookCache(s)) ? s : nullptr;
            return host;
        }

        [[nodiscard]] static std::wstring CallerBaseName(HMODULE m) noexcept
        {
            wchar_t path[MAX_PATH]{};
            if (!m || !GetModuleFileNameW(m, path, MAX_PATH))
                return L"?";
            return BasenamePtr(path);
        }

        // Caller’ы, для которых диагностика MMC/графика осмысленна (без спама от каждого PatBlt).
        [[nodiscard]] static bool IsMmcChartDiagCaller(LPCWSTR callerBase) noexcept
        {
            return StrEqI(callerBase, L"wvc.dll") || StrEqI(callerBase, L"ATL.DLL") || StrEqI(callerBase, L"sysmon.ocx")
                || StrEqI(callerBase, L"gdiplus.dll");
        }

        // Memory/compatibility DC: WindowFromDC часто NULL. Для caller’ов MMC-графика подставляем HWND
        // с вершины стека PaintDispatcher (тот же поток, вложенный BeginPaint на ATL под ChartGraphClass).
        [[nodiscard]] static HWND HwndForMmcPaintClassification(HDC hdc, LPCWSTR callerBase) noexcept
        {
            HWND w = HwndFromDc(hdc);
            if (w)
                return w;
            if (!callerBase || !IsMmcChartDiagCaller(callerBase))
                return nullptr;
            HWND s = GlassCore::PaintDispatcher::GetPaintSessionHostHwnd();
            if (!s || !IsWindow(s))
                return nullptr;
            return s;
        }

        [[nodiscard]] static bool WantMmcChartGdiLog(
            LPCWSTR callerBase,
            LPCWSTR cls,
            LPCWSTR parentCls,
            bool    wvcChartPaint) noexcept
        {
            if (wvcChartPaint)
                return true;
            if (StrEqI(cls, L"ChartGraphClass") || StrEqI(parentCls, L"ChartGraphClass"))
                return true;
            return IsMmcChartDiagCaller(callerBase);
        }

        // ExtTextOut + ETO_OPAQUE: в BWM для sysmon/графика — Sysmon::WorkExtTextOutW (OverallHooks).
        [[nodiscard]] static bool WantSysmonStyleExtTextOpaqueBg(LPCWSTR cls, LPCWSTR parentCls) noexcept
        {
            if (StrEqI(cls, L"SysmonCtrl") || StrEqI(cls, L"SysmonReport") || StrEqI(cls, L"PerfStatus")
                || StrEqI(cls, L"PerfLegend"))
                return true;
            if (StrEqI(cls, L"ChartGraphClass") || StrEqI(parentCls, L"ChartGraphClass"))
                return true;
            if (HasPrefixI(cls, L"ATL:") && StrEqI(parentCls, L"ChartGraphClass"))
                return true;
            return false;
        }

        // PatBlt BLACKNESS, затем условный FillRect(GetBkColor) — как Sysmon::WorkExtTextOutW.
        static void ApplySysmonStyleOpaqueBackground(HDC hdc, const RECT* lpr, HMODULE caller) noexcept
        {
            if (!lpr || !hdc)
                return;
            PatBlt(
                hdc,
                lpr->left,
                lpr->top,
                lpr->right - lpr->left,
                lpr->bottom - lpr->top,
                BLACKNESS);
            const COLORREF color = GetBkColor(hdc);
            wil::unique_hbrush brush{ Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(color, 255) };
            if (!brush)
                return;
            const COLORREF rgb = color & 0xFFFFFF;
            if (!rgb)
                return;
            if (rgb == (GetSysColor(COLOR_WINDOW) & 0xFFFFFF))
                return;
            if (rgb == (GetSysColor(COLOR_BTNFACE) & 0xFFFFFF) && CallerBaseIs(caller, L"sysmon.ocx"))
                return;
            FillRect(hdc, lpr, brush.get());
        }

        // BWM OverallHooks:1099-1114 без GDI+: штриховка плиток комбобокса diskmgmt
        // (dmdskmgr + ComboBox/ComboLBox) — 8×8 32bpp DIB с альфой (эталон — ComboLBox).
        static const BYTE kRootHatchPatterns[6][8] =
        {
            { 0x00,0x00,0x00,0xFF,0x00,0x00,0x00,0x00 }, // HS_HORIZONTAL
            { 0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08 }, // HS_VERTICAL
            { 0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80 }, // HS_FDIAGONAL
            { 0x80,0x40,0x20,0x10,0x08,0x04,0x02,0x01 }, // HS_BDIAGONAL
            { 0x08,0x08,0x08,0xFF,0x08,0x08,0x08,0x08 }, // HS_CROSS
            { 0x81,0x42,0x24,0x18,0x18,0x24,0x42,0x81 }, // HS_DIAGCROSS
        };

        [[nodiscard]] static HBRUSH CreateRootAlphaHatchBrush(int fnStyle, COLORREF color) noexcept
        {
            if (fnStyle < 0 || fnStyle > 5)
                return Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(color, 255);

            const BYTE r = GetRValue(color);
            const BYTE g = GetGValue(color);
            const BYTE b = GetBValue(color);

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
            if (!hbm)
                return nullptr;

            auto* px = static_cast<DWORD*>(bits);
            const DWORD fg = 0xFF000000u
                | (static_cast<DWORD>(r) << 16)
                | (static_cast<DWORD>(g) << 8)
                | b;
            const DWORD bg = 0x00000000u; // прозрачный

            const auto& rows = kRootHatchPatterns[fnStyle];
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++)
                    px[y * 8 + x] = (rows[y] & (0x80 >> x)) ? fg : bg;

            HBRUSH hbr = CreatePatternBrush(hbm);
            DeleteObject(hbm); // кисть владеет копией
            return hbr;
        }

        // BWM ComboBoxHooks::WorkExtTextOutW через OverallHooks::MyExtTextOutW: плитки
        // комбобоксов diskmgmt (dmdskmgr/dmview, поле и список). Сессионный аналог для
        // comctl32/COMDLG32/MFC42u — ComboBoxControlHooks::Overall_MyExtTextOutW.
        [[nodiscard]] static bool WantComboBoxStyleExtTextOut(HMODULE caller, LPCWSTR cls) noexcept
        {
            if (!StrEqI(cls, L"ComboBox") && !StrEqI(cls, L"ComboLBox"))
                return false;
            return CallerBaseIs(caller, L"dmdskmgr.dll") || CallerBaseIs(caller, L"dmview.ocx")
                || CallerBaseIs(caller, L"MFC42u.dll") || CallerBaseIs(caller, L"comctl32.dll");
        }

        static BOOL ComboBoxStyleExtTextOut(HDC hdc, int x, int y, UINT options, const RECT* lprect,
            LPCWSTR lpString, UINT c, const INT* lpDx, HWND hWnd, HMODULE caller,
            BOOL (WINAPI* orig)(HDC, int, int, UINT, const RECT*, LPCWSTR, UINT, const INT*))
        {
            auto callOrig = [&](UINT opts) -> BOOL
            {
                if (orig)
                    return orig(hdc, x, y, opts, lprect, lpString, c, lpDx);
                return ExtTextOutW(hdc, x, y, opts, lprect, lpString, c, lpDx);
            };

            RECT client{};
            if (hWnd)
                GetClientRect(hWnd, &client);

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
                    const bool mfcInset = CallerBaseIs(caller, L"MFC42u.dll")
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

        // ── Root GDI dispatcher ─────────────────────────────────────────────

        int WINAPI Root_MyDrawTextW(HDC, LPCWSTR, int, LPRECT, UINT);
        int WINAPI Root_MyDrawTextExW(HDC, LPWSTR, int, LPRECT, UINT, LPDRAWTEXTPARAMS);
        BOOL WINAPI Root_MyExtTextOutW(HDC, int, int, UINT, const RECT*, LPCWSTR, UINT, const INT*);
        COLORREF WINAPI Root_MySetBkColor(HDC, COLORREF);
        HBRUSH WINAPI Root_MyGetSysColorBrush(int);
        HBRUSH WINAPI Root_MyCreateSolidBrush(COLORREF);
        HGDIOBJ WINAPI Root_MySelectObject(HDC, HGDIOBJ);
        BOOL WINAPI Root_MyPatBlt(HDC, int, int, int, int, DWORD);
        BOOL WINAPI Root_MyLineTo(HDC, int, int);
        BOOL WINAPI Root_MyPolyline(HDC, const POINT*, int);
        BOOL WINAPI Root_MyPolygon(HDC, const POINT*, int);
        BOOL WINAPI Root_MyRectangle(HDC, int, int, int, int);
        int WINAPI Root_MyFrameRect(HDC, const RECT*, HBRUSH);
        BOOL WINAPI Root_MyBitBlt(HDC, int, int, int, int, HDC, int, int, DWORD);
        BOOL WINAPI Root_MyGradientFill(HDC, PTRIVERTEX, ULONG, PVOID, ULONG, ULONG);
        BOOL WINAPI Root_MyTextOutW(HDC, int, int, LPCWSTR, int);
        int WINAPI Root_MyFillRect(HDC, const RECT*, HBRUSH);

        inline const HookDispatcherDependency<OHK_COUNT, 3> g_rootHookDep
        { {
            std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
                "DrawTextW", reinterpret_cast<PVOID>(Root_MyDrawTextW) },
            std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
                "DrawTextExW", reinterpret_cast<PVOID>(Root_MyDrawTextExW) },
            std::tuple{ std::array{ "gdi32.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
                "ExtTextOutW", reinterpret_cast<PVOID>(Root_MyExtTextOutW) },
            std::tuple{ std::array{ "gdi32.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
                "SetBkColor", reinterpret_cast<PVOID>(Root_MySetBkColor) },
            std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
                "GetSysColorBrush", reinterpret_cast<PVOID>(Root_MyGetSysColorBrush) },
            std::tuple{ std::array{ "gdi32.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
                "CreateSolidBrush", reinterpret_cast<PVOID>(Root_MyCreateSolidBrush) },
            std::tuple{ std::array{ "gdi32.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
                "SelectObject", reinterpret_cast<PVOID>(Root_MySelectObject) },
            std::tuple{ std::array{ "gdi32.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
                "PatBlt", reinterpret_cast<PVOID>(Root_MyPatBlt) },
            std::tuple{ std::array{ "gdi32.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
                "LineTo", reinterpret_cast<PVOID>(Root_MyLineTo) },
            std::tuple{ std::array{ "gdi32.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
                "Polyline", reinterpret_cast<PVOID>(Root_MyPolyline) },
            std::tuple{ std::array{ "gdi32.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
                "Polygon", reinterpret_cast<PVOID>(Root_MyPolygon) },
            std::tuple{ std::array{ "gdi32.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
                "Rectangle", reinterpret_cast<PVOID>(Root_MyRectangle) },
            std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
                "FrameRect", reinterpret_cast<PVOID>(Root_MyFrameRect) },
            std::tuple{ std::array{ "gdi32.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
                "BitBlt", reinterpret_cast<PVOID>(Root_MyBitBlt) },
            std::tuple{ std::array{ "msimg32.dll"sv, ""sv, ""sv },
                "GradientFill", reinterpret_cast<PVOID>(Root_MyGradientFill) },
            std::tuple{ std::array{ "gdi32.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
                "TextOutW", reinterpret_cast<PVOID>(Root_MyTextOutW) },
            std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
                "FillRect", reinterpret_cast<PVOID>(Root_MyFillRect) },
        } };

        using RootDispatcher = HookDispatcher<OHK_COUNT, 3>;

        static wil::srwlock                                          g_rootLock;
        static std::unordered_map<void*, RootDispatcher>             g_rootMap;

        template <size_t idx, typename T>
        [[nodiscard]] static T GetRootOrg(HMODULE callerMod) noexcept
        {
            auto lock = g_rootLock.lock_shared();
            auto it = g_rootMap.find(static_cast<void*>(callerMod));
            if (it != g_rootMap.end())
                return it->second.template GetOrg<idx, T>();
            return nullptr;
        }

        // ── EnableRootHook / DisableAllRootHooks ─────────────────────────────

        bool EnableRootHook(void* baseAddress, size_t index, bool enable) noexcept
        {
            if (!baseAddress)
                return false;

            auto lock = g_rootLock.lock_exclusive();
            auto it = g_rootMap.find(baseAddress);
            if (it == g_rootMap.end())
            {
                auto [ins, ok] = g_rootMap.emplace(
                    std::piecewise_construct,
                    std::forward_as_tuple(baseAddress),
                    std::forward_as_tuple(g_rootHookDep, baseAddress));
                it = ins;
                it->second.CacheHookData();
            }
            return it->second.EnableHook(index, enable);
        }

        void PrewarmRootHook(void* baseAddress) noexcept
        {
            if (!baseAddress)
                return;

            auto lock = g_rootLock.lock_exclusive();
            auto it = g_rootMap.find(baseAddress);
            if (it == g_rootMap.end())
            {
                auto [ins, ok] = g_rootMap.emplace(
                    std::piecewise_construct,
                    std::forward_as_tuple(baseAddress),
                    std::forward_as_tuple(g_rootHookDep, baseAddress));
                it = ins;
                it->second.CacheHookData();
            }
        }

        void DisableAllRootHooks() noexcept
        {
            auto lock = g_rootLock.lock_exclusive();
            for (auto& [_, d] : g_rootMap)
                d.DisableAllHooks();
            g_rootMap.clear();
        }

        void SetRootGdiShutdown(bool value) noexcept
        {
            g_shutdown.store(value, std::memory_order_release);
        }

        // ── Hook bodies: root GDI (упрощённые ветки BWM OverallHooks) ───────

        int WINAPI Root_MyDrawTextW(
            HDC hdc, LPCWSTR t, int c, LPRECT rc, UINT fmt)
        {
            auto o = GetRootOrg<OHK_DRAWTEXT, decltype(&DrawTextW)>(GetCallerModule(_ReturnAddress()));
            if (!o)
                return DrawTextW(hdc, t, c, rc, fmt);
            if (g_shutdown.load(std::memory_order_acquire))
                return o(hdc, t, c, rc, fmt);
            if ((fmt & DT_CALCRECT) != 0)
                return o(hdc, t, c, rc, fmt);
            int r = 0;
            if (!FAILED(GlassCore::Text::DrawTextWithAlpha(hdc, t, c, rc, fmt, r)))
                return r;
            return o(hdc, t, c, rc, fmt);
        }

        int WINAPI Root_MyDrawTextExW(
            HDC hdc, LPWSTR t, int c, LPRECT rc, UINT fmt, LPDRAWTEXTPARAMS dtp)
        {
            auto o = GetRootOrg<OHK_DRAWTEXTEX, decltype(&DrawTextExW)>(GetCallerModule(_ReturnAddress()));
            if (!o)
                return DrawTextExW(hdc, t, c, rc, fmt, dtp);
            if (g_shutdown.load(std::memory_order_acquire))
                return o(hdc, t, c, rc, fmt, dtp);
            if ((fmt & DT_CALCRECT) != 0)
                return o(hdc, t, c, rc, fmt, dtp);
            int r = 0;
            if (!FAILED(GlassCore::Text::DrawTextWithAlpha(hdc, t, c, rc, fmt, r)))
                return r;
            return o(hdc, t, c, rc, fmt, dtp);
        }

        BOOL WINAPI Root_MyExtTextOutW(
            HDC hdc, int x, int y, UINT opt, const RECT* lpr,
            LPCWSTR s, UINT c, const INT* dx)
        {
            auto o = GetRootOrg<OHK_EXTTEXTOUT, decltype(&ExtTextOutW)>(GetCallerModule(_ReturnAddress()));
            if (!o)
                return ExtTextOutW(hdc, x, y, opt, lpr, s, c, dx);
            if (g_shutdown.load(std::memory_order_acquire))
                return o(hdc, x, y, opt, lpr, s, c, dx);
            // Плитки комбобоксов diskmgmt (BWM WorkExtTextOutW): полный паритет фона
            // и текста, общий путь ниже их не покрывает (там только sysmon-классы).
            {
                HMODULE caller = GetCallerModule(_ReturnAddress());
                HWND hW = HwndFromDcOrMmcSession(hdc, caller);
                wchar_t cls[128]{};
                if (hW)
                    GetClassNameW(hW, cls, _countof(cls));
                if (WantComboBoxStyleExtTextOut(caller, cls))
                {
                    return ComboBoxStyleExtTextOut(hdc, x, y, opt, lpr, s, c, dx, hW, caller, o);
                }
            }
            if (opt & ETO_OPAQUE)
            {
                if (lpr)
                {
                    auto caller = GetCallerModule(_ReturnAddress());
                    // D2: memory-DC классифицируем через сессию.
                    HWND    hW = HwndFromDcOrMmcSession(hdc, caller);
                    wchar_t cls[128]{};
                    wchar_t parentCls[128]{};
                    FillWndAndParentClass(hW, cls, _countof(cls), parentCls, _countof(parentCls));
                    if (WantSysmonStyleExtTextOpaqueBg(cls, parentCls))
                        ApplySysmonStyleOpaqueBackground(hdc, lpr, caller);
                    else
                    {
                        PatBlt(
                            hdc,
                            lpr->left,
                            lpr->top,
                            lpr->right - lpr->left,
                            lpr->bottom - lpr->top,
                            BLACKNESS);
                    }
                }
                if (!s || c == 0)
                {
                    return TRUE;
                }
            }

            if (opt == ETO_GLYPH_INDEX || opt == ETO_IGNORELANGUAGE)
                return o(hdc, x, y, opt, lpr, s, c, dx);

            RECT clip{};
            if (lpr) clip = *lpr;
            else GetClipBox(hdc, &clip);
            clip.left = x;
            clip.top = y;
            UINT align = (opt & ETO_RTLREADING) ? (DT_RIGHT | DT_RTLREADING) : DT_LEFT;
            SetBkMode(hdc, TRANSPARENT);
            int r = 0;
            const bool afxTextOk = !FAILED(GlassCore::Text::DrawTextWithAlpha(hdc, s, c, &clip, align, r));
            if (afxTextOk)
                return r;
            return o(hdc, x, y, opt, lpr, s, c, dx);
        }

        COLORREF WINAPI Root_MySetBkColor(HDC hdc, COLORREF color)
        {
            auto     caller = GetCallerModule(_ReturnAddress());
            auto     o = GetRootOrg<OHK_SETBKCOLOR, decltype(&SetBkColor)>(caller);
            if (!o)
                return SetBkColor(hdc, color);

            const std::wstring callerBaseStr = CallerBaseName(caller);
            const wchar_t*     callerBase = callerBaseStr.c_str();
            // D2: memory-DC классифицируем через сессию.
            HWND               hFromDc = HwndFromDcOrMmcSession(hdc, caller);
            wchar_t            cls[128]{};
            if (hFromDc)
                GetClassNameW(hFromDc, cls, _countof(cls));

            if (!g_shutdown.load(std::memory_order_acquire)
                && (0xFFFFFF & color) == GetSysColor(COLOR_WINDOW))
            {
                // Лог 2026-10-06: ремап действует ТОЛЬКО для MMC-графиков (иначе
                // SetBkColor(WHITE) от MFC42u для плиток комбобокса diskmgmt приходил
                // в ExtTextOut уже чёрным — BWM там не ремапит). Строка color = 0
                // раньше стояла вне if — накрывала все окна и коллеры.
                if (IsMmcChartDiagCaller(callerBase) || StrEqI(cls, L"ChartGraphClass")
                    || StrEqI(cls, L"PerfStatus") || StrEqI(cls, L"SysmonCtrl"))
                {
                    DebugLog(DBG_NORMAL,
                        L"[GlassCore][Root][MMC] SetBkColor: COLOR_WINDOW→0 hdc=0x%p hwndDC=0x%p caller=%s cls=\"%s\"",
                        hdc, hFromDc, callerBase, cls);
                    color = 0;
                }
            }

            if (!g_shutdown.load(std::memory_order_acquire) && StrEqI(cls, L"AfxWnd42u")
                && (CallerBaseIs(caller, L"MFC42u.dll") || CallerBaseIs(caller, L"comctl32.dll")
                    || CallerBaseIs(caller, L"dmview.ocx") || CallerBaseIs(caller, L"dmdskmgr.dll")))
            {
                const COLORREF c = color & 0xFFFFFF;
                if (c == (GetSysColor(COLOR_WINDOW) & 0xFFFFFF)
                    || c == (GetSysColor(COLOR_BTNFACE) & 0xFFFFFF))
                {
                    color = 0;
                }
            }
            return o(hdc, color);
        }

        HBRUSH WINAPI Root_MyGetSysColorBrush(int nIndex)
        {
            auto o = GetRootOrg<OHK_GETSYSCOLORBR, decltype(&GetSysColorBrush)>(GetCallerModule(_ReturnAddress()));
            if (!o)
                return GetSysColorBrush(nIndex);
            if (!g_shutdown.load(std::memory_order_acquire))
            {
                if (nIndex == COLOR_WINDOW || nIndex == COLOR_BTNFACE || nIndex == COLOR_MENU)
                    return CreateSolidBrush(0);
            }
            return o(nIndex);
        }

        HBRUSH WINAPI Root_MyCreateSolidBrush(COLORREF color)
        {
            auto caller = GetCallerModule(_ReturnAddress());
            auto o = GetRootOrg<OHK_CREATESOLIDBRUSH, decltype(&CreateSolidBrush)>(caller);
            if (!o)
                return CreateSolidBrush(color);

            if (g_shutdown.load(std::memory_order_acquire))
                return o(color);

            // BWM-паритет (WinHooks::MyCreateSolidBrush): глобальный перехват
            // создания фоновых кистей. Свои вызовы не трогаем, чтобы не
            // закольцевать собственные alpha-кисти.
            static HMODULE s_selfMod = ::GetModuleHandleW(L"GlassCore.dll");
            if (caller && caller == s_selfMod)
                return o(color);

            wchar_t baseBuf[MAX_PATH]{};
            GetModuleFileNameW(caller, baseBuf, MAX_PATH);
            const std::wstring base = BasenamePtr(baseBuf);

            // BWM: COMDLG32 — только alpha-обёртка.
            if (StrEqI(base.c_str(), L"COMDLG32.dll"))
            {
                HBRUSH b = Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(color, 255);
                return b ? b : o(color);
            }

            // BWM: 0 / window / btnface / цвета темы — в чёрный (прозрачный фон).
            const COLORREF plain = color & 0xFFFFFF;
            if (!plain
                || plain == (GetSysColor(COLOR_WINDOW) & 0xFFFFFF)
                || plain == (GetSysColor(COLOR_BTNFACE) & 0xFFFFFF)
                || Shared::ThemeHelper::IsThemeColor(color, L"ExplorerNavPane")
                || Shared::ThemeHelper::IsThemeColor(color, L"ProperTree"))
                return o(RGB(0, 0, 0));

            // Остальное — alpha-кисть для корректного композитинга.
            HBRUSH b = Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(color, 255);
            return b ? b : o(color);
        }

        HGDIOBJ WINAPI Root_MySelectObject(HDC hdc, HGDIOBJ h)
        {
            auto caller = GetCallerModule(_ReturnAddress());
            auto o = GetRootOrg<OHK_SELECTOBJECT, decltype(&SelectObject)>(caller);
            if (!o)
                return SelectObject(hdc, h);
            if (g_shutdown.load(std::memory_order_acquire))
                return o(hdc, h);

            HWND w = HwndFromDc(hdc);
            wchar_t cls[256]{};
            if (w)
                GetClassNameW(w, cls, _countof(cls));

            LOGBRUSH lb{};

            if (CallerBaseIs(caller, L"COMDLG32.dll")
                && w && static_cast<ATOM>(GetClassLongPtrW(w, GCW_ATOM)) == static_cast<ATOM>(32770))
            {
                if (GetObjectType(h) == OBJ_BRUSH && GetObject(h, sizeof(lb), &lb)
                    && !lb.lbColor && !lb.lbStyle)
                    h = Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(lb.lbColor, 255);
            }
            else if (CallerBaseIs(caller, L"USER32.dll"))
            {
                if (GetObjectType(h) == OBJ_BRUSH && GetObject(h, sizeof(lb), &lb))
                {
                    if (lb.lbColor == GetSysColor(COLOR_WINDOW) || lb.lbColor == GetSysColor(COLOR_BTNFACE))
                        h = GetStockObject(BLACK_BRUSH);
                }
            }

            return o(hdc, h);
        }

        BOOL WINAPI Root_MyPatBlt(HDC hdc, int x, int y, int cx, int cy, DWORD rop)
        {
            auto caller = GetCallerModule(_ReturnAddress());
            const std::wstring callerBaseStr = CallerBaseName(caller);
            const wchar_t*   callerBase = callerBaseStr.c_str();

            HWND hWnd = HwndForMmcPaintClassification(hdc, callerBase);
            wchar_t cls[128]{};
            wchar_t parentCls[128]{};
            FillWndAndParentClass(hWnd, cls, _countof(cls), parentCls, _countof(parentCls));

            const bool wvcChartPaint = IsWvcChartPaintSurface(cls, parentCls, caller);
            const bool chartTreeCls = StrEqI(cls, L"ChartGraphClass") || StrEqI(parentCls, L"ChartGraphClass");

            if (hdc && !HwndFromDc(hdc) && IsMmcChartDiagCaller(callerBase) && !hWnd)
            {
                DebugLog(DBG_WARNING,
                    L"[GlassCore][Root][MMC] PatBlt: WindowFromDC=NULL и нет session HWND — классификация фона hdc=0x%p caller=%s "
                    L"(wvcChart=%d chartTreeCls=%d)",
                    hdc, callerBase, wvcChartPaint ? 1 : 0, chartTreeCls ? 1 : 0);
            }

            auto o = GetRootOrg<OHK_PATBLT, decltype(&PatBlt)>(caller);
            if (!o && wvcChartPaint)
                o = GetRootOrg<OHK_PATBLT, decltype(&PatBlt)>(GetModuleHandleW(L"wvc.dll"));
            if (!o)
            {
                if (WantMmcChartGdiLog(callerBase, cls, parentCls, wvcChartPaint))
                {
                    DebugLog(DBG_WARNING,
                        L"[GlassCore][Root][MMC] PatBlt: нет org (IAT слот выключен или не зарегистрирован) hdc=0x%p hwndDC=0x%p "
                        L"caller=%s wvcChart=%d",
                        hdc, hWnd, callerBase, wvcChartPaint ? 1 : 0);
                }
                return PatBlt(hdc, x, y, cx, cy, rop);
            }
            if (g_shutdown.load(std::memory_order_acquire))
                return o(hdc, x, y, cx, cy, rop);

            LOGBRUSH lbrush{};
            HGDIOBJ hbr = GetCurrentObject(hdc, OBJ_BRUSH);
            GetObject(hbr, sizeof(lbrush), &lbrush);

            const bool sysmon = CallerBaseIs(caller, L"sysmon.ocx") && StrEqI(cls, L"SysmonCtrl");
            const bool gdiplusCaller = CallerBaseIs(caller, L"gdiplus.dll");
            const bool wantSpecial = wvcChartPaint || sysmon || gdiplusCaller;

            enum class PatDiag : int { None = 0, Blackness = 1, AlphaBrush = 2, SpecialNoop = 3 };
            PatDiag patDiag = PatDiag::None;

            if (wantSpecial)
            {
                const COLORREF rgb = lbrush.lbColor & 0xFFFFFF;
                if (!rgb || rgb == (GetSysColor(COLOR_WINDOW) & 0xFFFFFF)
                    || rgb == (GetSysColor(COLOR_BTNFACE) & 0xFFFFFF))
                {
                    rop = BLACKNESS;
                    patDiag = PatDiag::Blackness;
                }
                else if (CallerBaseIs(caller, L"USER32.dll") && rop == WHITENESS)
                {
                    rop = BLACKNESS;
                    patDiag = PatDiag::Blackness;
                }
                else
                {
                    const BYTE fillAlpha = wvcChartPaint ? kMmcWvcChartFillAlpha : static_cast<BYTE>(255);
                    wil::unique_hbrush br{ Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(lbrush.lbColor, fillAlpha) };
                    if (br)
                    {
                        HGDIOBJ old = SelectObject(hdc, br.get());
                        BOOL    res = o(hdc, x, y, cx, cy, rop);
                        SelectObject(hdc, old);
                        if (WantMmcChartGdiLog(callerBase, cls, parentCls, wvcChartPaint))
                        {
                            DebugLog(DBG_NORMAL,
                                L"[GlassCore][Root][MMC] PatBlt→alpha_brush hdc=0x%p hwndDC=0x%p caller=%s cls=\"%s\" parent=\"%s\" "
                                L"wvcChart=%d sysmon=%d gdiplus=%d xywh=%d,%d,%d,%d ropOut=0x%08X lb=0x%06X",
                                hdc, hWnd, callerBase, cls, parentCls, wvcChartPaint ? 1 : 0, sysmon ? 1 : 0,
                                gdiplusCaller ? 1 : 0, x, y, cx, cy, rop,
                                static_cast<unsigned>(lbrush.lbColor & 0xFFFFFF));
                        }
                        return res;
                    }
                    patDiag = PatDiag::SpecialNoop;
                }
            }

            if (WantMmcChartGdiLog(callerBase, cls, parentCls, wvcChartPaint))
            {
                DebugLog(DBG_NORMAL,
                    L"[GlassCore][Root][MMC] PatBlt hdc=0x%p hwndDC=0x%p caller=%s cls=\"%s\" parent=\"%s\" "
                    L"chartTree=%d wvcChart=%d sysmon=%d gdiplus=%d wantSpec=%d diag=%d xywh=%d,%d,%d,%d rop=0x%08X lb=0x%06X",
                    hdc, hWnd, callerBase, cls, parentCls, chartTreeCls ? 1 : 0, wvcChartPaint ? 1 : 0, sysmon ? 1 : 0,
                    gdiplusCaller ? 1 : 0, wantSpecial ? 1 : 0, static_cast<int>(patDiag), x, y, cx, cy, rop,
                    static_cast<unsigned>(lbrush.lbColor & 0xFFFFFF));
            }

            return o(hdc, x, y, cx, cy, rop);
        }

        BOOL WINAPI Root_MyLineTo(HDC hdc, int x, int y)
        {
            HMODULE caller = GetCallerModule(_ReturnAddress());
            const std::wstring callerBaseStr = CallerBaseName(caller);
            const wchar_t*   callerBase = callerBaseStr.c_str();

            HWND hWnd = HwndForMmcPaintClassification(hdc, callerBase);
            wchar_t cls[128]{};
            wchar_t parentCls[128]{};
            FillWndAndParentClass(hWnd, cls, _countof(cls), parentCls, _countof(parentCls));
            const bool wvcChartPaint = IsWvcChartPaintSurface(cls, parentCls, caller);
            const bool chartTreeCls = StrEqI(cls, L"ChartGraphClass") || StrEqI(parentCls, L"ChartGraphClass");

            if (hdc && !HwndFromDc(hdc) && IsMmcChartDiagCaller(callerBase) && !hWnd)
            {
                DebugLog(DBG_WARNING,
                    L"[GlassCore][Root][MMC] LineTo: WindowFromDC=NULL и нет session HWND hdc=0x%p caller=%s wvcChart=%d",
                    hdc, callerBase, wvcChartPaint ? 1 : 0);
            }

            auto o = GetRootOrg<OHK_LINETO, decltype(&LineTo)>(caller);
            if (!o && wvcChartPaint)
                o = GetRootOrg<OHK_LINETO, decltype(&LineTo)>(GetModuleHandleW(L"wvc.dll"));
            if (!o)
            {
                if (WantMmcChartGdiLog(callerBase, cls, parentCls, wvcChartPaint))
                {
                    DebugLog(DBG_WARNING,
                        L"[GlassCore][Root][MMC] LineTo: нет org hdc=0x%p hwndDC=0x%p caller=%s wvcChart=%d",
                        hdc, hWnd, callerBase, wvcChartPaint ? 1 : 0);
                }
                return LineTo(hdc, x, y);
            }
            if (g_shutdown.load(std::memory_order_acquire))
                return o(hdc, x, y);

            const bool useGlassDraw = wvcChartPaint || CallerBaseIs(caller, L"wdc.dll")
                || CallerBaseIs(caller, L"rarext.dll");

            if (!useGlassDraw)
            {
                if (WantMmcChartGdiLog(callerBase, cls, parentCls, wvcChartPaint))
                {
                    DebugLog(DBG_NORMAL,
                        L"[GlassCore][Root][MMC] LineTo→orig (useGlassDraw=0) hdc=0x%p hwndDC=0x%p caller=%s cls=\"%s\" "
                        L"parent=\"%s\" wvcChart=%d wdc=%d",
                        hdc, hWnd, callerBase, cls, parentCls, wvcChartPaint ? 1 : 0,
                        CallerBaseIs(caller, L"wdc.dll") ? 1 : 0);
                }
                return o(hdc, x, y);
            }

            HGDIOBJ penObj = GetCurrentObject(hdc, OBJ_PEN);
            LOGPEN  lp{};
            if (GetObject(penObj, sizeof(lp), &lp) != sizeof(LOGPEN) || lp.lopnStyle == PS_NULL)
            {
                if (WantMmcChartGdiLog(callerBase, cls, parentCls, wvcChartPaint))
                {
                    DebugLog(DBG_NORMAL,
                        L"[GlassCore][Root][MMC] LineTo→orig (pen null/invalid) hdc=0x%p hwndDC=0x%p caller=%s wvcChart=%d",
                        hdc, hWnd, callerBase, wvcChartPaint ? 1 : 0);
                }
                return o(hdc, x, y);
            }

            POINT lppt{};
            if (!GetCurrentPositionEx(hdc, &lppt))
                return o(hdc, x, y);

            GdiUtils::LayeredStrokePen layeredStroke;
            wil::unique_hpen          drawPen;
            if (wvcChartPaint || CallerBaseIs(caller, L"rarext.dll"))
                layeredStroke = GdiUtils::LayeredStrokePen::TryCreateSolid(lp, kMmcWvcChartStrokeAlpha);
            if (!layeredStroke)
                drawPen = GdiUtils::PenFromLogPen(lp);
            HGDIOBJ hStroke = layeredStroke ? static_cast<HGDIOBJ>(layeredStroke.get())
                                            : static_cast<HGDIOBJ>(drawPen.get());
            if (!hStroke)
                return o(hdc, x, y);

            wil::unique_hpen nullPen(CreatePen(PS_NULL, 0, 0));
            if (!nullPen)
                return o(hdc, x, y);

            HGDIOBJ old = SelectObject(hdc, nullPen.get());
            o(hdc, x, y);
            SelectObject(hdc, hStroke);
            MoveToEx(hdc, lppt.x, lppt.y, nullptr);
            const BOOL r = o(hdc, x, y);
            SelectObject(hdc, old);

            if (WantMmcChartGdiLog(callerBase, cls, parentCls, wvcChartPaint))
            {
                DebugLog(DBG_NORMAL,
                    L"[GlassCore][Root][MMC] LineTo→glass_pen hdc=0x%p hwndDC=0x%p caller=%s cls=\"%s\" parent=\"%s\" "
                    L"chartTree=%d wvcChart=%d to=%d,%d penClr=0x%06X",
                    hdc, hWnd, callerBase, cls, parentCls, chartTreeCls ? 1 : 0, wvcChartPaint ? 1 : 0, x, y,
                    static_cast<unsigned>(lp.lopnColor & 0xFFFFFF));
            }
            return r;
        }

        BOOL WINAPI Root_MyPolyline(HDC hdc, const POINT* p, int n)
        {
            HMODULE caller = GetCallerModule(_ReturnAddress());
            const std::wstring callerBaseStr = CallerBaseName(caller);
            const wchar_t*   callerBase = callerBaseStr.c_str();

            HWND hWnd = HwndForMmcPaintClassification(hdc, callerBase);
            wchar_t cls[128]{};
            wchar_t parentCls[128]{};
            FillWndAndParentClass(hWnd, cls, _countof(cls), parentCls, _countof(parentCls));
            const bool wvcChartPaint = IsWvcChartPaintSurface(cls, parentCls, caller);
            const bool chartTreeCls = StrEqI(cls, L"ChartGraphClass") || StrEqI(parentCls, L"ChartGraphClass");

            if (hdc && !HwndFromDc(hdc) && IsMmcChartDiagCaller(callerBase) && !hWnd)
            {
                DebugLog(DBG_WARNING,
                    L"[GlassCore][Root][MMC] Polyline: WindowFromDC=NULL и нет session HWND hdc=0x%p caller=%s wvcChart=%d n=%d",
                    hdc, callerBase, wvcChartPaint ? 1 : 0, n);
            }

            auto o = GetRootOrg<OHK_POLYLINE, decltype(&Polyline)>(caller);
            if (!o && wvcChartPaint)
                o = GetRootOrg<OHK_POLYLINE, decltype(&Polyline)>(GetModuleHandleW(L"wvc.dll"));
            if (!o)
            {
                if (WantMmcChartGdiLog(callerBase, cls, parentCls, wvcChartPaint))
                {
                    DebugLog(DBG_WARNING,
                        L"[GlassCore][Root][MMC] Polyline: нет org hdc=0x%p hwndDC=0x%p caller=%s wvcChart=%d n=%d",
                        hdc, hWnd, callerBase, wvcChartPaint ? 1 : 0, n);
                }
                return Polyline(hdc, p, n);
            }
            if (g_shutdown.load(std::memory_order_acquire))
                return o(hdc, p, n);

            if (n <= 0 || !p)
                return o(hdc, p, n);

            if (!wvcChartPaint)
            {
                if (WantMmcChartGdiLog(callerBase, cls, parentCls, false))
                {
                    DebugLog(DBG_NORMAL,
                        L"[GlassCore][Root][MMC] Polyline→orig (wvcChart=0) hdc=0x%p hwndDC=0x%p caller=%s cls=\"%s\" n=%d",
                        hdc, hWnd, callerBase, cls, n);
                }
                return o(hdc, p, n);
            }

            HGDIOBJ penObj = GetCurrentObject(hdc, OBJ_PEN);
            LOGPEN  lp{};
            if (GetObject(penObj, sizeof(lp), &lp) != sizeof(LOGPEN) || lp.lopnStyle == PS_NULL)
                return o(hdc, p, n);

            GdiUtils::LayeredStrokePen layeredStroke =
                GdiUtils::LayeredStrokePen::TryCreateSolid(lp, kMmcWvcChartStrokeAlpha);
            wil::unique_hpen drawPen;
            if (!layeredStroke)
                drawPen = GdiUtils::PenFromLogPen(lp);
            HGDIOBJ hStroke = layeredStroke ? static_cast<HGDIOBJ>(layeredStroke.get())
                                            : static_cast<HGDIOBJ>(drawPen.get());
            if (!hStroke)
                return o(hdc, p, n);

            HGDIOBJ old = SelectObject(hdc, hStroke);
            const BOOL r = o(hdc, p, n);
            SelectObject(hdc, old);

            if (WantMmcChartGdiLog(callerBase, cls, parentCls, wvcChartPaint))
            {
                DebugLog(DBG_NORMAL,
                    L"[GlassCore][Root][MMC] Polyline→glass_pen hdc=0x%p hwndDC=0x%p caller=%s cls=\"%s\" parent=\"%s\" "
                    L"chartTree=%d n=%d penClr=0x%06X",
                    hdc, hWnd, callerBase, cls, parentCls, chartTreeCls ? 1 : 0, n,
                    static_cast<unsigned>(lp.lopnColor & 0xFFFFFF));
            }
            return r;
        }

        BOOL WINAPI Root_MyPolygon(HDC hdc, const POINT* p, int n)
        {
            HMODULE caller = GetCallerModule(_ReturnAddress());
            auto    o = GetRootOrg<OHK_POLYGON, decltype(&Polygon)>(caller);
            if (!o)
                return Polygon(hdc, p, n);
            if (g_shutdown.load(std::memory_order_acquire))
                return o(hdc, p, n);

            if (n <= 0 || !p)
                return o(hdc, p, n);

            if (!CallerBaseIs(caller, L"wdc.dll"))
                return o(hdc, p, n);

            HGDIOBJ penObj = GetCurrentObject(hdc, OBJ_PEN);
            LOGPEN  lp{};
            if (GetObject(penObj, sizeof(lp), &lp) != sizeof(LOGPEN) || lp.lopnStyle == PS_NULL)
                return o(hdc, p, n);

            wil::unique_hpen drawPen = GdiUtils::PenFromLogPen(lp);
            if (!drawPen)
                return o(hdc, p, n);

            HGDIOBJ            brObj = GetCurrentObject(hdc, OBJ_BRUSH);
            LOGBRUSH           lb{};
            wil::unique_hbrush fillBr;
            if (GetObject(brObj, sizeof(lb), &lb))
                fillBr.reset(Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(lb.lbColor, 100));

            HGDIOBJ oldPen = SelectObject(hdc, drawPen.get());
            HGDIOBJ oldBr = nullptr;
            if (fillBr)
                oldBr = SelectObject(hdc, fillBr.get());

            const BOOL r = o(hdc, p, n);

            if (oldBr)
                SelectObject(hdc, oldBr);
            SelectObject(hdc, oldPen);
            return r;
        }

        BOOL WINAPI Root_MyRectangle(HDC hdc, int left, int top, int right, int bottom)
        {
            HMODULE caller = GetCallerModule(_ReturnAddress());
            auto    o = GetRootOrg<OHK_RECTANGLE, decltype(&Rectangle)>(caller);
            if (!o)
                return Rectangle(hdc, left, top, right, bottom);
            if (g_shutdown.load(std::memory_order_acquire))
                return o(hdc, left, top, right, bottom);

            HWND hWnd = HwndFromDcOrMmcSession(hdc, caller);
            if (CallerBaseIs(caller, L"COMDLG32.dll") && hWnd
                && static_cast<ATOM>(GetClassLongPtrW(hWnd, GCW_ATOM)) == static_cast<ATOM>(32770))
            {
                HGDIOBJ h = GetCurrentObject(hdc, OBJ_BRUSH);
                LOGBRUSH lb{};
                if (GetObject(h, sizeof(lb), &lb) && lb.lbStyle == BS_NULL)
                {
                    RECT rc{ left, top, right, bottom };
                    FrameRect(hdc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
                    return TRUE;
                }
            }
            return o(hdc, left, top, right, bottom);
        }

        int WINAPI Root_MyFrameRect(HDC hDC, const RECT* lprc, HBRUSH hbr)
        {
            auto o = GetRootOrg<OHK_FRAMERECT, decltype(&FrameRect)>(GetCallerModule(_ReturnAddress()));
            if (!o)
                return FrameRect(hDC, lprc, hbr);
            if (g_shutdown.load(std::memory_order_acquire))
                return o(hDC, lprc, hbr);

            HWND hWnd = HwndFromDc(hDC);
            wchar_t cls[256]{};
            if (hWnd)
                GetClassNameW(hWnd, cls, _countof(cls));

            HMODULE caller = GetCallerModule(_ReturnAddress());
            if (CallerBaseIs(caller, L"ACLUI.dll") && StrEqI(cls, L"CHECKLIST_ACLUI"))
            {
                LOGBRUSH lb{};
                if (GetObject(hbr, sizeof(lb), &lb)
                    && (0xFFFFFF & lb.lbColor) == (0xFFFFFF & GetSysColor(COLOR_WINDOW))
                    && !lb.lbStyle)
                    hbr = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
            }
            return o(hDC, lprc, hbr);
        }

        // BWM OverallHooks::MyFillRect (solid + hatched): цветные плитки комбобокса
        // и квадратики под рисунками дисков в дисковой утилите.
        int WINAPI Root_MyFillRect(HDC hDC, const RECT* lprc, HBRUSH hbr)
        {
            HMODULE caller = GetCallerModule(_ReturnAddress());
            auto o = GetRootOrg<OHK_FILLRECT, decltype(&FillRect)>(caller);
            if (!o)
                return FillRect(hDC, lprc, hbr);
            if (g_shutdown.load(std::memory_order_acquire))
                return o(hDC, lprc, hbr);

            // D2: memory-DC классифицируем через сессию (иначе cls пустой и фон мимо).
            HWND hWnd = HwndFromDcOrMmcSession(hDC, caller);
            wchar_t cls[256]{};
            if (hWnd)
                GetClassNameW(hWnd, cls, _countof(cls));

            const bool isAfx = StrEqI(cls, L"AfxWnd42u");
            const bool isDmView = CallerBaseIs(caller, L"dmview.ocx");
            const bool isDmdskmgr = CallerBaseIs(caller, L"dmdskmgr.dll");
            const bool isCombo = StrEqI(cls, L"ComboBox") || StrEqI(cls, L"ComboLBox");

            if (!lprc || !hbr)
                return o(hDC, lprc, hbr);

            LOGBRUSH lb{};
            if (!GetObject(hbr, sizeof(lb), &lb))
                return o(hDC, lprc, hbr);

            if (lb.lbStyle == BS_SOLID)
            {
                // BWM OverallHooks:1054-1062 — ветка dmview.ocx + AfxWnd42u
                // (квадратики под рисунками дисков) и dmdskmgr + ComboBox/ComboLBox
                // (цветные плитки на комбобоксе — любой сплошной цвет в alpha-кисть).
                // SysListView32 — в ListViewHooks, здесь ему не место.
                if (lb.lbColor == GetSysColor(COLOR_BTNFACE)
                    || lb.lbColor == GetSysColor(COLOR_HIGHLIGHT)
                    || (isDmView && isAfx)
                    || (isDmdskmgr && isCombo))
                {
                    HBRUSH alphaBrush = Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(lb.lbColor, 255);
                    const int res = alphaBrush ? o(hDC, lprc, alphaBrush) : 0;
                    if (alphaBrush)
                        DeleteObject(alphaBrush);
                    if (alphaBrush)
                        return res;
                }
            }
            else if (lb.lbStyle == BS_HATCHED)
            {
                // BWM OverallHooks:1099-1114 — штриховка плиток: dmview.ocx + AfxWnd42u
                // (тело ниже не трогать) и dmdskmgr + ComboBox/ComboLBox («Узор» diskmgmt,
                // цветные штриховки на комбобоксе — DIB-узор с альфой, без GDI+).
                if (isDmdskmgr && isCombo)
                {
                    PatBlt(hDC, lprc->left, lprc->top,
                        lprc->right - lprc->left, lprc->bottom - lprc->top, BLACKNESS);
                    HBRUSH hatch = CreateRootAlphaHatchBrush(static_cast<int>(lb.lbHatch), lb.lbColor);
                    if (hatch)
                    {
                        const int res = o(hDC, lprc, hatch);
                        DeleteObject(hatch);
                        return res;
                    }
                    return 1;
                }
                if (isDmView && isAfx)
                {
                    PatBlt(hDC, lprc->left, lprc->top,
                        lprc->right - lprc->left, lprc->bottom - lprc->top, BLACKNESS);
                    HBRUSH hatch = CreateBrushIndirect(&lb);
                    if (hatch)
                    {
                        const int res = o(hDC, lprc, hatch);
                        DeleteObject(hatch);
                        return res;
                    }
                    return 1;
                }
            }

            return o(hDC, lprc, hbr);
        }

        BOOL WINAPI Root_MyBitBlt(
            HDC hdc, int x, int y, int cx, int cy,
            HDC hdcSrc, int x1, int y1, DWORD rop)
        {
            HMODULE caller = GetCallerModule(_ReturnAddress());
            auto    o = GetRootOrg<OHK_BITBLT, decltype(&BitBlt)>(caller);
            if (!o)
                return BitBlt(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);
            if (g_shutdown.load(std::memory_order_acquire))
                return o(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);

            HWND hWnd = HwndFromDc(hdc);
            wchar_t cls[256]{};
            if (hWnd)
                GetClassNameW(hWnd, cls, _countof(cls));

            const bool afxToolbar = CallerBaseIs(caller, L"comctl32.dll")
                && StrEqI(cls, L"AfxWnd42u")
                && cx == 16 && cy == 16;

            if (!afxToolbar)
                return o(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);

            // BWM OverallHooks::MyBitBlt: часть rop — успех без копирования
            if (rop == 0x00220326 || rop == 0x00E20746)
                return TRUE;

            if (rop != SRCCOPY && rop != 0x00EE0086)
                return o(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);

            if (!cx || !cy || !hdcSrc)
                return o(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);

            HBITMAP hSrcBitmap = static_cast<HBITMAP>(GetCurrentObject(hdcSrc, OBJ_BITMAP));
            if (!hSrcBitmap || Shared::BitmapUtils::IsOemBitmap(hSrcBitmap))
                return o(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);

            if (Shared::BitmapUtils::HasAlphaChannel(hSrcBitmap))
            {
                BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
                return GdiAlphaBlend(hdc, x, y, cx, cy, hdcSrc, x1, y1, cx, cy, bf);
            }

            wil::unique_hbitmap hConverted{ Shared::BitmapUtils::Promise32BPP(hSrcBitmap) };
            if (!hConverted)
                return o(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);

            // MFC Afx toolbar (16×16) — только black-key (без настраиваемого chroma).
            GlassCore::GdiUtils::ApplyBlackKeyAlpha(hConverted.get());

            HGDIOBJ hOldBitmap = SelectObject(hdcSrc, hConverted.get());
            auto    restoreBitmap = wil::scope_exit([&] { SelectObject(hdcSrc, hOldBitmap); });

            BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            return GdiAlphaBlend(hdc, x, y, cx, cy, hdcSrc, x1, y1, cx, cy, bf);
        }

        BOOL WINAPI Root_MyGradientFill(
            HDC hdc, PTRIVERTEX pVertex, ULONG nVertex, PVOID pMesh, ULONG nCount, ULONG ulMode)
        {
            HMODULE caller = GetCallerModule(_ReturnAddress());
            auto    o = GetRootOrg<OHK_GRADIENTFILL, decltype(&GdiGradientFill)>(caller);
            if (!o)
                return GdiGradientFill(hdc, pVertex, nVertex, pMesh, nCount, ulMode);
            if (g_shutdown.load(std::memory_order_acquire))
                return o(hdc, pVertex, nVertex, pMesh, nCount, ulMode);
            if (!CallerBaseIs(caller, L"rarext.dll"))
                return o(hdc, pVertex, nVertex, pMesh, nCount, ulMode);
            if (!pVertex || !nVertex)
                return o(hdc, pVertex, nVertex, pMesh, nCount, ulMode);

            RECT clipRect{};
            switch (GetClipBox(hdc, &clipRect))
            {
            case NULLREGION:
            case ERROR:
                return o(hdc, pVertex, nVertex, pMesh, nCount, ulMode);
            default:
                break;
            }

            RECT drawRect{};
            if (!RarextComputeGradientDrawRect(pVertex, nVertex, pMesh, nCount, ulMode, clipRect, drawRect))
                return o(hdc, pVertex, nVertex, pMesh, nCount, ulMode);

            const int w = drawRect.right - drawRect.left;
            const int h = drawRect.bottom - drawRect.top;
            if (w <= 0 || h <= 0)
                return o(hdc, pVertex, nVertex, pMesh, nCount, ulMode);

            if (RarextGradientAllSysColors(pVertex, nVertex))
            {
                PatBlt(hdc, drawRect.left, drawRect.top, w, h, BLACKNESS);
                return TRUE;
            }

            wil::unique_hdc memDc{ CreateCompatibleDC(hdc) };
            if (!memDc)
                return o(hdc, pVertex, nVertex, pMesh, nCount, ulMode);

            BITMAPINFO bmi{};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = w;
            bmi.bmiHeader.biHeight = -h;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            void* bits = nullptr;
            wil::unique_hbitmap dib{
                CreateDIBSection(memDc.get(), &bmi, DIB_RGB_COLORS, &bits, nullptr, 0) };
            if (!dib || !bits)
                return o(hdc, pVertex, nVertex, pMesh, nCount, ulMode);

            const HGDIOBJ oldBmp = SelectObject(memDc.get(), dib.get());
            auto        restoreBmp = wil::scope_exit([&] { SelectObject(memDc.get(), oldBmp); });

            ZeroMemory(bits, static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);

            std::vector<TRIVERTEX> vtx(static_cast<size_t>(nVertex));
            for (ULONG i = 0; i < nVertex; ++i)
            {
                vtx[i] = pVertex[i];
                vtx[i].x -= drawRect.left;
                vtx[i].y -= drawRect.top;
                vtx[i].Alpha = 0xFF00;
            }

            if (!o(memDc.get(), vtx.data(), nVertex, pMesh, nCount, ulMode))
                return o(hdc, pVertex, nVertex, pMesh, nCount, ulMode);

            RarextSetOpaqueAlphaOnDib(bits, w, h);

            BLENDFUNCTION bf{};
            bf.BlendOp = AC_SRC_OVER;
            bf.SourceConstantAlpha = kRarextGradientFillAlpha;
            bf.AlphaFormat = AC_SRC_ALPHA;
            if (!GdiAlphaBlend(hdc, drawRect.left, drawRect.top, w, h, memDc.get(), 0, 0, w, h, bf))
                return o(hdc, pVertex, nVertex, pMesh, nCount, ulMode);
            return TRUE;
        }

        BOOL WINAPI Root_MyTextOutW(HDC hdc, int x, int y, LPCWSTR lpString, int c)
        {
            HMODULE caller = GetCallerModule(_ReturnAddress());
            auto    o = GetRootOrg<OHK_TEXTOUT, decltype(&TextOutW)>(caller);
            if (!o)
                return TextOutW(hdc, x, y, lpString, c);
            if (g_shutdown.load(std::memory_order_acquire))
                return o(hdc, x, y, lpString, c);
            if (!CallerBaseIs(caller, L"rarext.dll"))
                return o(hdc, x, y, lpString, c);
            if (!lpString || c <= 0)
                return o(hdc, x, y, lpString, c);

            SIZE sz{};
            if (!GetTextExtentPoint32W(hdc, lpString, c, &sz))
                return o(hdc, x, y, lpString, c);

            RECT rc{ x, y, x + sz.cx, y + sz.cy };
            int  result = 0;
            if (!FAILED(GlassCore::Text::DrawTextWithAlpha(
                    hdc, lpString, c, &rc, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOCLIP, result)))
                return TRUE;
            return o(hdc, x, y, lpString, c);
        }

} // namespace GlassCore::Root
