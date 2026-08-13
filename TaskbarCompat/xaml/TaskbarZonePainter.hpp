#pragma once

// Раздельные зоны (Windhawk DockLike): полный chrome TaskbarBackground гасится;
// left/center/tray — overlay на RootGrid (без мутации SystemTrayFrameGrid).

#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Media.Animation.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>
#include <winrt/Windows.Foundation.h>

#include <cstdint>
#include <memory>

#include "config/TaskbarRegistry.h"

namespace TaskbarCompat::Zones
{
    struct FrameZoneState;

    // Токен живости кадра для асинхронных завершений.
    // Composition ScopedBatch.Completed и Storyboard.Completed приходят позже, чем
    // запись реестра панели может быть удалена: захват FrameZoneState& по ссылке
    // давал use-after-free (в дампе explorer — freed-heap 0xdd… в ResetLeftAppearVisual).
    // Колбэк держит weak_ptr; деструктор состояния обнуляет цель.
    using ZoneLifetimeToken = std::shared_ptr<FrameZoneState*>;

    struct ZoneOverlay
    {
        // Хост: геометрия/анимация; element — тинт/бэкдроп сегмента.
        // Без Composition DropShadow / ThemeShadow plate: оба давали чёрную непрозрачную плитку.
        winrt::Windows::UI::Xaml::Controls::Grid host{ nullptr };
        winrt::Windows::UI::Xaml::Controls::Border element{ nullptr };
        // TranslateX + ScaleX на host.
        winrt::Windows::UI::Xaml::Media::CompositeTransform transform{ nullptr };
        bool inserted{};
        bool active{};
        winrt::Windows::Foundation::Rect visual{};
        winrt::Windows::Foundation::Rect target{};
        winrt::Windows::Foundation::Rect pending{};
        bool hasVisual{};
        bool hasPending{};
        bool queuedForAnim{};
        bool inFrameAnim{};
    };

    struct FrameZoneState
    {
        ZoneOverlay left;
        ZoneOverlay center;
        ZoneOverlay tray;

        // Один Storyboard на кадр — left/center/tray стартуют и заканчивают вместе.
        winrt::Windows::UI::Xaml::Media::Animation::Storyboard frameStoryboard{ nullptr };
        winrt::Windows::UI::Xaml::DispatcherTimer animArmTimer{ nullptr };
        bool frameAnimRunning{};

        // Появление left только после скрытия в этой сессии (не при старте платформы).
        bool leftAllowAppearAnim{};
        // Composition KeyFrame (не XAML Storyboard — тот растягивался до ~1000ms).
        bool leftAppearRunning{};

        winrt::Windows::UI::Xaml::Controls::Grid rootGrid{ nullptr };
        winrt::Windows::UI::Xaml::Media::Brush originalRootBackground{ nullptr };
        winrt::Windows::UI::Xaml::CornerRadius originalRootCornerRadius{};
        winrt::Windows::UI::Xaml::Media::Brush originalRootBorderBrush{ nullptr };
        winrt::Windows::UI::Xaml::Thickness originalRootBorderThickness{};
        bool rootChromeCaptured{};
        bool rootChromeCleared{};

        // Полный слой TaskbarBackground — Collapsed в режиме зон.
        winrt::Windows::UI::Xaml::UIElement backgroundControl{ nullptr };
        double originalBackgroundControlOpacity{ 1.0 };
        winrt::Windows::UI::Xaml::Visibility originalBackgroundControlVisibility{
            winrt::Windows::UI::Xaml::Visibility::Visible };
        bool backgroundControlOpacityPatched{};

        winrt::Windows::UI::Xaml::Controls::Grid fillParentGrid{ nullptr };
        winrt::Windows::UI::Xaml::Media::Brush originalFillParentBackground{ nullptr };
        bool fillParentCleared{};

        // Legacy: снять Background с SystemTrayFrameGrid, если патчили раньше.
        winrt::Windows::UI::Xaml::Controls::Grid legacyTrayHost{ nullptr };
        winrt::Windows::UI::Xaml::Media::Brush legacyTrayBackground{ nullptr };
        winrt::Windows::UI::Xaml::CornerRadius legacyTrayCorner{};
        winrt::Windows::UI::Xaml::Media::Brush legacyTrayBorderBrush{ nullptr };
        winrt::Windows::UI::Xaml::Thickness legacyTrayBorderThickness{};
        bool legacyTrayCaptured{};
        bool legacyTrayPatched{};

        bool fillCollapsed{};
        winrt::Windows::UI::Xaml::Visibility originalFillVisibility{
            winrt::Windows::UI::Xaml::Visibility::Visible };
        bool strokeCollapsed{};
        winrt::Windows::UI::Xaml::Visibility originalStrokeVisibility{
            winrt::Windows::UI::Xaml::Visibility::Visible };

        // Пропуск ApplyOverlay при неизменной геометрии (LayoutUpdated debounce).
        std::uint64_t lastBoundsSig{};

        // Close-window: не сужать center по mid-anim measure — hold до settle-commit.
        bool centerShrinkCommit{};
        bool centerShrinkHoldPending{};

        ZoneLifetimeToken lifetime{};

        // Поколения анимаций: Stop* инкрементирует, устаревшее Completed отбрасывается,
        // чтобы завершение отменённой анимации не трогало уже перезапущенную.
        std::uint32_t frameAnimGeneration{};
        std::uint32_t leftAppearGeneration{};

        FrameZoneState() = default;

        ~FrameZoneState() noexcept
        {
            if (lifetime && *lifetime == this)
                *lifetime = nullptr;
        }

        // Токен живости привязан к адресу состояния: копия/перемещение сделали бы
        // его невалидным. Состояние живёт только внутри записи реестра панелей.
        FrameZoneState(const FrameZoneState&) = delete;
        FrameZoneState& operator=(const FrameZoneState&) = delete;
    };

    void Apply(
        winrt::Windows::UI::Xaml::Shapes::Rectangle const& backgroundFill,
        winrt::Windows::UI::Xaml::Shapes::Rectangle const& backgroundStroke,
        FrameZoneState& state,
        Registry::Settings const& settings,
        winrt::Windows::UI::Xaml::Media::Brush const& brush) noexcept;

    void Restore(
        winrt::Windows::UI::Xaml::Shapes::Rectangle const& backgroundFill,
        winrt::Windows::UI::Xaml::Shapes::Rectangle const& backgroundStroke,
        FrameZoneState& state) noexcept;

    // panelHeightDip — ActualHeight панели/RootGrid; MaxRound (5) → height/2;
    // Increased (4) → системный CoreWindow NC (NcSystemCornerRadius).
    [[nodiscard]] double CornerRadiusDip(DWORD cornerType, double panelHeightDip) noexcept;
}
