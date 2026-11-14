#pragma once

#include <Windows.h>

// Параметры UICompat: XAML-фон/бордюр CoreWindow surfaces из Tray\Xaml\{Popup|Menu}.
// См. TaskbarCompat/docs/TaskbarBorders.md (секция UICompat).

namespace UICompat::Registry
{
    enum class SurfaceKind : DWORD
    {
        Popup   = 0, // NC / QS / Start / Search chrome
        Menu    = 1, // MenuFlyout / JumpList / Search WebView menus
        Tooltip = 2, // Shell tooltips (Start menu items)
    };

    struct Settings
    {
        bool  paintPathXaml{ false };
        DWORD xamlEffectType{ 0 };
        DWORD tintArgb{ 0x9EDDDDDD };
        bool  gradientColorPresent{ false };

        // XAML chrome Grid (Windhawk NC Styler): BorderBrush/Thickness/CornerRadius.
        bool  cornersPresent{ false };
        DWORD cornerType{ 0 };       // 0 Default … 5 MaxRound (4=Increased NC)
        bool  patchBorder{ false };  // перекрасить BorderBrush (системный stroke Grid)
        DWORD borderArgb{ 0 };
        bool  noBorder{ false };     // BorderThickness=0 (+ L1Chrome Collapsed)
    };

    // Popup (совместимость ctor / pending).
    [[nodiscard]] Settings ReadFromRegistry() noexcept;

    [[nodiscard]] Settings ReadSurface(SurfaceKind kind) noexcept;
}
