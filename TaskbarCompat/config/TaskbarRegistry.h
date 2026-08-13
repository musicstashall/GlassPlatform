#pragma once

#include <Windows.h>

// Параметры TaskbarCompat: XAML-фон / бордюр / зоны.
// Shared (Tray\): BorderDrawMode (и SWCA EffectType/Gradient/Border/CornerType).
// XAML bar (Tray\Xaml\): EffectType, SeparateZones, CornerType, Gradient/Border.
// Смежные: Tray\Xaml\{Popup|Preview|Menu|Tooltip} с fallback на Tray\Xaml.
// См. docs/guides/TaskbarBorders.md

namespace TaskbarCompat::Registry
{
    enum class SurfaceKind : DWORD
    {
        Bar     = 0, // Tray\Xaml — только панель
        Popup   = 1, // Overflow / InputSwitch (+ UICompat NC/Start/Search)
        Preview = 2, // HoverFlyout (углы всегда системные / CornerType=0)
        Menu    = 3, // Tray MenuFlyout
        Tooltip = 4, // Taskbar ToolTip
    };

    struct Settings
    {
        // BorderDrawMode == XAML — полный XAML paint path.
        bool paintPathXaml{ false };

        // BackgroundStroke / perimeter: патчить цветом (или 0 = скрыть).
        bool patchBorderStroke{ false };
        DWORD borderColorArgb{ 0x00000000 };

        // Tray\Xaml\EffectType (0–5). На DWM-пути не используется для fill (transparent).
        DWORD xamlEffectType{ 0 };

        // Тинт: Tray\ (SWCA) или Tray\Xaml\ (XAML path).
        DWORD tintArgb{ 0x9EDDDDDD };

        // Ключ *GradientColor присутствует — иначе системный бэкдроп/тинт.
        bool gradientColorPresent{ false };

        // Углы: DWM — Tray\CornerType (0–3); XAML — CornerType 0–5.
        bool cornersPresent{ false };
        DWORD cornerType{ 3 };

        // Tray\Xaml\SeparateZones (только Bar при paintPathXaml && cornersPresent).
        bool separateZones{ false };
    };

    [[nodiscard]] Settings ReadFromRegistry() noexcept;

    [[nodiscard]] Settings ReadSurface(SurfaceKind kind) noexcept;

    // Запомнить системный SolidColorBrush с острова (дефолты Tray\Xaml при отсутствии ключей).
    void NoteSystemFillArgb(DWORD argb) noexcept;
    void NoteSystemStrokeArgb(DWORD argb) noexcept;

    /// Сброс in-memory system fill/stroke перед ReloadSettings (смена темы / visual style).
    /// Theme/reload: mem + удаление CachedSystemStrokeArgb (не оставлять stale theme).
    void InvalidateSystemBrushSamples() noexcept;
}
