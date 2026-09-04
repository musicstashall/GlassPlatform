// Глобальные Detours для System.Windows.Forms.ni.dll, MFC42u.dll (AMCCustomTab)
// и mmc.exe (Disk Management / all MMC snap-ins).
// BWM: FormsHooks::ColorBrushAttach + ModuleHandler при LOAD forms.ni.
// AMCCustomTab: CreateSolidBrush + GetSysColorBrush подменяют системные цвета на чёрный.
// mmc.exe: SetBkColor, FillRect, ExtTextOutW — always-on Detours (BWM mmcHooks.cpp).

#include "CorePch.h"

#include <Hooks/forms/FormsHooks.h>
#include <Hooks/forms/FormsHooksInternal.h>
#include <Core/Intercept/HookHelper.h>
#include <Subsystems/RenderText.h>
#include <WinApiUtils.h>
#include <wil.h>

using namespace GlassCore::Hooks;

namespace GlassCore::FormsHooks
{
    void ForceDetachDrawTextEx() noexcept;
    void ForceDetachDrawThemeBackground() noexcept;
    void ForceDetachDrawText() noexcept;

    static wil::srwlock              g_lock;
    static std::atomic<bool>         g_shutdown{ false };
    static bool                      g_brushHooksAttached{ false };

    static decltype(&::CreateSolidBrush) g_trueCreateSolidBrush = ::CreateSolidBrush;
    static decltype(&::GetSysColorBrush) g_trueGetSysColorBrush = ::GetSysColorBrush;

    // ── Helpers ─────────────────────────────────────────────────────────────

    static bool IsWindowClass(HWND hWnd, const wchar_t* className) noexcept
    {
        if (!hWnd)
            return false;
        wchar_t cls[128]{};
        return GetClassNameW(hWnd, cls, _countof(cls)) > 0
            && _wcsicmp(cls, className) == 0;
    }

    static HMODULE GetCallerModuleFromRet() noexcept
    {
        return Internal::GetCallerModule(_ReturnAddress());
    }

    // Определяет, нужно ли обрабатывать CreateSolidBrush для данного caller-модуля.
    // BWM: System.Windows.Forms.ni.dll (WinForms) + MFC42u.dll (AMCCustomTab).
    static bool IsBrushHookTargetModule(HMODULE callerMod) noexcept
    {
        return Internal::IsSystemWindowsFormsNiModule(callerMod)
            || Internal::IsMFC42uModule(callerMod);
    }

    HBRUSH WINAPI FormsNi_MyCreateSolidBrush(COLORREF color)
    {
        if (g_shutdown.load(std::memory_order_acquire))
            return g_trueCreateSolidBrush(color);

        const HMODULE callerMod = Internal::GetCallerModule(_ReturnAddress());
        if (!IsBrushHookTargetModule(callerMod))
            return g_trueCreateSolidBrush(color);

        DebugLog(DBG_NORMAL,
            L"[GlassCore][FormsHooks] CreateSolidBrush: caller=0x%p color=0x%08X",
            callerMod, static_cast<unsigned>(color));

        const COLORREF rgb = color & 0x00FFFFFF;
        if (rgb == (GetSysColor(COLOR_WINDOW) & 0x00FFFFFF)
            || rgb == (GetSysColor(COLOR_BTNFACE) & 0x00FFFFFF)
            || rgb == (GetSysColor(COLOR_BTNSHADOW) & 0x00FFFFFF))
        {
            color = (color & 0xFF000000) | RGB(0, 0, 0);
            return g_trueCreateSolidBrush(color);
        }

        if (HBRUSH alphaBrush = Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(color, 255))
            return alphaBrush;

        return g_trueCreateSolidBrush(color);
    }

    HBRUSH WINAPI FormsNi_MyGetSysColorBrush(int nIndex)
    {
        if (g_shutdown.load(std::memory_order_acquire))
            return g_trueGetSysColorBrush(nIndex);

        const HMODULE callerMod = Internal::GetCallerModule(_ReturnAddress());
        if (IsBrushHookTargetModule(callerMod)
            && (nIndex == COLOR_WINDOW || nIndex == COLOR_BTNFACE))
        {
            DebugLog(DBG_NORMAL,
                L"[GlassCore][FormsHooks] GetSysColorBrush: caller=0x%p index=%d → BLACK_BRUSH",
                callerMod, nIndex);
            return static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        }

        return g_trueGetSysColorBrush(nIndex);
    }

    void ColorBrushAttach(bool enable) noexcept
    {
        auto lock = g_lock.lock_exclusive();

        if (enable && g_shutdown.load(std::memory_order_acquire))
            return;

        if (enable)
        {
            if (g_brushHooksAttached)
                return;

            const HRESULT hr = Detours::Write([&]() {
                Detours::Attach(
                    reinterpret_cast<void**>(&g_trueCreateSolidBrush),
                    reinterpret_cast<void*>(&FormsNi_MyCreateSolidBrush));
                Detours::Attach(
                    reinterpret_cast<void**>(&g_trueGetSysColorBrush),
                    reinterpret_cast<void*>(&FormsNi_MyGetSysColorBrush));
            });
            if (FAILED(hr))
            {
                DebugLog(DBG_ERROR,
                    L"[GlassCore][FormsHooks] ColorBrushAttach Attach hr=0x%08X",
                    static_cast<unsigned>(hr));
                return;
            }

            g_brushHooksAttached = true;
            DebugLog(DBG_IMPORTANT, L"[GlassCore][FormsHooks] ColorBrushAttach: attached");
            return;
        }

        if (!g_brushHooksAttached)
            return;

        const HRESULT hr = Detours::Write([&]() {
            Detours::Detach(
                reinterpret_cast<void**>(&g_trueCreateSolidBrush),
                reinterpret_cast<void*>(&FormsNi_MyCreateSolidBrush));
            Detours::Detach(
                reinterpret_cast<void**>(&g_trueGetSysColorBrush),
                reinterpret_cast<void*>(&FormsNi_MyGetSysColorBrush));
        });
        if (FAILED(hr))
        {
            DebugLog(DBG_ERROR,
                L"[GlassCore][FormsHooks] ColorBrushAttach Detach hr=0x%08X",
                static_cast<unsigned>(hr));
            return;
        }

        g_brushHooksAttached = false;
        DebugLog(DBG_IMPORTANT, L"[GlassCore][FormsHooks] ColorBrushAttach: detached");
    }

    // =========================================================================
    //  mmc.exe Detours — always-on (BWM mmcHooks.cpp)
    // =========================================================================

    namespace
    {
        // ── SetBkColor ──────────────────────────────────────────────────────
        // BWM mmcHooks::MySetBkColor (lines 244-265).
        // Принудительно ставит RGB(0,0,0) вместо COLOR_WINDOW/COLOR_BTNFACE
        // для MFC42u.dll, COMCTL32.dll, sysmon.ocx, wvc.dll в mmc.exe.

        static wil::srwlock                         g_mmcSetBkLock;
        static bool                                 g_mmcSetBkAttached{ false };
        static decltype(&::SetBkColor)              g_trueSetBkColor = ::SetBkColor;

        COLORREF WINAPI Mmc_MySetBkColor(HDC hdc, COLORREF color)
        {
            if (g_shutdown.load(std::memory_order_acquire))
                return g_trueSetBkColor(hdc, color);

            const HMODULE callerMod = GetCallerModuleFromRet();
            HWND hWnd = WindowFromDC(hdc);

            bool forceBlack = false;

            // MFC42u.dll — всё кроме ComboLBox
            if (Internal::IsMFC42uModule(callerMod) && !IsWindowClass(hWnd, L"ComboLBox"))
                forceBlack = true;

            // COMCTL32.dll — SysHeader32, AfxWnd42u
            if (callerMod == GetModuleHandleW(L"COMCTL32.dll")
                && (IsWindowClass(hWnd, L"SysHeader32") || IsWindowClass(hWnd, L"AfxWnd42u")))
                forceBlack = true;

            // sysmon.ocx — SysmonReport
            if (callerMod == GetModuleHandleW(L"sysmon.ocx") && IsWindowClass(hWnd, L"SysmonReport"))
                forceBlack = true;

            // wvc.dll — PerfStatus
            if (callerMod == GetModuleHandleW(L"wvc.dll") && IsWindowClass(hWnd, L"PerfStatus"))
                forceBlack = true;

            if (forceBlack)
            {
                const COLORREF sysWnd = GetSysColor(COLOR_WINDOW) & 0x00FFFFFF;
                const COLORREF sysBtn = GetSysColor(COLOR_BTNFACE) & 0x00FFFFFF;
                const COLORREF c = color & 0x00FFFFFF;
                if (c == sysWnd || (c == sysBtn && !IsWindowClass(hWnd, L"SysListView32")))
                {
                    DebugLog(DBG_NORMAL,
                        L"[GlassCore][FormsHooks][Mmc] SetBkColor → black: caller=0x%p color=0x%08X",
                        callerMod, static_cast<unsigned>(color));
                    color = RGB(0, 0, 0);
                }
            }

            return g_trueSetBkColor(hdc, color);
        }

        // ── FillRect ────────────────────────────────────────────────────────
        // BWM mmcHooks::MyFillRect (lines 331-386) + OverallHooks::MyFillRect.
        // dmdskmgr.dll/dmview.ocx: BS_SOLID → alpha brush, BS_HATCHED → recreate.
        // mmc.exe SysListView32: COLOR_WINDOW → BLACKNESS.

        static wil::srwlock                         g_mmcFillRectLock;
        static bool                                 g_mmcFillRectAttached{ false };
        static decltype(&::FillRect)                g_trueFillRect = ::FillRect;

        int WINAPI Mmc_MyFillRect(HDC hDC, const RECT* lprc, HBRUSH hbr)
        {
            if (g_shutdown.load(std::memory_order_acquire))
                return g_trueFillRect(hDC, lprc, hbr);

            const HMODULE callerMod = GetCallerModuleFromRet();
            HWND hWnd = WindowFromDC(hDC);

            // dmdskmgr.dll — ComboBox / ComboLBox
            if (callerMod == GetModuleHandleW(L"dmdskmgr.dll")
                && (IsWindowClass(hWnd, L"ComboBox") || IsWindowClass(hWnd, L"ComboLBox")))
            {
                LOGBRUSH lb{};
                if (GetObject(hbr, sizeof(lb), &lb))
                {
                    if (lb.lbStyle == BS_SOLID)
                    {
                        if (HBRUSH alphaBrush = Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(lb.lbColor, 255))
                        {
                            int result = g_trueFillRect(hDC, lprc, alphaBrush);
                            DeleteObject(alphaBrush);
                            return result;
                        }
                    }
                    else if (lb.lbStyle == BS_HATCHED)
                    {
                        // IAT OHK_CREATEHATCHBRUSH в ComboLBoxHooks обрабатывает это.
                        // Пересоздаём кисть для корректного отображения.
                        HBRUSH newBrush = CreateBrushIndirect(&lb);
                        if (newBrush)
                        {
                            int result = g_trueFillRect(hDC, lprc, newBrush);
                            DeleteObject(newBrush);
                            return result;
                        }
                    }
                }
            }

            // dmview.ocx — AfxWnd42u (дисковые квадратики)
            if (callerMod == GetModuleHandleW(L"dmview.ocx") && IsWindowClass(hWnd, L"AfxWnd42u"))
            {
                LOGBRUSH lb{};
                if (GetObject(hbr, sizeof(lb), &lb))
                {
                    if (lb.lbStyle == BS_SOLID)
                    {
                        if (HBRUSH alphaBrush = Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(lb.lbColor, 255))
                        {
                            int result = g_trueFillRect(hDC, lprc, alphaBrush);
                            DeleteObject(alphaBrush);
                            return result;
                        }
                    }
                }
            }

            // mmc.exe SysListView32 — COLOR_WINDOW → black
            if (callerMod == GetModuleHandleW(L"mmc.exe") && IsWindowClass(hWnd, L"SysListView32"))
            {
                LOGBRUSH lb{};
                if (GetObject(hbr, sizeof(lb), &lb))
                {
                    if (lb.lbStyle == BS_SOLID && lb.lbColor == GetSysColor(COLOR_WINDOW))
                    {
                        if (lprc)
                            PatBlt(hDC, lprc->left, lprc->top,
                                lprc->right - lprc->left, lprc->bottom - lprc->top, BLACKNESS);
                        return 1;
                    }
                }
            }

            return g_trueFillRect(hDC, lprc, hbr);
        }

        // ── ExtTextOutW ─────────────────────────────────────────────────────
        // BWM mmcHooks::MyExtTextOutW (lines 456-580).
        // Прозрачные заливки (ETO_OPAQUE) + DrawTextWithAlpha для mmc snap-in модулей.

        static wil::srwlock                         g_mmcExtTextOutLock;
        static bool                                 g_mmcExtTextOutAttached{ false };
        static decltype(&::ExtTextOutW)             g_trueExtTextOutW = ::ExtTextOutW;

        BOOL WINAPI Mmc_MyExtTextOutW(HDC hdc, int x, int y, UINT options,
            const RECT* lprect, LPCWSTR lpString, UINT c, const INT* lpDx)
        {
            if (g_shutdown.load(std::memory_order_acquire))
                return g_trueExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);

            const HMODULE callerMod = GetCallerModuleFromRet();
            HWND hWnd = WindowFromDC(hdc);

            // Guard: только конкретные mmc snap-in модули
            const bool isTarget = callerMod == GetModuleHandleW(L"wvc.dll")
                || callerMod == GetModuleHandleW(L"sysmon.ocx")
                || callerMod == GetModuleHandleW(L"comctl32.dll")
                || callerMod == GetModuleHandleW(L"dmdskmgr.dll")
                || callerMod == GetModuleHandleW(L"dmview.ocx");

            if (!isTarget)
                return g_trueExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);

            DWORD dwTextFlags = DT_SINGLELINE;
            bool handled = false;

            auto handler = [&]() -> bool
            {
                RECT rc{};
                if (lprect && !IsRectEmpty(lprect))
                    CopyRect(&rc, lprect);
                else
                    GetClipBox(hdc, &rc);

                if (options & ETO_OPAQUE)
                {
                    COLORREF bkColor = GetBkColor(hdc);
                    if (bkColor == GetSysColor(COLOR_HIGHLIGHT)
                        || bkColor == GetSysColor(COLOR_BTNSHADOW))
                    {
                        // Highlight/shadow → alpha brush fill
                        if (HBRUSH brush = Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(bkColor, 255))
                        {
                            FillRect(hdc, lprect, brush);
                            DeleteObject(brush);
                        }
                    }
                    else
                    {
                        // Фоновый цвет → чёрный
                        if (lprect)
                            PatBlt(hdc, lprect->left, lprect->top,
                                lprect->right - lprect->left, lprect->bottom - lprect->top, BLACKNESS);
                    }
                    options &= ~ETO_OPAQUE;
                }
                else if (IsWindowClass(hWnd, L"ComboLBox") && callerMod == GetModuleHandleW(L"dmdskmgr.dll"))
                {
                    // Disk Management combo list — black fill по clip-региону
                    PatBlt(hdc, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, BLACKNESS);
                }

                if ((options == ETO_IGNORELANGUAGE) || options == ETO_GLYPH_INDEX)
                    return false;

                if (!c || (c == 1 && lpString && lpString[0] == 0x20))
                    return false;

                if (options & ETO_RTLREADING)
                    dwTextFlags |= DT_RIGHT | DT_RTLREADING;
                else
                    dwTextFlags |= DT_LEFT;

                if (callerMod == GetModuleHandleW(L"sysmon.ocx"))
                {
                    int result = 0;
                    if (!FAILED(GlassCore::Text::DrawTextWithAlpha(
                        hdc, lpString, static_cast<int>(c),
                        const_cast<LPRECT>(lprect), dwTextFlags, result)))
                    {
                        return true;
                    }
                }
                else
                {
                    int result = 0;
                    if (!FAILED(GlassCore::Text::DrawTextWithAlpha(
                        hdc, lpString, static_cast<int>(c), &rc, dwTextFlags, result)))
                    {
                        return true;
                    }
                }

                return false;
            };

            if (!handler())
            {
                return g_trueExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);
            }

            return TRUE;
        }

        // ── Attach / Detach ─────────────────────────────────────────────────

        void MmcSetBkColorAttachImpl(bool attach) noexcept
        {
            auto lock = g_mmcSetBkLock.lock_exclusive();
            if (attach == g_mmcSetBkAttached)
                return;
            if (attach && g_shutdown.load(std::memory_order_acquire))
                return;

            const HRESULT hr = Detours::Write([&]() {
                if (attach)
                    Detours::Attach(
                        reinterpret_cast<void**>(&g_trueSetBkColor),
                        reinterpret_cast<void*>(&Mmc_MySetBkColor));
                else
                    Detours::Detach(
                        reinterpret_cast<void**>(&g_trueSetBkColor),
                        reinterpret_cast<void*>(&Mmc_MySetBkColor));
            });
            if (FAILED(hr))
            {
                DebugLog(DBG_ERROR,
                    L"[GlassCore][FormsHooks] MmcSetBkColor %s hr=0x%08X",
                    attach ? L"Attach" : L"Detach", static_cast<unsigned>(hr));
                return;
            }
            g_mmcSetBkAttached = attach;
            DebugLog(DBG_IMPORTANT, L"[GlassCore][FormsHooks] MmcSetBkColor: %s", attach ? L"attached" : L"detached");
        }

        void MmcFillRectAttachImpl(bool attach) noexcept
        {
            auto lock = g_mmcFillRectLock.lock_exclusive();
            if (attach == g_mmcFillRectAttached)
                return;
            if (attach && g_shutdown.load(std::memory_order_acquire))
                return;

            const HRESULT hr = Detours::Write([&]() {
                if (attach)
                    Detours::Attach(
                        reinterpret_cast<void**>(&g_trueFillRect),
                        reinterpret_cast<void*>(&Mmc_MyFillRect));
                else
                    Detours::Detach(
                        reinterpret_cast<void**>(&g_trueFillRect),
                        reinterpret_cast<void*>(&Mmc_MyFillRect));
            });
            if (FAILED(hr))
            {
                DebugLog(DBG_ERROR,
                    L"[GlassCore][FormsHooks] MmcFillRect %s hr=0x%08X",
                    attach ? L"Attach" : L"Detach", static_cast<unsigned>(hr));
                return;
            }
            g_mmcFillRectAttached = attach;
            DebugLog(DBG_IMPORTANT, L"[GlassCore][FormsHooks] MmcFillRect: %s", attach ? L"attached" : L"detached");
        }

        void MmcExtTextOutAttachImpl(bool attach) noexcept
        {
            auto lock = g_mmcExtTextOutLock.lock_exclusive();
            if (attach == g_mmcExtTextOutAttached)
                return;
            if (attach && g_shutdown.load(std::memory_order_acquire))
                return;

            const HRESULT hr = Detours::Write([&]() {
                if (attach)
                    Detours::Attach(
                        reinterpret_cast<void**>(&g_trueExtTextOutW),
                        reinterpret_cast<void*>(&Mmc_MyExtTextOutW));
                else
                    Detours::Detach(
                        reinterpret_cast<void**>(&g_trueExtTextOutW),
                        reinterpret_cast<void*>(&Mmc_MyExtTextOutW));
            });
            if (FAILED(hr))
            {
                DebugLog(DBG_ERROR,
                    L"[GlassCore][FormsHooks] MmcExtTextOut %s hr=0x%08X",
                    attach ? L"Attach" : L"Detach", static_cast<unsigned>(hr));
                return;
            }
            g_mmcExtTextOutAttached = attach;
            DebugLog(DBG_IMPORTANT, L"[GlassCore][FormsHooks] MmcExtTextOut: %s", attach ? L"attached" : L"detached");
        }
    }

    // ── Public API ───────────────────────────────────────────────────────────

    void MmcSetBkColorAttach(bool enable) noexcept
    {
        MmcSetBkColorAttachImpl(enable);
    }

    void MmcFillRectAttach(bool enable) noexcept
    {
        MmcFillRectAttachImpl(enable);
    }

    void MmcExtTextOutAttach(bool enable) noexcept
    {
        MmcExtTextOutAttachImpl(enable);
    }

    void ForceDetachAllMmcHooks() noexcept
    {
        MmcSetBkColorAttachImpl(false);
        MmcFillRectAttachImpl(false);
        MmcExtTextOutAttachImpl(false);
    }

    void Startup() noexcept
    {
        g_shutdown.store(false, std::memory_order_release);

        // Регистрируем PaintDispatcher провайдер для AMCCustomTab (до PaintDispatcher::Startup).
        RegisterCustomTabPaintProvider();

        // Подключаем ColorBrush хуки, если есть целевые модули.
        if (GetModuleHandleW(L"System.Windows.Forms.ni.dll") || GetModuleHandleW(L"MFC42u.dll"))
            ColorBrushAttach(true);

        // mmc.exe: always-on Detours (BWM mmcHooks — entire process lifetime).
        if (Internal::IsMmcProcess())
        {
            MmcSetBkColorAttach(true);
            MmcFillRectAttach(true);
            MmcExtTextOutAttach(true);
        }
    }

    void Shutdown() noexcept
    {
        g_shutdown.store(true, std::memory_order_release);
        ForceDetachAllMmcHooks();
        ForceDetachDrawText();
        ForceDetachDrawTextEx();
        ForceDetachDrawThemeBackground();
        ColorBrushAttach(false);
    }
}
