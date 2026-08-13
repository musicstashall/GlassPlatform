#pragma once
#include "../../GlassCore/Include/Subsystems/Settings.h"

// Контекст текущего активного DropDown-окна (per-thread, per-DLL при inline в заголовке).
// Заполняется в GlassCore: OnDropDownShow, OnViewControlAttach, OnComboLBoxShow — для анимации
// DropDownPopupIn и сабклассов на том же GUI-потоке.

namespace WndCompat
{
    struct DropDownContext
    {
        bool useDarkMode{ false };
        GlassCore::Settings::WindowBackdropEffectContext backdropEffect{};
        GlassCore::Settings::BorderContext               border{};
        GlassCore::Settings::FlyoutAnimationContext      animation{};
    };

    inline thread_local DropDownContext g_dropDownCtx{};
}