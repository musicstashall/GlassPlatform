#pragma once
#include <Subsystems/Settings.h>
#include <windows.h>

namespace GlassCore::Tooltip::Detail
{
    // Глобальное состояние тултипа/KbxLabel (аналог BWM g_tooltipContext), читают TooltipHooks.
    struct RuntimeContext
    {
        HWND hwnd{};
        bool useDarkMode{};
        GlassCore::Settings::TooltipRenderingContext rendering{};
        bool noSystemDropShadow{};
        bool noMarginsHandling{};
        GlassCore::Settings::WindowBackdropEffectContext backdrop{};
        GlassCore::Settings::BorderContext border{};
    };

    [[nodiscard]] RuntimeContext& Context() noexcept;
    [[nodiscard]] bool IsShuttingDown() noexcept;
}
