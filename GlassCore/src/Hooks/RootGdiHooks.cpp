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
#include <Hooks/delphi/TPanelDraw.h>
#include <Hooks/delphi/TButtonGroupsDraw.h>
#include <Hooks/delphi/RestoratorMemoDraw.h>
#include <GlassDelphiButtonGroups.h>
#include <Hooks/RootGdiHooks.h>
#include <Hooks/RootWindowHooks.h>
#include <Core/Intercept/HookDispatcher.h>
#include <Core/Intercept/IATRegistry.h>
#include <Module/ModuleHandler.h>
#include <Subsystems/RenderText.h>
#include <Helpers/ThemeHelper.h>
#include <Subsystems/PaintDispatcher.h>
#include <GlassControlKind.h>
#include <Subsystems/RootSwcaGradient.h>
#include <Subsystems/GdiHookUtils.h>
#include <WinApiUtils.h>
#include <BitmapUtils.h>
#include <DebugLog.h>


#include <iterator>
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
        HRESULT WINAPI Root_MyDelphiDrawThemeTextEx(HTHEME, HDC, int, int, LPCWSTR, int, DWORD, LPRECT, const DTTOPTS*);

        BOOL WINAPI Root_MyDelphiGridText(HDC, int, int, UINT, const RECT*, LPCWSTR, UINT, const INT*);
        int WINAPI Root_MyDelphiGridFill(HDC, const RECT*, HBRUSH);
        HRESULT WINAPI Root_MyDelphiGridTheme(HTHEME, HDC, int, int, LPCRECT, LPCRECT);
        DWORD WINAPI Root_MyGetSysColor(int);
        int WINAPI Root_MyDelphiGroupText(HDC, LPWSTR, int, LPRECT, UINT, LPDRAWTEXTPARAMS);
        int WINAPI Root_MyDelphiGroupFill(HDC, const RECT*, HBRUSH);
        template <size_t slot> BOOL WINAPI Root_MyDelphiGroupGradient(HDC, PTRIVERTEX, ULONG, PVOID, ULONG, ULONG);
        int WINAPI Root_MyRestoratorMemoText(HDC, LPCSTR, int, LPRECT, UINT);
        HBRUSH WINAPI Root_MyRestoratorMemoBrush(const LOGBRUSH*);
        DWORD WINAPI Root_MyRestoratorMemoColor(int);
        int WINAPI Root_MyRestoratorMemoFill(HDC, const RECT*, HBRUSH);

#if defined(_DEBUG) && defined(GLASS_DELPHI_DRAW_PROBE)
        int WINAPI VclProbeFill(HDC, const RECT*, HBRUSH);
        BOOL WINAPI VclProbePatBlt(HDC, int, int, int, int, DWORD);
        BOOL WINAPI VclProbeRect(HDC, int, int, int, int);
        BOOL WINAPI VclProbeRoundRect(HDC, int, int, int, int, int, int);
        BOOL WINAPI VclProbeBitBlt(HDC, int, int, int, int, HDC, int, int, DWORD);
        BOOL WINAPI VclProbeText(HDC, int, int, UINT, const RECT*, LPCWSTR, UINT, const INT*);
        int WINAPI VclProbeDrawText(HDC, LPCWSTR, int, LPRECT, UINT);
        HRESULT WINAPI VclProbeTheme(HTHEME, HDC, int, int, LPCRECT, LPCRECT);
        HBRUSH WINAPI VclProbeCreateBrush(COLORREF);
        HGDIOBJ WINAPI VclProbeSelect(HDC, HGDIOBJ);
        BOOL WINAPI VclProbeEllipse(HDC, int, int, int, int);
        int WINAPI VclProbeDrawTextEx(HDC, LPWSTR, int, LPRECT, UINT, LPDRAWTEXTPARAMS);
        HBRUSH WINAPI VclProbeCreateBrushIndirect(const LOGBRUSH*);
        HRESULT WINAPI VclProbeThemeTextEx(HTHEME, HDC, int, int, LPCWSTR, int, DWORD, LPRECT, const DTTOPTS*);
        BOOL WINAPI VclProbeStretchBlt(HDC, int, int, int, int, HDC, int, int, int, int, DWORD);
        BOOL WINAPI VclProbeFocusRect(HDC, const RECT*);
#endif

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
            std::tuple{ std::array{ "uxtheme.dll"sv, "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv, ""sv },
                "DrawThemeTextEx", reinterpret_cast<PVOID>(Root_MyDelphiDrawThemeTextEx) },
            std::tuple{ std::array{ "gdi32.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
                "ExtTextOutW", reinterpret_cast<PVOID>(Root_MyDelphiGridText) },
            std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
                "FillRect", reinterpret_cast<PVOID>(Root_MyDelphiGridFill) },
            std::tuple{ std::array{ "uxtheme.dll"sv, "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv, ""sv },
                "DrawThemeBackground", reinterpret_cast<PVOID>(Root_MyDelphiGridTheme) },
            std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv },
                "GetSysColor", reinterpret_cast<PVOID>(Root_MyGetSysColor) },
            std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
                "DrawTextExW", reinterpret_cast<PVOID>(Root_MyDelphiGroupText) },
            std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
                "FillRect", reinterpret_cast<PVOID>(Root_MyDelphiGroupFill) },
            std::tuple{ std::array{ "msimg32.dll"sv, ""sv, ""sv },
                "GradientFill", reinterpret_cast<PVOID>(Root_MyDelphiGroupGradient<OHK_DELPHI_GROUP_GRADIENT>) },
            std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv },
                "GdiGradientFill", reinterpret_cast<PVOID>(Root_MyDelphiGroupGradient<OHK_DELPHI_GROUP_GDIGRADIENT>) },
            // Restorator memo route: keep order in sync with its readiness bits.
            std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
                "DrawTextA", reinterpret_cast<PVOID>(Root_MyRestoratorMemoText) },
            std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv },
                "CreateBrushIndirect", reinterpret_cast<PVOID>(Root_MyRestoratorMemoBrush) },
            std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv },
                "GetSysColor", reinterpret_cast<PVOID>(Root_MyRestoratorMemoColor) },
            std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
                "FillRect", reinterpret_cast<PVOID>(Root_MyRestoratorMemoFill) },
#if defined(_DEBUG) && defined(GLASS_DELPHI_DRAW_PROBE)
            std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv }, "FillRect", reinterpret_cast<PVOID>(VclProbeFill) },
            std::tuple{ std::array{ "gdi32.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv }, "PatBlt", reinterpret_cast<PVOID>(VclProbePatBlt) },
            std::tuple{ std::array{ "gdi32.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv }, "Rectangle", reinterpret_cast<PVOID>(VclProbeRect) },
            std::tuple{ std::array{ "gdi32.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv }, "RoundRect", reinterpret_cast<PVOID>(VclProbeRoundRect) },
            std::tuple{ std::array{ "gdi32.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv }, "BitBlt", reinterpret_cast<PVOID>(VclProbeBitBlt) },
            std::tuple{ std::array{ "gdi32.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv }, "ExtTextOutW", reinterpret_cast<PVOID>(VclProbeText) },
            std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv }, "DrawTextW", reinterpret_cast<PVOID>(VclProbeDrawText) },
            std::tuple{ std::array{ "uxtheme.dll"sv, "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv, ""sv }, "DrawThemeBackground", reinterpret_cast<PVOID>(VclProbeTheme) },
            std::tuple{ std::array{ "gdi32.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv }, "CreateSolidBrush", reinterpret_cast<PVOID>(VclProbeCreateBrush) },
            std::tuple{ std::array{ "gdi32.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv }, "SelectObject", reinterpret_cast<PVOID>(VclProbeSelect) },
            std::tuple{ std::array{ "gdi32.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv }, "Ellipse", reinterpret_cast<PVOID>(VclProbeEllipse) },
            std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv }, "DrawTextExW", reinterpret_cast<PVOID>(VclProbeDrawTextEx) },
            std::tuple{ std::array{ "gdi32.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv }, "CreateBrushIndirect", reinterpret_cast<PVOID>(VclProbeCreateBrushIndirect) },
            std::tuple{ std::array{ "uxtheme.dll"sv, "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv, ""sv }, "DrawThemeTextEx", reinterpret_cast<PVOID>(VclProbeThemeTextEx) },
            std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv }, "StretchBlt", reinterpret_cast<PVOID>(VclProbeStretchBlt) },
            std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv }, "DrawFocusRect", reinterpret_cast<PVOID>(VclProbeFocusRect) },
#endif
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

#if defined(_DEBUG) && defined(GLASS_DELPHI_DRAW_PROBE)
        struct VclProbeSession
        {
            HWND hwnd;
            bool paint;
            HMODULE module;
            std::array<bool, OHK_COUNT> acquired{};
        };

        // EnableHook возвращает false как при ошибке, так и при увеличении
        // существующего refcount. Фиксируем реальное приращение под root lock.
        static bool AcquireVclProbeHook(HMODULE module, size_t index) noexcept
        {
            auto lock = g_rootLock.lock_exclusive();
            auto it = g_rootMap.find(module);
            if (it == g_rootMap.end())
            {
                auto [ins, inserted] = g_rootMap.emplace(
                    std::piecewise_construct, std::forward_as_tuple(module),
                    std::forward_as_tuple(g_rootHookDep, module));
                it = ins;
                it->second.CacheHookData();
            }
            const int before = it->second.GetRef(index);
            it->second.EnableHook(index, true);
            return it->second.GetRef(index) > before;
        }
        static thread_local std::vector<VclProbeSession> g_vclProbeStack;

        // Эти passthrough IAT-детуры включаются только на EXE, пока активен
        // BeginPaint/GetDC точного класса; стек — контекст, НЕ доказательство
        // принадлежности произвольного memory DC. Текст не записываем.
        void BeginVclDrawProbe(HWND hwnd, bool paint) noexcept
        {
            if (g_shutdown.load(std::memory_order_acquire) || !hwnd) return;
            WCHAR cls[64]{};
            if (!GetClassNameW(hwnd, cls, _countof(cls))
                || (_wcsicmp(cls, L"TBitBtn") != 0 && _wcsicmp(cls, L"TToggleSwitch") != 0
                    && _wcsicmp(cls, L"TListBox") != 0 && _wcsicmp(cls, L"TCheckListBox") != 0
                    && _wcsicmp(cls, L"TColorListBox") != 0 && _wcsicmp(cls, L"TNumberBox") != 0
                    && _wcsicmp(cls, L"THotKey") != 0 && _wcsicmp(cls, L"TRichEdit") != 0))
                return;
            HMODULE ep = GlassCore::ModuleHandler::g_entryPointModule;
            if (!ep) return;
            VclProbeSession session{ hwnd, paint, ep };
            unsigned armed = 0;
            for (size_t i = OHK_VCLPROBE_FILL; i <= OHK_VCLPROBE_FOCUSRECT; ++i)
            {
                session.acquired[i] = AcquireVclProbeHook(ep, i);
                if (session.acquired[i]) ++armed;
            }
            g_vclProbeStack.push_back(session);
            DebugLog(DBG_NORMAL, L"[GlassCore][DelphiDrawProbe] Begin hwnd=0x%p class=%s channel=%s exe=0x%p acquired=%u depth=%zu",
                hwnd, cls, paint ? L"paint" : L"getdc", ep, armed, g_vclProbeStack.size());
        }

        void EndVclDrawProbe(HWND hwnd, bool paint) noexcept
        {
            for (auto it = g_vclProbeStack.rbegin(); it != g_vclProbeStack.rend(); ++it)
            {
                if (it->hwnd != hwnd || it->paint != paint) continue;
                const VclProbeSession session = *it;
                g_vclProbeStack.erase((it + 1).base());
                for (size_t i = OHK_VCLPROBE_FILL; i <= OHK_VCLPROBE_FOCUSRECT; ++i)
                    if (session.acquired[i]) EnableRootHook(session.module, i, false);
                DebugLog(DBG_NORMAL, L"[GlassCore][DelphiDrawProbe] End hwnd=0x%p channel=%s depth=%zu",
                    hwnd, paint ? L"paint" : L"getdc", g_vclProbeStack.size());
                return;
            }
        }

        void ClearVclDrawProbe() noexcept { g_vclProbeStack.clear(); }

        static void LogVclProbe(const wchar_t* api, HDC dc, const RECT* rect,
            HGDIOBJ object, HDC src, DWORD flags, int a = 0, int b = 0, int c = 0, int d = 0) noexcept
        {
            if (g_shutdown.load(std::memory_order_acquire) || g_vclProbeStack.empty()) return;
            static thread_local ULONGLONG epoch = 0;
            static thread_local unsigned budget = 0;
            const ULONGLONG now = GetTickCount64();
            if (!epoch || now - epoch >= 1000) { epoch = now; budget = 160; }
            if (!budget) return;
            --budget;
            const auto& session = g_vclProbeStack.back();
            WCHAR cls[64]{};
            GetClassNameW(session.hwnd, cls, _countof(cls));
            const RECT r = rect ? *rect : RECT{};
            RECT clip{};
            const int clipKind = dc ? GetClipBox(dc, &clip) : 0;
            HGDIOBJ brush = object;
            if (!brush && dc) brush = GetCurrentObject(dc, OBJ_BRUSH);
            LOGBRUSH lb{};
            const bool brushValid = brush && GetObjectType(brush) == OBJ_BRUSH
                && GetObjectW(brush, sizeof(lb), &lb) == sizeof(lb);
            BITMAP destBitmap{};
            const HGDIOBJ destObject = dc ? GetCurrentObject(dc, OBJ_BITMAP) : nullptr;
            const bool destBitmapValid = destObject && GetObjectW(destObject, sizeof(destBitmap), &destBitmap) == sizeof(destBitmap);
            LOGPEN pen{};
            const HGDIOBJ penObject = dc ? GetCurrentObject(dc, OBJ_PEN) : nullptr;
            const bool penValid = penObject && GetObjectW(penObject, sizeof(pen), &pen) == sizeof(pen);
            BITMAP sourceBitmap{};
            const HGDIOBJ bitmap = src ? GetCurrentObject(src, OBJ_BITMAP) : nullptr;
            const bool bitmapValid = bitmap && GetObjectW(bitmap, sizeof(sourceBitmap), &sourceBitmap) == sizeof(sourceBitmap);
            DebugLog(DBG_NORMAL,
                L"[GlassCore][DelphiDrawProbe] api=%s hwnd=0x%p class=%s channel=%s dc=0x%p dcWnd=0x%p dcType=%u destBitmap=0x%p destValid=%d destSize=%ldx%ld destBpp=%u pen=0x%p penValid=%d penStyle=%u penWidth=%ld penColor=0x%08X src=0x%p srcWnd=0x%p bitmap=0x%p bitmapValid=%d bitmapSize=%ldx%ld bpp=%u obj=0x%p objType=%u brush=0x%p brushValid=%d brushStyle=%u brushColor=0x%08X rect=[%ld,%ld,%ld,%ld] clipKind=%d clip=[%ld,%ld,%ld,%ld] flags=0x%08X a=%d b=%d c=%d d=%d",
                api, session.hwnd, cls, session.paint ? L"paint" : L"getdc", dc,
                dc ? WindowFromDC(dc) : nullptr, dc ? static_cast<unsigned>(GetObjectType(dc)) : 0,
                destObject, destBitmapValid ? 1 : 0, destBitmap.bmWidth, destBitmap.bmHeight,
                static_cast<unsigned>(destBitmap.bmBitsPixel), penObject, penValid ? 1 : 0,
                static_cast<unsigned>(pen.lopnStyle), pen.lopnWidth.x, static_cast<unsigned>(pen.lopnColor),
                src, src ? WindowFromDC(src) : nullptr, bitmap, bitmapValid ? 1 : 0,
                sourceBitmap.bmWidth, sourceBitmap.bmHeight, static_cast<unsigned>(sourceBitmap.bmBitsPixel), object,
                object ? static_cast<unsigned>(GetObjectType(object)) : 0,
                brush, brushValid ? 1 : 0, static_cast<unsigned>(lb.lbStyle), static_cast<unsigned>(lb.lbColor),
                r.left, r.top, r.right, r.bottom, clipKind, clip.left, clip.top, clip.right, clip.bottom,
                static_cast<unsigned>(flags), a, b, c, d);
        }

        int WINAPI VclProbeFill(HDC dc, const RECT* r, HBRUSH brush)
        {
            auto orig = GetRootOrg<OHK_VCLPROBE_FILL, decltype(&FillRect)>(GlassCore::ModuleHandler::g_entryPointModule);
            LogVclProbe(L"FillRect", dc, r, brush, nullptr, 0);
            return orig ? orig(dc, r, brush) : FillRect(dc, r, brush);
        }
        BOOL WINAPI VclProbePatBlt(HDC dc, int x, int y, int w, int h, DWORD rop)
        {
            auto orig = GetRootOrg<OHK_VCLPROBE_PATBLT, decltype(&PatBlt)>(GlassCore::ModuleHandler::g_entryPointModule);
            const RECT r{ x, y, x + w, y + h };
            LogVclProbe(L"PatBlt", dc, &r, nullptr, nullptr, rop);
            return orig ? orig(dc, x, y, w, h, rop) : PatBlt(dc, x, y, w, h, rop);
        }
        BOOL WINAPI VclProbeRect(HDC dc, int l, int t, int r, int b)
        {
            auto orig = GetRootOrg<OHK_VCLPROBE_RECT, decltype(&Rectangle)>(GlassCore::ModuleHandler::g_entryPointModule);
            const RECT rc{ l, t, r, b }; LogVclProbe(L"Rectangle", dc, &rc, nullptr, nullptr, 0);
            return orig ? orig(dc, l, t, r, b) : Rectangle(dc, l, t, r, b);
        }
        BOOL WINAPI VclProbeRoundRect(HDC dc, int l, int t, int r, int b, int w, int h)
        {
            auto orig = GetRootOrg<OHK_VCLPROBE_ROUNDRECT, decltype(&RoundRect)>(GlassCore::ModuleHandler::g_entryPointModule);
            const RECT rc{ l, t, r, b }; LogVclProbe(L"RoundRect", dc, &rc, nullptr, nullptr, 0, w, h);
            return orig ? orig(dc, l, t, r, b, w, h) : RoundRect(dc, l, t, r, b, w, h);
        }
        BOOL WINAPI VclProbeEllipse(HDC dc, int l, int t, int r, int b)
        {
            auto orig = GetRootOrg<OHK_VCLPROBE_ELLIPSE, decltype(&Ellipse)>(GlassCore::ModuleHandler::g_entryPointModule);
            const RECT rc{ l, t, r, b }; LogVclProbe(L"Ellipse", dc, &rc, nullptr, nullptr, 0);
            return orig ? orig(dc, l, t, r, b) : Ellipse(dc, l, t, r, b);
        }
        BOOL WINAPI VclProbeBitBlt(HDC dc, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop)
        {
            auto orig = GetRootOrg<OHK_VCLPROBE_BITBLT, decltype(&BitBlt)>(GlassCore::ModuleHandler::g_entryPointModule);
            const RECT r{ x, y, x + w, y + h }; LogVclProbe(L"BitBlt", dc, &r, nullptr, src, rop, sx, sy);
            return orig ? orig(dc, x, y, w, h, src, sx, sy, rop) : BitBlt(dc, x, y, w, h, src, sx, sy, rop);
        }
        BOOL WINAPI VclProbeText(HDC dc, int x, int y, UINT options, const RECT* r, LPCWSTR text, UINT count, const INT* dx)
        {
            auto orig = GetRootOrg<OHK_VCLPROBE_TEXT, decltype(&ExtTextOutW)>(GlassCore::ModuleHandler::g_entryPointModule);
            LogVclProbe(L"ExtTextOutW", dc, r, nullptr, nullptr, options, x, y);
            return orig ? orig(dc, x, y, options, r, text, count, dx) : ExtTextOutW(dc, x, y, options, r, text, count, dx);
        }
        int WINAPI VclProbeDrawText(HDC dc, LPCWSTR text, int count, LPRECT r, UINT flags)
        {
            auto orig = GetRootOrg<OHK_VCLPROBE_DRAWTEXT, decltype(&DrawTextW)>(GlassCore::ModuleHandler::g_entryPointModule);
            LogVclProbe(L"DrawTextW", dc, r, nullptr, nullptr, flags, count);
            return orig ? orig(dc, text, count, r, flags) : DrawTextW(dc, text, count, r, flags);
        }
        int WINAPI VclProbeDrawTextEx(HDC dc, LPWSTR text, int count, LPRECT r, UINT flags, LPDRAWTEXTPARAMS params)
        {
            auto orig = GetRootOrg<OHK_VCLPROBE_DRAWTEXTEX, decltype(&DrawTextExW)>(GlassCore::ModuleHandler::g_entryPointModule);
            LogVclProbe(L"DrawTextExW", dc, r, nullptr, nullptr, flags, count);
            return orig ? orig(dc, text, count, r, flags, params) : DrawTextExW(dc, text, count, r, flags, params);
        }
        HRESULT WINAPI VclProbeTheme(HTHEME theme, HDC dc, int part, int state, LPCRECT r, LPCRECT clip)
        {
            auto orig = GetRootOrg<OHK_VCLPROBE_THEME, decltype(&DrawThemeBackground)>(GlassCore::ModuleHandler::g_entryPointModule);
            LogVclProbe(L"DrawThemeBackground", dc, r, nullptr, nullptr, 0, part, state);
            return orig ? orig(theme, dc, part, state, r, clip) : DrawThemeBackground(theme, dc, part, state, r, clip);
        }
        HRESULT WINAPI VclProbeThemeTextEx(HTHEME theme, HDC dc, int part, int state, LPCWSTR text, int count, DWORD flags, LPRECT r, const DTTOPTS* options)
        {
            auto orig = GetRootOrg<OHK_VCLPROBE_THEMETEXTEX, decltype(&DrawThemeTextEx)>(GlassCore::ModuleHandler::g_entryPointModule);
            LogVclProbe(L"DrawThemeTextEx", dc, r, nullptr, nullptr, flags, part, state);
            return orig ? orig(theme, dc, part, state, text, count, flags, r, options) : DrawThemeTextEx(theme, dc, part, state, text, count, flags, r, options);
        }
        HBRUSH WINAPI VclProbeCreateBrush(COLORREF color)
        {
            auto orig = GetRootOrg<OHK_VCLPROBE_CREATEBRUSH, decltype(&CreateSolidBrush)>(GlassCore::ModuleHandler::g_entryPointModule);
            HBRUSH result = orig ? orig(color) : CreateSolidBrush(color);
            LogVclProbe(L"CreateSolidBrush", nullptr, nullptr, result, nullptr, static_cast<DWORD>(color));
            return result;
        }
        HBRUSH WINAPI VclProbeCreateBrushIndirect(const LOGBRUSH* lb)
        {
            auto orig = GetRootOrg<OHK_VCLPROBE_CREATEBRUSHINDIRECT, decltype(&CreateBrushIndirect)>(GlassCore::ModuleHandler::g_entryPointModule);
            HBRUSH result = orig ? orig(lb) : CreateBrushIndirect(lb);
            LogVclProbe(L"CreateBrushIndirect", nullptr, nullptr, result, nullptr, lb ? lb->lbStyle : 0);
            return result;
        }
        HGDIOBJ WINAPI VclProbeSelect(HDC dc, HGDIOBJ obj)
        {
            auto orig = GetRootOrg<OHK_VCLPROBE_SELECT, decltype(&SelectObject)>(GlassCore::ModuleHandler::g_entryPointModule);
            LogVclProbe(L"SelectObject", dc, nullptr, obj, nullptr, 0);
            return orig ? orig(dc, obj) : SelectObject(dc, obj);
        }
        BOOL WINAPI VclProbeStretchBlt(HDC dc, int x, int y, int w, int h,
            HDC src, int sx, int sy, int sw, int sh, DWORD rop)
        {
            auto orig = GetRootOrg<OHK_VCLPROBE_STRETCHBLT, decltype(&StretchBlt)>(GlassCore::ModuleHandler::g_entryPointModule);
            const RECT r{ x, y, x + w, y + h };
            LogVclProbe(L"StretchBlt", dc, &r, nullptr, src, rop, sx, sy, sw, sh);
            return orig ? orig(dc, x, y, w, h, src, sx, sy, sw, sh, rop)
                : StretchBlt(dc, x, y, w, h, src, sx, sy, sw, sh, rop);
        }
        BOOL WINAPI VclProbeFocusRect(HDC dc, const RECT* r)
        {
            auto orig = GetRootOrg<OHK_VCLPROBE_FOCUSRECT, decltype(&DrawFocusRect)>(GlassCore::ModuleHandler::g_entryPointModule);
            LogVclProbe(L"DrawFocusRect", dc, r, nullptr, nullptr, 0);
            return orig ? orig(dc, r) : DrawFocusRect(dc, r);
        }
#endif

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

        // Restorator memo acquisition is deliberately separate from EnableSpecs:
        // EnableHook(false) is not paired with a failed enable (nor is its return
        // value an acquisition indicator when a nested ref already exists).
        static constexpr size_t kRestoratorMemoSlots[] = {
            OHK_RESTORATOR_MEMO_TEXT, OHK_RESTORATOR_MEMO_BRUSH,
            OHK_RESTORATOR_MEMO_COLOR, OHK_RESTORATOR_MEMO_FILL };
        unsigned AcquireRestoratorMemoHooks() noexcept
        {
            void* ep = GlassCore::ModuleHandler::g_entryPointModule;
            if (!ep || g_shutdown.load(std::memory_order_acquire)) return 0;
            auto lock = g_rootLock.lock_exclusive();
            auto it = g_rootMap.find(ep);
            if (it == g_rootMap.end())
            {
                auto [inserted, ok] = g_rootMap.emplace(std::piecewise_construct,
                    std::forward_as_tuple(ep), std::forward_as_tuple(g_rootHookDep, ep));
                it = inserted;
                it->second.CacheHookData();
            }
            unsigned mask = 0;
            for (size_t i = 0; i < std::size(kRestoratorMemoSlots); ++i)
            {
                const size_t slot = kRestoratorMemoSlots[i];
                const int before = it->second.GetRef(slot);
                it->second.EnableHook(slot, true);
                if (it->second.GetRef(slot) > before) mask |= 1u << i;
            }
            if (mask != GlassCore::RestoratorMemo::kReadyMask)
                for (size_t i = std::size(kRestoratorMemoSlots); i-- > 0;)
                    if (mask & (1u << i)) it->second.EnableHook(kRestoratorMemoSlots[i], false);
            return mask;
        }
        void ReleaseRestoratorMemoHooks() noexcept
        {
            auto lock = g_rootLock.lock_exclusive();
            const auto it = g_rootMap.find(GlassCore::ModuleHandler::g_entryPointModule);
            if (it == g_rootMap.end()) return;
            for (size_t i = std::size(kRestoratorMemoSlots); i-- > 0;)
                it->second.EnableHook(kRestoratorMemoSlots[i], false);
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

        // IAT является общим для процесса: не изменять чужой DC или memory DC,
        // даже если они вызваны во время paint этой таблицы.
        // VCL-таблицы рисуют себя сами из EXE (TCustomGrid.WMPaint → TCanvas):
        // TShownValueList (msinfo), TStringGrid/TDrawGrid — одна и та же процедура.
        // Низкая альфа для ячеек таблиц, цвет которых отличается от COLOR_WINDOW:
        // цвет виден, но стекло просвечивает (тот же порядок, что у темы/комбо).
        constexpr BYTE kDelphiGridCellAlpha = 100;

        [[nodiscard]] static bool IsDelphiGridClass(const wchar_t* cls) noexcept
        {
            return StrEqI(cls, L"TShownValueList")
                || StrEqI(cls, L"TStringGrid")
                || StrEqI(cls, L"TDrawGrid")
                || StrEqI(cls, L"TValueListEditor")
                || StrEqI(cls, L"TControlList");
        }

        // Заливка прямоугольника цветом с истинной альфой: GDI не переносит
        // альфу из pattern-кисти (FillRect с alpha-кистью даёт непрозрачный
        // цвет — см. скриншот «альфа 12»). Поэтому 1x1 premultiplied DIB и
        // GdiAlphaBlend с растяжением на весь прямоугольник.
        static bool FillRectWithAlphaBlend(HDC dc, const RECT* rect,
            COLORREF color, BYTE alpha) noexcept
        {
            if (!dc || !rect)
                return false;
            const int w = rect->right - rect->left;
            const int h = rect->bottom - rect->top;
            if (w <= 0 || h <= 0 || !alpha)
                return false;

            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = 1;
            bi.bmiHeader.biHeight = -1;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;

            void* bits = nullptr;
            HDC mem = CreateCompatibleDC(dc);
            if (!mem)
                return false;
            HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (!bmp || !bits)
            {
                if (bmp) DeleteObject(bmp);
                DeleteDC(mem);
                return false;
            }
            const HGDIOBJ old = SelectObject(mem, bmp);
            const auto premul = [alpha](BYTE c) noexcept
            {
                return static_cast<BYTE>((c * (alpha + 1)) >> 8);
            };
            auto* px = static_cast<BYTE*>(bits);
            px[0] = premul(GetBValue(color));
            px[1] = premul(GetGValue(color));
            px[2] = premul(GetRValue(color));
            px[3] = alpha;

            const BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            const BOOL ok = GdiAlphaBlend(dc, rect->left, rect->top, w, h, mem, 0, 0, 1, 1, bf);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
            return ok != FALSE;
        }

        // Фон ячейки таблицы: сначала чёрная очистка (прозрачность), затем —
        // если цвет не COLOR_WINDOW — цветная подложка с альфой.
        static bool ApplyDelphiGridCellBackground(HDC dc, const RECT* rect, COLORREF color) noexcept
        {
            if (!dc || !rect || IsRectEmpty(rect))
                return false;
            if (!PatBlt(dc, rect->left, rect->top, rect->right - rect->left,
                    rect->bottom - rect->top, BLACKNESS))
                return false;
            const COLORREF plain = color & 0xFFFFFF;
            if (!plain || plain == (GetSysColor(COLOR_WINDOW) & 0xFFFFFF))
                return true;
            return FillRectWithAlphaBlend(dc, rect, color, kDelphiGridCellAlpha);
        }

        static bool IsDelphiGridPaintDC(HDC dc) noexcept
        {
            if (!dc || GlassCore::PaintDispatcher::IsForeignPaintBarrierActive())
                return false;
            // VCL может рисовать таблицу в промежуточный bitmap (двойная
            // буферизация): WindowFromDC == NULL. Тогда владельцем считаем
            // окно активной paint-сессии, если это таблица. Оконный DC
            // по-прежнему должен принадлежать именно ей.
            const HWND host = GlassCore::PaintDispatcher::GetPaintSessionHostHwnd();
            if (!host)
                return false;
            const HWND dcWnd = WindowFromDC(dc);
            if (dcWnd && dcWnd != host)
                return false;
            wchar_t cls[128]{};
            return GetClassNameW(host, cls, _countof(cls)) && IsDelphiGridClass(cls);
        }

        // Заливка окна таблицы вне paint-сессии: BeginPaint сам посылает
        // WM_ERASEBKGND до того, как PaintDispatcher положит окно в стек,
        // поэтому GetPaintSessionHostHwnd() ещё не указывает на таблицу.
        // Фон стирания (FixedColor) иначе остаётся непрозрачным.
        static bool IsDelphiGridFillDC(HDC dc) noexcept
        {
            if (!dc || GlassCore::PaintDispatcher::IsForeignPaintBarrierActive())
                return false;
            if (IsDelphiGridPaintDC(dc))
                return true;
            const HWND w = WindowFromDC(dc);
            if (!w)
                return false;
            wchar_t cls[128]{};
            return GetClassNameW(w, cls, _countof(cls)) && IsDelphiGridClass(cls);
        }

        // ── Реконструкция фона фиксированной области таблицы ─────────────────
        // Фон клиента (TCustomGrid.Paint: GetSysColor → CreateBrushIndirect →
        // FillRect(ClientRect)) приходит мимо наших IAT-слотов: ни перехватить,
        // ни перекрасить его нельзя. Поэтому после отрисовки чистим
        // фиксированную строку/колонку чёрным и заново рисуем их текст —
        // записи текста собраны в Root_MyDelphiGridText, границы области берём
        // из первой перехваченной заливки ячейки (левый верхний угол данных).
        struct GridTextRec
        {
            int      x{};
            int      y{};
            UINT     options{};
            RECT     rect{};
            std::wstring text;
            COLORREF color{};
            HFONT    font{};
        };
        struct GridPaintRec
        {
            HWND                    hwnd{};
            HDC                     dc{};
            bool                    haveDataOrigin{};
            POINT                   dataOrigin{};
            std::vector<GridTextRec> texts;
        };
        static thread_local std::vector<GridPaintRec> g_gridPaints;
        // Левый верхний угол области данных, запомненный по прошлым отрисовкам:
        // частичная перерисовка (клик, активация окна) может не содержать ни
        // одной заливки ячейки, и тогда границы фиксированной области брать неоткуда.
        // -1 = «неизвестно»; 0 значит «фиксированной полосы нет» (у
        // TValueListEditor данные начинаются с x=0 — фиксированной колонки нет).
        static thread_local std::unordered_map<HWND, POINT> g_gridDataOrigins;

        static void RememberGridDataOrigin(HWND wnd, LONG x, LONG y) noexcept
        {
            if (!wnd || x < 0 || y < 0)
                return;
            if (g_gridDataOrigins.size() > 64)
                g_gridDataOrigins.clear();
            POINT& p = g_gridDataOrigins.try_emplace(wnd, POINT{ -1, -1 }).first->second;
            if (p.x < 0 || x < p.x)
                p.x = x;
            if (p.y < 0 || y < p.y)
                p.y = y;
        }

        static GridPaintRec* FindGridPaint(HDC dc, HWND wnd) noexcept
        {
            if (!wnd || g_gridPaints.empty())
                return nullptr;
            auto& rec = g_gridPaints.back();
            if (rec.hwnd != wnd)
                return nullptr;
            if (!rec.dc)
                rec.dc = dc;
            return &rec;
        }

        static void ReplayGridFixedArea(const GridPaintRec& rec) noexcept
        {
            if (!rec.hwnd || !rec.dc || rec.texts.empty())
                return;
            RECT client{};
            if (!GetClientRect(rec.hwnd, &client) || IsRectEmpty(&client))
                return;
            LONG x0 = rec.haveDataOrigin ? rec.dataOrigin.x : -1;
            LONG y0 = rec.haveDataOrigin ? rec.dataOrigin.y : -1;
            {
                const auto it = g_gridDataOrigins.find(rec.hwnd);
                if (it != g_gridDataOrigins.end())
                {
                    if (x0 < 0) x0 = it->second.x;
                    if (y0 < 0) y0 = it->second.y;
                }
            }
            // У TStringGrid/TDrawGrid есть и фиксированная строка, и колонка;
            // у TValueListEditor — только строка (данные начинаются с x=0).
            const bool haveRow = y0 > client.top && y0 < client.bottom;
            const bool haveCol = x0 > client.left && x0 < client.right;
            if (!haveRow && !haveCol)
                return;
            HDC dc = rec.dc;
            const int saved = SaveDC(dc);
            const RECT rowStrip{ client.left, client.top, client.right,
                haveRow ? y0 : client.top };
            const RECT colStrip{ client.left, client.top,
                haveCol ? x0 : client.left, client.bottom };
            if (haveRow)
                PatBlt(dc, client.left, client.top, client.right - client.left,
                    y0 - client.top, BLACKNESS);
            if (haveCol)
                PatBlt(dc, client.left, client.top, x0 - client.left,
                    client.bottom - client.top, BLACKNESS);
            size_t drawn = 0;
            for (const auto& t : rec.texts)
            {
                RECT tmp{};
                if ((!haveRow || !IntersectRect(&tmp, &t.rect, &rowStrip))
                    && (!haveCol || !IntersectRect(&tmp, &t.rect, &colStrip)))
                    continue;
                const HGDIOBJ oldFont = t.font ? SelectObject(dc, t.font) : nullptr;
                const COLORREF oldColor = SetTextColor(dc, t.color);
                const int oldBk = SetBkMode(dc, TRANSPARENT);
                // Тот же путь, что и при обычной отрисовке: чистый GDI-текст
                // чёрным на прозрачном фоне уходит в color key и пропадает.
                const UINT drawOpt = t.options & ~static_cast<UINT>(ETO_OPAQUE);
                const HRESULT hr = GlassCore::Text::ExtTextOutWithAlpha(dc, t.x, t.y,
                    drawOpt, &t.rect, t.text.c_str(), static_cast<UINT>(t.text.size()),
                    nullptr, &::ExtTextOutW);
                if (FAILED(hr))
                    ExtTextOutW(dc, t.x, t.y, drawOpt, &t.rect, t.text.c_str(),
                        static_cast<UINT>(t.text.size()), nullptr);
                SetBkMode(dc, oldBk);
                SetTextColor(dc, oldColor);
                if (oldFont)
                    SelectObject(dc, oldFont);
                ++drawn;
            }
            if (saved)
                RestoreDC(dc, saved);
#if defined(_DEBUG)
            if (drawn)
                DebugLog(DBG_NORMAL,
                    L"[GlassCore][DelphiGrid] FixedReplay hwnd=0x%p row=%ld col=%ld texts=%zu",
                    rec.hwnd, y0, x0, drawn);
#endif
        }

        static void GridPaintBegin(HWND hWnd, HMODULE /*callerMod*/) noexcept
        {
            if (g_shutdown.load(std::memory_order_acquire) || !hWnd)
                return;
            wchar_t cls[128]{};
            if (!GetClassNameW(hWnd, cls, _countof(cls)) || !IsDelphiGridClass(cls))
                return;
            // Повторный BeginPaint того же окна (EndPaint мог не дойти до нас)
            // не плодит записи: реконструкция идёт по свежим данным.
            if (!g_gridPaints.empty() && g_gridPaints.back().hwnd == hWnd)
                g_gridPaints.back() = GridPaintRec{ hWnd };
            else
            {
                while (g_gridPaints.size() >= 8)
                    g_gridPaints.erase(g_gridPaints.begin());
                g_gridPaints.push_back(GridPaintRec{ hWnd });
            }
            g_gridPaints.back().hwnd = hWnd;
        }

        static void GridPaintEnd(HWND hWnd, HMODULE /*callerMod*/) noexcept
        {
            for (auto it = g_gridPaints.rbegin(); it != g_gridPaints.rend(); ++it)
            {
                if (it->hwnd != hWnd)
                    continue;
                GridPaintRec rec = std::move(*it);
                g_gridPaints.erase((it + 1).base());
                if (!g_shutdown.load(std::memory_order_acquire))
                    ReplayGridFixedArea(rec);
                return;
            }
        }

        // Окно текущей paint-сессии — таблица? (без DC: для GetSysColor его нет)
        static bool IsDelphiGridPaintHost() noexcept
        {
            const HWND host = GlassCore::PaintDispatcher::GetPaintSessionHostHwnd();
            if (!host)
                return false;
            wchar_t cls[128]{};
            return GetClassNameW(host, cls, _countof(cls)) && IsDelphiGridClass(cls);
        }

#if defined(_DEBUG)
        static bool AllowDelphiGridLog(unsigned channel) noexcept
        {
            static thread_local ULONGLONG epoch[3]{};
            static thread_local unsigned budget[3]{};
            const ULONGLONG now = GetTickCount64();
            if (!epoch[channel] || now - epoch[channel] >= 1000)
            {
                epoch[channel] = now;
                budget[channel] = 16;
            }
            if (!budget[channel]) return false;
            --budget[channel];
            return true;
        }
#endif

        BOOL WINAPI Root_MyDelphiGridText(HDC dc, int x, int y, UINT options,
            const RECT* rect, LPCWSTR text, UINT count, const INT* dx)
        {
            auto orig = GetRootOrg<OHK_DELPHI_GRID_TEXT, decltype(&ExtTextOutW)>(GetCallerModule(_ReturnAddress()));
            if (!orig) return ExtTextOutW(dc, x, y, options, rect, text, count, dx);
            if (g_shutdown.load(std::memory_order_acquire) || !IsDelphiGridPaintDC(dc)
                || (GetTextAlign(dc) & TA_UPDATECP) || (options & (ETO_GLYPH_INDEX | ETO_PDY))
                || ((options & ETO_OPAQUE) && !rect))
                return orig(dc, x, y, options, rect, text, count, dx);
            // VCL рисует свои заголовки до текста; DrawThemeBackground может
            // вызываться не через IAT EXE. В наблюдённом пути caption приходит
            // с ETO_CLIPPED и прямоугольником первого ряда (top=0). Очистить
            // только его до текста, не трогая data rows и произвольные окна.
            if (rect && text && count)
            {
                if (auto* rec = FindGridPaint(dc, WindowFromDC(dc)))
                {
                    if (rec->texts.size() < 512)
                    {
                        GridTextRec recText{};
                        recText.x = x;
                        recText.y = y;
                        recText.options = options;
                        recText.rect = *rect;
                        recText.text.assign(text, count);
                        recText.color = GetTextColor(dc);
                        recText.font = static_cast<HFONT>(GetCurrentObject(dc, OBJ_FONT));
                        rec->texts.push_back(std::move(recText));
                    }
                }
            }
            if (options == ETO_CLIPPED && rect && rect->top == 0
                && !IsRectEmpty(rect) && x >= rect->left && x < rect->right
                && y >= rect->top && y < rect->bottom)
            {
                const BOOL cleared = PatBlt(dc, rect->left, rect->top,
                    rect->right - rect->left, rect->bottom - rect->top, BLACKNESS);
#if defined(_DEBUG)
                if (AllowDelphiGridLog(2))
                    DebugLog(DBG_NORMAL, L"[GlassCore][DelphiGrid] HeaderTextClear hwnd=0x%p rect=[%ld,%ld,%ld,%ld] ok=%d",
                        WindowFromDC(dc), rect->left, rect->top, rect->right, rect->bottom, cleared);
#endif
                (void)cleared;
            }
            // ETO_OPAQUE заливает фон под текстом цветом текущего BkColor.
            // Раньше здесь был только BLACKNESS — он стирал цветную подложку,
            // нарисованную FillRect до текста (ячейки становились невидимыми).
            if ((options & ETO_OPAQUE) && (!rect || IsRectEmpty(rect)
                || !ApplyDelphiGridCellBackground(dc, rect, GetBkColor(dc))))
                return orig(dc, x, y, options, rect, text, count, dx);
            if (!count) return TRUE;
            if (!text) return orig(dc, x, y, options, rect, text, count, dx);
            const UINT drawOptions = options & ~static_cast<UINT>(ETO_OPAQUE);
            const HRESULT hr = GlassCore::Text::ExtTextOutWithAlpha(dc, x, y, drawOptions,
                rect, text, count, dx, orig);
#if defined(_DEBUG)
            if (AllowDelphiGridLog(0))
                DebugLog(DBG_NORMAL, L"[GlassCore][DelphiGrid] Text hwnd=0x%p options=0x%X count=%u rect=[%ld,%ld,%ld,%ld] hr=0x%08lX",
                    WindowFromDC(dc), options, count,
                    rect ? rect->left : 0, rect ? rect->top : 0,
                    rect ? rect->right : 0, rect ? rect->bottom : 0,
                    static_cast<unsigned long>(hr));
#endif
            if (SUCCEEDED(hr)) return TRUE;
            return orig(dc, x, y, drawOptions, rect, text, count, dx);
        }

        int WINAPI Root_MyDelphiGridFill(HDC dc, const RECT* rect, HBRUSH brush)
        {
            const HMODULE callerMod = GetCallerModule(_ReturnAddress());
            auto orig = GetRootOrg<OHK_DELPHI_GRID_FILL, decltype(&FillRect)>(callerMod);
#if defined(_DEBUG)
            // Вход в детур виден всегда, в том числе когда orig для этого caller'а
            // не найден (иначе такие вызовы уходили в FillRect молча — так
            // пропадала заливка всего клиента таблицы).
            if (AllowDelphiGridLog(0))
            {
                wchar_t dcCls[128]{};
                const HWND dcWnd = WindowFromDC(dc);
                if (dcWnd) GetClassNameW(dcWnd, dcCls, _countof(dcCls));
                LOGBRUSH elb{};
                const bool eok = brush && GetObjectW(brush, sizeof(elb), &elb) == sizeof(elb);
                DebugLog(DBG_NORMAL,
                    L"[GlassCore][DelphiGrid] FillEnter dc=0x%p dcWnd=0x%p dcCls=\"%s\" rect=[%ld,%ld,%ld,%ld] style=%d color=0x%08X gate=%d orig=%d caller=%s",
                    dc, dcWnd, dcCls,
                    rect ? rect->left : 0, rect ? rect->top : 0, rect ? rect->right : 0, rect ? rect->bottom : 0,
                    eok ? static_cast<int>(elb.lbStyle) : -1,
                    eok ? static_cast<unsigned>(elb.lbColor) : 0u,
                    IsDelphiGridFillDC(dc) ? 1 : 0, orig ? 1 : 0, CallerBaseName(callerMod).c_str());
            }
#endif
            const auto passthrough = [&]() -> int
            {
                return orig ? orig(dc, rect, brush) : ::FillRect(dc, rect, brush);
            };
            if (g_shutdown.load(std::memory_order_acquire) || !IsDelphiGridFillDC(dc))
                return passthrough();
            LOGBRUSH lb{};
            const bool haveBrush = brush
                && GetObjectW(brush, sizeof(lb), &lb) == sizeof(lb);
            // Стоковые кисти GetObject не отдают — определяем по хэндлу.
            COLORREF fillColor = haveBrush ? (lb.lbColor & 0xFFFFFF) : 0;
            bool solid = haveBrush && lb.lbStyle == BS_SOLID;
            if (!haveBrush && brush)
            {
                if (brush == GetSysColorBrush(COLOR_WINDOW))
                {
                    solid = true;
                    fillColor = GetSysColor(COLOR_WINDOW) & 0xFFFFFF;
                }
                else if (brush == GetSysColorBrush(COLOR_BTNFACE))
                {
                    solid = true;
                    fillColor = GetSysColor(COLOR_BTNFACE) & 0xFFFFFF;
                }
            }

            if (rect && !IsRectEmpty(rect) && solid
                && ApplyDelphiGridCellBackground(dc, rect, fillColor))
            {
                const HWND fillWnd = WindowFromDC(dc);
                RememberGridDataOrigin(fillWnd, rect->left, rect->top);
                if (auto* rec = FindGridPaint(dc, fillWnd))
                {
                    if (!rec->haveDataOrigin)
                    {
                        rec->dataOrigin = POINT{ rect->left, rect->top };
                        rec->haveDataOrigin = true;
                    }
                }
#if defined(_DEBUG)
                if (AllowDelphiGridLog(1))
                {
                    const COLORREF plain = fillColor & 0xFFFFFF;
                    const bool tinted = plain && plain != (GetSysColor(COLOR_WINDOW) & 0xFFFFFF);
                    if (tinted)
                        DebugLog(DBG_NORMAL,
                            L"[GlassCore][DelphiGrid] AlphaFill hwnd=0x%p dcWnd=0x%p rect=[%ld,%ld,%ld,%ld] color=0x%08X alpha=%u",
                            WindowFromDC(dc), WindowFromDC(dc), rect->left, rect->top, rect->right, rect->bottom,
                            static_cast<unsigned>(fillColor), static_cast<unsigned>(kDelphiGridCellAlpha));
                    else
                        DebugLog(DBG_NORMAL, L"[GlassCore][DelphiGrid] WhiteFill hwnd=0x%p dcWnd=0x%p rect=[%ld,%ld,%ld,%ld] color=0x%08X",
                            WindowFromDC(dc), WindowFromDC(dc), rect->left, rect->top, rect->right, rect->bottom,
                            static_cast<unsigned>(fillColor));
                }
#endif
                return 1;
            }
#if defined(_DEBUG)
            // Диагностика: чем именно закрашивается фон (например, заголовок
            // TStringGrid), чтобы не гадать по скриншоту.
            if (AllowDelphiGridLog(1))
                DebugLog(DBG_NORMAL,
                    L"[GlassCore][DelphiGrid] Fill hwnd=0x%p dcWnd=0x%p rect=[%ld,%ld,%ld,%ld] color=0x%08X style=%u brush=%d",
                    WindowFromDC(dc), WindowFromDC(dc), rect ? rect->left : 0, rect ? rect->top : 0,
                    rect ? rect->right : 0, rect ? rect->bottom : 0,
                    haveBrush ? static_cast<unsigned>(lb.lbColor) : 0u,
                    haveBrush ? static_cast<unsigned>(lb.lbStyle) : 0u, haveBrush ? 1 : 0);
#endif
            return passthrough();
        }

        HRESULT WINAPI Root_MyDelphiGridTheme(HTHEME theme, HDC dc, int part, int state,
            LPCRECT rect, LPCRECT clip)
        {
            auto orig = GetRootOrg<OHK_DELPHI_GRID_THEME, decltype(&DrawThemeBackground)>(GetCallerModule(_ReturnAddress()));
            if (!orig) return DrawThemeBackground(theme, dc, part, state, rect, clip);
            wchar_t cls[MAX_PATH + 1]{};
#if defined(_DEBUG)
            // Диагностика: какие элементы темы рисуются в DC таблицы. Фон
            // фиксированных ячеек не приходит ни через FillRect/PatBlt/BitBlt,
            // поэтому проверяем DrawThemeBackground (StyleServices.DrawElement).
            if (!g_shutdown.load(std::memory_order_acquire) && IsDelphiGridPaintDC(dc)
                && AllowDelphiGridLog(0))
            {
                wchar_t themeCls[MAX_PATH + 1]{};
                (void)Shared::ThemeHelper::GetThemeClass(theme, themeCls, MAX_PATH);
                DebugLog(DBG_NORMAL,
                    L"[GlassCore][DelphiGrid] Theme hwnd=0x%p part=%d state=%d rect=[%ld,%ld,%ld,%ld] theme=\"%s\"",
                    WindowFromDC(dc), part, state,
                    rect ? rect->left : 0, rect ? rect->top : 0,
                    rect ? rect->right : 0, rect ? rect->bottom : 0, themeCls);
            }
#endif
            // Header — фон фиксированной строки (TStringGrid/TValueListEditor),
            // Toolbar part=1 — фон ячейки значения у TValueListEditor
            // (StyleServices рисует его темой панели). Оба должны стать
            // прозрачными, иначе ячейка/заголовок остаются непрозрачными.
            const bool headerPart = (part == 0 || part == 1) && (state == 1 || state == 4);
            const bool toolbarPart = part == 1 && (state == 1 || state == 2);
            if (!g_shutdown.load(std::memory_order_acquire) && IsDelphiGridPaintDC(dc)
                && rect && !IsRectEmpty(rect) && (headerPart || toolbarPart)
                && SUCCEEDED(Shared::ThemeHelper::GetThemeClass(theme, cls, MAX_PATH))
                && (StrEqI(cls, L"Header") || StrEqI(cls, L"Toolbar")))
            {
                RECT area = *rect;
                if (clip && !IntersectRect(&area, &area, clip)) return S_OK;
                if (PatBlt(dc, area.left, area.top, area.right - area.left,
                    area.bottom - area.top, BLACKNESS))
                {
#if defined(_DEBUG)
                    if (AllowDelphiGridLog(2))
                        DebugLog(DBG_NORMAL, L"[GlassCore][DelphiGrid] Header hwnd=0x%p part=%d state=%d",
                            WindowFromDC(dc), part, state);
#endif
                    return S_OK;
                }
            }
            return orig(theme, dc, part, state, rect, clip);
        }

        // Фон клиента VCL-таблицы: TCustomGrid.Paint берёт цвет как
        // GetSysColor(COLOR_WINDOW) → CreateBrushIndirect → FillRect(ClientRect).
        // Этот FillRect приходит мимо нашего детура, поэтому гасим цвет в самой
        // точке его получения: белый (COLOR_WINDOW) → чёрный = прозрачность.
        DWORD WINAPI Root_MyGetSysColor(int nIndex)
        {
            const HMODULE callerMod = GetCallerModule(_ReturnAddress());
            auto o = GetRootOrg<OHK_GETSYSCOLOR, decltype(&GetSysColor)>(callerMod);
#if defined(_DEBUG)
            // Вход виден всегда: иначе не отличить «детур не позвали» от
            // «orig не найден» и «хост сессии не таблица».
            if (nIndex == COLOR_WINDOW && AllowDelphiGridLog(2))
            {
                const HWND host = GlassCore::PaintDispatcher::GetPaintSessionHostHwnd();
                wchar_t hostCls[128]{};
                if (host) GetClassNameW(host, hostCls, _countof(hostCls));
                DebugLog(DBG_NORMAL,
                    L"[GlassCore][DelphiGrid] GetSysColorEnter nIndex=%d host=0x%p hostCls=\"%s\" gridHost=%d orig=%d caller=%s",
                    nIndex, host, hostCls, IsDelphiGridPaintHost() ? 1 : 0, o ? 1 : 0,
                    CallerBaseName(callerMod).c_str());
            }
#endif
            if (!o)
                return GetSysColor(nIndex);
            if (!g_shutdown.load(std::memory_order_acquire) && nIndex == COLOR_WINDOW
                && IsDelphiGridPaintHost())
            {
#if defined(_DEBUG)
                if (AllowDelphiGridLog(2))
                    DebugLog(DBG_NORMAL,
                        L"[GlassCore][DelphiGrid] GetSysColor COLOR_WINDOW→0 (client fill)");
#endif
                return 0;
            }
            return o(nIndex);
        }

        int WINAPI Root_MyDrawTextW(
            HDC hdc, LPCWSTR t, int c, LPRECT rc, UINT fmt)
        {
            HMODULE caller = GetCallerModule(_ReturnAddress());
            auto o = GetRootOrg<OHK_DRAWTEXT, decltype(&DrawTextW)>(caller);
            // Паритет BWM OverallHooks::MyDrawTextW: вызов DrawTextW из кода msinfo32.exe
            // идёт через IAT-слот MFC42u (хвостовой вызов CDC::DrawText — адрес возврата
            // остаётся в EXE, а импорт DrawTextW в msinfo32.exe отсутствует), поэтому orig
            // берём из диспетчера MFC42u. Без этого o == null и вызов уходит в локальный
            // DrawTextW мимо стекла — кастомный заголовок не виден.
            if (!o && CallerBaseIs(caller, L"msinfo32.exe"))
            {
                if (HMODULE mfc = GetModuleHandleW(L"MFC42u.dll"))
                    o = GetRootOrg<OHK_DRAWTEXT, decltype(&DrawTextW)>(mfc);
            }
            if (!o)
                return DrawTextW(hdc, t, c, rc, fmt);
            if (g_shutdown.load(std::memory_order_acquire))
                return o(hdc, t, c, rc, fmt);
            // An outer root can still own this generic import. Do not run half
            // of another text path when the dedicated ANSI route is unarmed.
            if (caller == GlassCore::ModuleHandler::g_entryPointModule
                && GlassCore::RestoratorMemo::IsWindow(GlassCore::PaintDispatcher::GetPaintSessionHostHwnd()))
                return o(hdc, t, c, rc, fmt);
            if ((fmt & DT_CALCRECT) != 0)
                return o(hdc, t, c, rc, fmt);
            int r = 0;
            const HRESULT hr = GlassCore::Text::DrawTextWithAlpha(hdc, t, c, rc, fmt, r);
            if (!FAILED(hr))
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

        HRESULT WINAPI Root_MyDelphiDrawThemeTextEx(
            HTHEME theme, HDC dc, int part, int state, LPCWSTR text, int count,
            DWORD flags, LPRECT rect, const DTTOPTS* options)
        {
            // This slot is installed only on the entry-point EXE. An IAT-chain
            // trampoline may return into GlassCore, not into that EXE.
            const auto orig = GetRootOrg<OHK_DELPHI_DRAWTHEMETEXTEX, decltype(&::DrawThemeTextEx)>(
                GlassCore::ModuleHandler::g_entryPointModule);
            if (!orig)
                return ::DrawThemeTextEx(theme, dc, part, state, text, count, flags, rect, options);
            if (g_shutdown.load(std::memory_order_acquire))
                return orig(theme, dc, part, state, text, count, flags, rect, options);
            return GlassCore::DelphiPanel::DrawContainerThemeTextEx(
                orig, theme, dc, part, state, text, count, flags, rect, options);
        }

        // Restorator memo EXE thunks: the import owner, not a possible chain
        // trampoline's return address, selects the original. Helpers enforce TLS
        // readiness and exact window/DC scope even while another thread holds IAT.
        int WINAPI Root_MyRestoratorMemoText(HDC dc, LPCSTR text, int count, LPRECT rect, UINT flags)
        {
            const auto orig = GetRootOrg<OHK_RESTORATOR_MEMO_TEXT, decltype(&::DrawTextA)>(
                GlassCore::ModuleHandler::g_entryPointModule);
            if (!orig) return ::DrawTextA(dc, text, count, rect, flags);
            if (g_shutdown.load(std::memory_order_acquire)) return orig(dc, text, count, rect, flags);
            return GlassCore::RestoratorMemo::DrawAnsi(orig, dc, text, count, rect, flags);
        }
        HBRUSH WINAPI Root_MyRestoratorMemoBrush(const LOGBRUSH* brush)
        {
            const auto orig = GetRootOrg<OHK_RESTORATOR_MEMO_BRUSH, decltype(&::CreateBrushIndirect)>(
                GlassCore::ModuleHandler::g_entryPointModule);
            if (!orig) return ::CreateBrushIndirect(brush);
            if (g_shutdown.load(std::memory_order_acquire)) return orig(brush);
            return GlassCore::RestoratorMemo::CreateBrush(orig, brush);
        }
        DWORD WINAPI Root_MyRestoratorMemoColor(int index)
        {
            const auto orig = GetRootOrg<OHK_RESTORATOR_MEMO_COLOR, decltype(&::GetSysColor)>(
                GlassCore::ModuleHandler::g_entryPointModule);
            if (!orig) return ::GetSysColor(index);
            if (g_shutdown.load(std::memory_order_acquire)) return orig(index);
            return GlassCore::RestoratorMemo::SystemColor(orig, index);
        }
        int WINAPI Root_MyRestoratorMemoFill(HDC dc, const RECT* rect, HBRUSH brush)
        {
            const auto orig = GetRootOrg<OHK_RESTORATOR_MEMO_FILL, decltype(&::FillRect)>(
                GlassCore::ModuleHandler::g_entryPointModule);
            if (!orig) return ::FillRect(dc, rect, brush);
            if (g_shutdown.load(std::memory_order_acquire)) return orig(dc, rect, brush);
            return GlassCore::RestoratorMemo::Fill(orig, dc, rect, brush);
        }

        // Dedicated EXE slots: return addresses in IAT chains are not import owners.
        int WINAPI Root_MyDelphiGroupText(HDC dc, LPWSTR text, int count,
            LPRECT rect, UINT flags, LPDRAWTEXTPARAMS params)
        {
            const auto orig = GetRootOrg<OHK_DELPHI_GROUP_TEXT, decltype(&::DrawTextExW)>(
                GlassCore::ModuleHandler::g_entryPointModule);
            if (!orig) return ::DrawTextExW(dc, text, count, rect, flags, params);
            if (g_shutdown.load(std::memory_order_acquire)) return orig(dc, text, count, rect, flags, params);
            return GlassCore::DelphiButtonGroups::DrawCaptionText(orig, dc, text, count, rect, flags, params);
        }
        int WINAPI Root_MyDelphiGroupFill(HDC dc, const RECT* rect, HBRUSH brush)
        {
            const auto orig = GetRootOrg<OHK_DELPHI_GROUP_FILL, decltype(&::FillRect)>(
                GlassCore::ModuleHandler::g_entryPointModule);
            if (!orig) return ::FillRect(dc, rect, brush);
            if (g_shutdown.load(std::memory_order_acquire)) return orig(dc, rect, brush);
            return GlassCore::DelphiButtonGroups::Fill(orig, dc, rect, brush);
        }
        template <size_t slot>
        BOOL WINAPI Root_MyDelphiGroupGradient(HDC dc, PTRIVERTEX vertices, ULONG vertexCount,
            PVOID mesh, ULONG meshCount, ULONG mode)
        {
            const auto orig = GetRootOrg<slot, decltype(&::GdiGradientFill)>(
                GlassCore::ModuleHandler::g_entryPointModule);
            if (!orig) return ::GdiGradientFill(dc, vertices, vertexCount, mesh, meshCount, mode);
            if (g_shutdown.load(std::memory_order_acquire))
                return orig(dc, vertices, vertexCount, mesh, meshCount, mode);
            return GlassCore::DelphiButtonGroups::Gradient(orig, dc, vertices, vertexCount, mesh, meshCount, mode,
                slot == OHK_DELPHI_GROUP_GRADIENT ? L"GradientFill" : L"GdiGradientFill");
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
            // An outer root paint may still hold this generic EXE hook while a
            // group repaints. Preserve its operation RGB for the dedicated path.
            if (caller == GlassCore::ModuleHandler::g_entryPointModule
                && GlassShared::IsDelphiButtonGroupsClassName(cls)) return o(hdc, color);
            if (caller == GlassCore::ModuleHandler::g_entryPointModule
                && GlassCore::RestoratorMemo::IsWindow(hFromDc)) return o(hdc, color);

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

            // BWM OverallHooks::MySetBkColor parity, дженерик-ветка
            // (!mmc && caller != user32 && color == COLOR_WINDOW → 0):
            // фон окон и #32770-диалогов, рисуемых кодом самого EXE (dfrgui.exe),
            // заливается через SetBkColor(COLOR_WINDOW) из точки входа.
            // Слот включается динамически в Root_OnBeginPaint (caller == entry-point).
            // Гейты: только attach'нутый root (HasRootHookCache), только COLOR_WINDOW,
            // mmc.exe исключён как в BWM.
            if (!g_shutdown.load(std::memory_order_acquire) && caller
                && caller == GlassCore::ModuleHandler::g_entryPointModule
                && (color & 0xFFFFFF) == (GetSysColor(COLOR_WINDOW) & 0xFFFFFF)
                && hFromDc && GlassCore::Root::HasRootHookCache(hFromDc))
            {
                wchar_t epPath[MAX_PATH]{};
                bool isMmc = false;
                if (GetModuleFileNameW(
                        GlassCore::ModuleHandler::g_entryPointModule,
                        epPath, _countof(epPath)))
                {
                    isMmc = StrEqI(BasenamePtr(epPath).c_str(), L"mmc.exe");
                }
                if (!isMmc)
                {
                    DebugLog(DBG_NORMAL,
                        L"[GlassCore][Root] SetBkColor: entryPoint COLOR_WINDOW→0 hdc=0x%p hwndDC=0x%p caller=%s cls=\"%s\"",
                        hdc, hFromDc, callerBase, cls);
                    color = 0;
                }
            }
            return o(hdc, color);
        }

        HBRUSH WINAPI Root_MyGetSysColorBrush(int nIndex)
        {
            const auto caller = GetCallerModule(_ReturnAddress());
            auto o = GetRootOrg<OHK_GETSYSCOLORBR, decltype(&GetSysColorBrush)>(caller);
            if (!o)
                return GetSysColorBrush(nIndex);
            if (caller == GlassCore::ModuleHandler::g_entryPointModule
                && GlassShared::IsDelphiButtonGroupsWindow(GlassCore::PaintDispatcher::GetPaintSessionHostHwnd()))
                return o(nIndex);
            if (caller == GlassCore::ModuleHandler::g_entryPointModule
                && GlassCore::RestoratorMemo::IsWindow(GlassCore::PaintDispatcher::GetPaintSessionHostHwnd()))
                return o(nIndex);
            if (!g_shutdown.load(std::memory_order_acquire))
            {
                // PopupOnly: вне активной paint-сессии (стеклянного popup'а) comctl32/user32/
                // comdlg32 не трогаем — иначе неприаттаченные окна (файловые диалоги и т.п.)
                // получают чёрные кисти вместо системных. В Full — прежнее поведение.
                if (IsFullInjectionMode() || GlassCore::PaintDispatcher::GetPaintSessionHostHwnd() != nullptr)
                {
                    if (const HWND host = GlassCore::PaintDispatcher::GetPaintSessionHostHwnd())
                    {
                        wchar_t hostCls[64]{};
                        if (GetClassNameW(host, hostCls, _countof(hostCls))
                            && IsDelphiGridClass(hostCls))
                        {
                            // COLOR_WINDOW → прозрачный; цветные системные кисти
                            // (COLOR_BTNFACE и т.п.) отдаём как есть: альфа
                            // применяется в Root_MyDelphiGridFill.
                            if (nIndex == COLOR_WINDOW)
                                return CreateSolidBrush(0);
                        }
                    }
                    if (nIndex == COLOR_WINDOW || nIndex == COLOR_BTNFACE || nIndex == COLOR_MENU)
                    {
                        // Диагностика ограничена по числу срабатываний: видно, от каких модулей
                        // и для каких индексов чёрная кисть запрашивалась под сессией (например,
                        // comctl32 для WM_CTLCOLOREDIT-подобных путей неприаттаченных контролов).
                        static std::atomic<unsigned> s_sysColorBrushDiag{64};
                        unsigned n = s_sysColorBrushDiag.fetch_sub(1);
                        if (n < 64)
                        {
                            const std::wstring callerBase = CallerBaseName(GetCallerModule(_ReturnAddress()));
                            const HWND sessionHost = GlassCore::PaintDispatcher::GetPaintSessionHostHwnd();
                            wchar_t hostCls[128]{};
                            if (sessionHost)
                                GetClassNameW(sessionHost, hostCls, _countof(hostCls));
                            DebugLog(DBG_IMPORTANT,
                                L"[GlassCore][Root][GDI] GetSysColorBrush black: nIndex=%d caller=%s host=0x%p hostClass=\"%s\"",
                                nIndex, callerBase.c_str(), sessionHost, hostCls);
                        }
                        return CreateSolidBrush(0);
                    }
                }
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
            if (caller == GlassCore::ModuleHandler::g_entryPointModule
                && GlassShared::IsDelphiButtonGroupsWindow(GlassCore::PaintDispatcher::GetPaintSessionHostHwnd()))
                return o(color);
            if (caller == GlassCore::ModuleHandler::g_entryPointModule
                && GlassCore::RestoratorMemo::IsWindow(GlassCore::PaintDispatcher::GetPaintSessionHostHwnd()))
                return o(color);

            // PopupOnly: без активной paint-сессии (стеклянного popup'а) comctl32
            // не трогаем — иначе неприаттаченные окна (файловые диалоги и т.п.)
            // получают чёрные/alpha-кисти вместо системных. В Full — прежнее поведение.
            if (!IsFullInjectionMode() && GlassCore::PaintDispatcher::GetPaintSessionHostHwnd() == nullptr)
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

            // VCL-таблицы (TStringGrid/TDrawGrid/TShownValueList): COLOR_WINDOW →
            // прозрачный фон, прочие цвета → низкая альфа (стекло просвечивает).
            if (const HWND host = GlassCore::PaintDispatcher::GetPaintSessionHostHwnd())
            {
                wchar_t hostCls[64]{};
                if (GetClassNameW(host, hostCls, _countof(hostCls)) && IsDelphiGridClass(hostCls))
                {
                    // Фон таблиц: COLOR_WINDOW → прозрачный. Прочие цвета — обычная
                    // кисть; альфа делается на этапе заливки (Root_MyDelphiGridFill),
                    // потому что pattern-кисть через FillRect альфу не переносит.
                    const COLORREF plainGrid = color & 0xFFFFFF;
                    if (!plainGrid || plainGrid == (GetSysColor(COLOR_WINDOW) & 0xFFFFFF))
                        return o(RGB(0, 0, 0));
                    return o(color);
                }
            }

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
            {
                if (!IsFullInjectionMode())
                {
                    static std::atomic<unsigned> s_popupBrushLog{ 32 };
                    const unsigned n = s_popupBrushLog.fetch_sub(1, std::memory_order_relaxed);
                    if (n < 32)
                    {
                        const HWND host = GlassCore::PaintDispatcher::GetPaintSessionHostHwnd();
                        wchar_t hostCls[128]{};
                        if (host)
                            GetClassNameW(host, hostCls, _countof(hostCls));
                        DebugLog(DBG_IMPORTANT,
                            L"[GlassCore][Root] CreateSolidBrush black (PopupOnly) color=0x%06X caller=%s host=0x%p hostClass=\"%s\"",
                            plain, base.c_str(), host, hostCls);
                    }
                }
                return o(RGB(0, 0, 0));
            }

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

            // PopupOnly: вне активной paint-сессии замены кистей (USER32 → BLACK_BRUSH,
            // COMDLG32 → alpha) не делаем — неприаттаченные окна получают системные кисти.
            // В Full — прежнее поведение.
            if (!IsFullInjectionMode()
                && GlassCore::PaintDispatcher::GetPaintSessionHostHwnd() == nullptr)
                return o(hdc, h);

            // PopupOnly: оконный DC чужого контрола (поле комбо, TreeView, шапка) не черним,
            // даже если на потоке ещё жива стеклянная сессия попапа.
            if (!IsFullInjectionMode())
            {
                if (HWND foreign = HwndFromDc(hdc))
                {
                    using GlassShared::GlassControlKind;
                    const auto kind = GlassShared::GlassGetWindowKind(foreign);
                    const bool glassTarget =
                        kind == GlassControlKind::ComboLBox
                        || kind == GlassControlKind::Toolbar
                        || kind == GlassControlKind::MonthCal
                        || kind == GlassControlKind::TrackBar;
                    if (!glassTarget)
                        return o(hdc, h);
                }
            }

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
                    {
                        static std::atomic<unsigned> s_selectBlackDiag{64};
                        unsigned n = s_selectBlackDiag.fetch_sub(1);
                        if (n < 64)
                        {
                            HWND w2 = HwndFromDc(hdc);
                            wchar_t wCls[128]{};
                            if (w2)
                                GetClassNameW(w2, wCls, _countof(wCls));
                            DebugLog(DBG_IMPORTANT,
                                L"[GlassCore][Root][GDI] SelectObject USER32 black: dc=0x%p wnd=0x%p class=\"%s\" lb=0x%06X",
                                hdc, w2, wCls, lb.lbColor & 0xFFFFFF);
                        }
                        h = GetStockObject(BLACK_BRUSH);
                    }
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

#if defined(_DEBUG)
            if (IsDelphiGridFillDC(hdc) && AllowDelphiGridLog(1))
                DebugLog(DBG_NORMAL,
                    L"[GlassCore][DelphiGrid] PatBltAny hwnd=0x%p rop=0x%08X rect=[%ld,%ld,%ld,%ld] style=%u color=0x%08X",
                    WindowFromDC(hdc), static_cast<unsigned>(rop), x, y, x + cx, y + cy,
                    static_cast<unsigned>(lbrush.lbStyle), static_cast<unsigned>(lbrush.lbColor));
#endif
            // Фон VCL-таблицы стирается PatBlt-ом (в т.ч. WM_ERASEBKGND до
            // начала paint-сессии): PATCOPY/WHITENESS → прозрачность + тинт.
            if ((rop == PATCOPY || rop == WHITENESS) && IsDelphiGridFillDC(hdc))
            {
                const RECT gridRect{ x, y, x + cx, y + cy };
                const COLORREF brushColor = (lbrush.lbStyle == BS_SOLID && lbrush.lbColor)
                    ? lbrush.lbColor : GetBkColor(hdc);
#if defined(_DEBUG)
                if (AllowDelphiGridLog(1))
                    DebugLog(DBG_NORMAL,
                        L"[GlassCore][DelphiGrid] PatBlt hwnd=0x%p rop=0x%08X rect=[%ld,%ld,%ld,%ld] color=0x%08X",
                        WindowFromDC(hdc), static_cast<unsigned>(rop),
                        gridRect.left, gridRect.top, gridRect.right, gridRect.bottom,
                        static_cast<unsigned>(brushColor));
#endif
                if (ApplyDelphiGridCellBackground(hdc, &gridRect, brushColor))
                    return TRUE;
            }

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

#if defined(_DEBUG)
            if (lprc && hbr && (StrEqI(cls, L"TStringGrid") || StrEqI(cls, L"TDrawGrid")
                || StrEqI(cls, L"TShownValueList") || StrEqI(cls, L"TValueListEditor")
                || StrEqI(cls, L"TControlList")))
            {
                LOGBRUSH glb{};
                const bool gok = GetObject(hbr, sizeof(glb), &glb) != 0;
                DebugLog(DBG_NORMAL,
                    L"[GlassCore][DelphiGrid] GenFill hwnd=0x%p rect=[%ld,%ld,%ld,%ld] style=%d color=0x%08X",
                    hWnd, lprc->left, lprc->top, lprc->right, lprc->bottom,
                    gok ? static_cast<int>(glb.lbStyle) : -1,
                    gok ? static_cast<unsigned>(glb.lbColor) : 0u);
            }
#endif
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

#if defined(_DEBUG)
            // Диагностика таблиц: BitBlt в их DC (двойная буферизация) не виден
            // другим хукам — пишем, чтобы найти источник непрозрачного фона.
            if (IsDelphiGridFillDC(hdc) && AllowDelphiGridLog(2))
            {
                const HWND srcWnd = hdcSrc ? WindowFromDC(hdcSrc) : nullptr;
                const HGDIOBJ srcBmp = hdcSrc ? GetCurrentObject(hdcSrc, OBJ_BITMAP) : nullptr;
                BITMAP bm{};
                const bool bmOk = srcBmp && GetObjectW(srcBmp, sizeof(bm), &bm) == sizeof(bm);
                DebugLog(DBG_NORMAL,
                    L"[GlassCore][DelphiGrid] BitBlt hwnd=0x%p dst=(%d,%d) %dx%d srcWnd=0x%p srcBmp=0x%p %ldx%ld bpp=%u rop=0x%08X",
                    hWnd, x, y, cx, cy, srcWnd, srcBmp, bm.bmWidth, bm.bmHeight,
                    static_cast<unsigned>(bm.bmBitsPixel), static_cast<unsigned>(rop));
            }
#endif

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

    // Пара провайдеров для реконструкции фона фиксированной области таблиц.
    void RegisterDelphiGridPaintProvider() noexcept
    {
        GlassCore::PaintDispatcher::RegisterPaintProvider({ GridPaintBegin, GridPaintEnd });
    }

} // namespace GlassCore::Root
