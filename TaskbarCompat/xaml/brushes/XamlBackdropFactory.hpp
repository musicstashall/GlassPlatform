#pragma once

// Фабрика XAML-кистей фона панели задач (Transparent / Solid / Blur / Acrylic / System).

#include <windows.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Media.h>

namespace TaskbarCompat
{
    // effectType: 0 Disable, 1 Transparent, 2 Solid, 3 Blur, 4 Acrylic, 5 System
    // nullptr — «вернуть оригинал» (Disable/System) или ошибка создания.
    [[nodiscard]] winrt::Windows::UI::Xaml::Media::Brush CreateBackdropBrush(
        winrt::Windows::UI::Xaml::UIElement const& elementForCompositor,
        DWORD effectType,
        DWORD tintArgb) noexcept;

    // Нейтральный Acrylic Backdrop (не Tint из Glass registry).
    // Stop после theme: Acrylic TintColor={ThemeResource} (XamlReader), иначе
    // hardcoded dark/light. SetValue(App.Resources) не следует теме (O7).
    [[nodiscard]] winrt::Windows::UI::Xaml::Media::Brush
        CreateNeutralAcrylicBackdropBrush() noexcept;
}
