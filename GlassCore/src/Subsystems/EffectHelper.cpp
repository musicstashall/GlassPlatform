// GlassCore/src/Subsystems/EffectHelper.cpp
//
// Реализация backdrop — в Shared::SwcaBackdrop::SetWindowBackdrop (паритет с BWM EffectHelper.hpp:
// dropShadow → маска рамки, порядок DWMWA 1029 / SYSTEMBACKDROP / DwmMakeWindowTransparent / SWCA / NC refresh).
// AccentFlags и тинт для acrylic/blur задаются там.

#include "CorePch.h"
#include "Subsystems/EffectHelper.h"
#include <Helpers/SwcaBackdrop.h>

#pragma comment(lib, "dwmapi.lib")

namespace GlassCore::EffectHelper
{
    namespace
    {
        // Маппинг выровнен с BWM enum EffectType:
        // 0=None, 1=Transparent, 2=Solid, 3=Blur, 4=AcrylicBlur,
        // 5=ModernAcrylicBlur, 6=Acrylic, 7=Mica, 8=MicaAlt
        [[nodiscard]] int MapEffectTypeToRegistry(EffectType effect) noexcept
        {
            switch (effect)
            {
            case EffectType::None:              return 0;
            case EffectType::Transparent:       return 1;
            case EffectType::Solid:             return 2;
            case EffectType::Blur:              return 3;
            case EffectType::AcrylicBlur:       return 4;
            case EffectType::ModernAcrylicBlur: return 5;
            case EffectType::Acrylic:           return 6;
            case EffectType::Mica:              return 7;
            case EffectType::MicaAlt:           return 8;
            default: return 0;
            }
        }
    } // namespace

    void EnableWindowDarkMode(HWND hwnd, BOOL dark)
    {
        Shared::SwcaBackdrop::SetImmersiveDarkMode(hwnd, dark);
    }

    void DwmMakeWindowTransparent(HWND hwnd, BOOL enable)
    {
        DWM_BLURBEHIND bb{};
        bb.dwFlags = DWM_BB_ENABLE | (enable ? (DWM_BB_BLURREGION | DWM_BB_TRANSITIONONMAXIMIZED) : 0);
        bb.fEnable = enable;
        bb.hRgnBlur = CreateRectRgn(0, 0, -1, -1);
        DwmEnableBlurBehindWindow(hwnd, &bb);
        DeleteObject(bb.hRgnBlur);
    }

    void TriggerWindowNCRendering(HWND hwnd)
    {
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
            SWP_NOOWNERZORDER | SWP_DRAWFRAME | SWP_NOACTIVATE);
    }

    // -------------------------------------------------------------------------
    // SetWindowBackdrop
    //
    // tintColor — формат ABGR (0xAABBGGRR), уже сконвертированный в
    // Settings::ApplyBackdropEffect из ARGB → ABGR.
    //
    // FIX: AccentFlags:
    //   0x02 — включает тинт для Blur и Acrylic.
    //   0x00 — нет тинта (для Solid, Transparent, None и при tintColor==0).
    //
    // Для dropShadow отдельного флага в SWCA нет — тень управляется
    // через DWMWA_NCRENDERING_POLICY / WS_EX_LAYERED.
    // -------------------------------------------------------------------------
    void SetWindowBackdrop(HWND hwnd, BOOL dropShadow, DWORD tintAbgr, EffectType effect, bool skipNcRendering, bool flyoutBackdrop)
    {
        Shared::SwcaBackdrop::SetWindowBackdrop(
            hwnd,
            dropShadow,
            tintAbgr,
            MapEffectTypeToRegistry(effect),
            skipNcRendering,
            flyoutBackdrop);
    }
}