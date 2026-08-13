#include "CorePch.h"
#include <Subsystems/FlyoutFrame.h>
#include <Subsystems/Settings.h>
#include <Subsystems/EffectHelper.h>
#include <MenuUtils.h>
#include <dwmapi.h>

#include <mutex>
#include <unordered_set>

namespace GlassCore::FlyoutFrame
{
    namespace
    {
        std::mutex g_hostBorderDeferredMutex;
        std::unordered_set<HWND> g_hostBorderDeferredHwnds;
        [[nodiscard]] WndCompat::Menu::BorderContext ToMenuBorder(
            const Settings::BorderContext& border) noexcept
        {
            WndCompat::Menu::BorderContext out{};
            out.colorUseNone    = border.colorUseNone;
            out.colorUseDefault = border.colorUseDefault;
            out.color           = border.color;
            out.cornerType      = border.cornerType;
            return out;
        }

        void RefreshDwmNc(HWND hwnd, unsigned effectType, DWM_WINDOW_CORNER_PREFERENCE cornerType) noexcept
        {
            if (effectType >= 6 && effectType <= 8)
            {
                (void)DefWindowProcW(hwnd, WM_NCACTIVATE, TRUE, 0);
                EffectHelper::TriggerWindowNCRendering(hwnd);
            }
            else if (cornerType != DWMWCP_DEFAULT)
            {
                EffectHelper::TriggerWindowNCRendering(hwnd);
            }
        }

        void PaintNcRingGdi(HWND hwnd, bool darkMode, const Settings::BorderContext& border) noexcept
        {
            HDC hdc = GetWindowDC(hwnd);
            if (!hdc)
                return;

            auto releaseDC = wil::scope_exit([&] { ReleaseDC(hwnd, hdc); });

            constexpr MARGINS mr{ 1, 1, 1, 1 };
            RECT paintRect{};
            if (!GetWindowRect(hwnd, &paintRect))
                return;
            OffsetRect(&paintRect, -paintRect.left, -paintRect.top);

            {
                const int savedDC = SaveDC(hdc);
                ExcludeClipRect(hdc,
                    paintRect.left + mr.cxLeftWidth,
                    paintRect.top + mr.cyTopHeight,
                    paintRect.right - mr.cxRightWidth,
                    paintRect.bottom - mr.cyBottomHeight);
                PatBlt(hdc, paintRect.left, paintRect.top,
                    paintRect.right - paintRect.left,
                    paintRect.bottom - paintRect.top,
                    BLACKNESS);
                RestoreDC(hdc, savedDC);
            }

            if (border.cornerType == DWMWCP_DONOTROUND)
                return;

            (void)Shared::MenuUtils::PaintFlyoutNcOutlineGdi(
                hdc, paintRect, ToMenuBorder(border), darkMode);
        }

        void ApplyHostBorderFlush(HWND hwnd, FlyoutHostBorderFlush flush) noexcept
        {
            switch (flush)
            {
            case FlyoutHostBorderFlush::None:
                (void)DwmFlush();
                break;
            case FlyoutHostBorderFlush::FrameOnly:
                (void)RedrawWindow(
                    hwnd,
                    nullptr,
                    nullptr,
                    RDW_FRAME | RDW_NOCHILDREN);
                (void)DwmFlush();
                break;
            case FlyoutHostBorderFlush::Full:
                (void)RedrawWindow(
                    hwnd,
                    nullptr,
                    nullptr,
                    RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOCHILDREN);
                break;
            }
        }
    } // namespace

    void SetHostBorderDeferred(HWND hWnd, bool deferred) noexcept
    {
        if (!hWnd)
            return;
        std::lock_guard lock(g_hostBorderDeferredMutex);
        if (deferred)
        {
            g_hostBorderDeferredHwnds.insert(hWnd);
            DebugLog(DBG_IMPORTANT,
                L"[GlassCore][FlyoutFrame] HostBorderDeferred hwnd=0x%p",
                hWnd);
        }
        else
        {
            g_hostBorderDeferredHwnds.erase(hWnd);
        }
    }

    bool IsHostBorderDeferred(HWND hWnd) noexcept
    {
        if (!hWnd)
            return false;
        std::lock_guard lock(g_hostBorderDeferredMutex);
        return g_hostBorderDeferredHwnds.find(hWnd) != g_hostBorderDeferredHwnds.end();
    }

    void ClearHostBorderDeferred(HWND hWnd) noexcept
    {
        if (!hWnd)
            return;
        std::lock_guard lock(g_hostBorderDeferredMutex);
        g_hostBorderDeferredHwnds.erase(hWnd);
    }

    void SyncPrimeFlyoutNcBlack(HWND hWnd) noexcept
    {
        if (!hWnd || !IsWindow(hWnd))
            return;

        (void)SendMessageW(hWnd, WM_NCPAINT, reinterpret_cast<WPARAM>(HRGN(1)), 0);
        (void)DwmFlush();

        DebugLog(DBG_IMPORTANT,
            L"[GlassCore][FlyoutFrame] PrimeFlyoutNcBlackBeforeCloak hwnd=0x%p",
            hWnd);
    }

    void PaintDeferredHostBorder(
        HWND hWnd,
        bool darkMode,
        const Settings::BorderContext& border,
        unsigned effectType,
        FlyoutHostBorderFlush flush) noexcept
    {
        if (!hWnd || !IsWindow(hWnd) || !IsHostBorderDeferred(hWnd))
            return;

        ClearHostBorderDeferred(hWnd);
        PaintHostBorder(hWnd, darkMode, border, effectType, flush);

        DebugLog(DBG_IMPORTANT,
            L"[GlassCore][FlyoutFrame] PaintDeferredHostBorder hwnd=0x%p flush=%u",
            hWnd,
            static_cast<unsigned>(flush));
    }

    void PaintHostBorder(
        HWND hwnd,
        bool darkMode,
        const Settings::BorderContext& border,
        unsigned effectType,
        FlyoutHostBorderFlush flush) noexcept
    {
        if (!hwnd || !IsWindow(hwnd))
            return;

        if (IsHostBorderDeferred(hwnd))
            return;

        Settings::ApplyBorderEffect(hwnd, darkMode, border);
        RefreshDwmNc(hwnd, effectType, border.cornerType);
        PaintNcRingGdi(hwnd, darkMode, border);
        ApplyHostBorderFlush(hwnd, flush);

        DebugLog(DBG_IMPORTANT,
            L"[GlassCore][FlyoutFrame] PaintHostBorder hwnd=0x%p corner=%d effectType=%u flush=%u",
            hwnd,
            static_cast<int>(border.cornerType),
            effectType,
            static_cast<unsigned>(flush));
    }
}
