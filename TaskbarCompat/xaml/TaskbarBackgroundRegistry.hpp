#pragma once

#include <Windows.h>
#include <cstdint>

#include <xamlOM.h>
#include <map>

#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>

#include "config/TaskbarRegistry.h"
#include "xaml/TaskbarZonePainter.hpp"

namespace TaskbarCompat
{
    // RedrawWindow Shell_*TrayWnd после XAML-патча (May-14); без него compositor может не обновить stroke.
    void RedrawShellTraysAfterTaskbarXamlPatch() noexcept;

    // Регистр панелей задач + патчинг BackgroundFill/BackgroundStroke / зоны.
    //
    // Один экземпляр на VisualTreeWatcher. Никакого дополнительного COM/proxy-stub
    // (в отличие от TranslucentTB::TaskbarAppearanceService) — TaskbarCompat не
    // экспонирует ITaskbarAppearanceService наружу процесса.
    class TaskbarBackgroundRegistry
    {
    public:
        explicit TaskbarBackgroundRegistry(Registry::Settings settings) noexcept;
        ~TaskbarBackgroundRegistry() = default;

        TaskbarBackgroundRegistry(const TaskbarBackgroundRegistry&) = delete;
        TaskbarBackgroundRegistry& operator=(const TaskbarBackgroundRegistry&) = delete;

        void RegisterBackgroundFill(
            InstanceHandle frameHandle,
            winrt::Windows::UI::Xaml::Shapes::Rectangle shape) noexcept;

        void RegisterBackgroundStroke(
            InstanceHandle frameHandle,
            winrt::Windows::UI::Xaml::Shapes::Rectangle shape) noexcept;

        void UnregisterFrame(InstanceHandle frameHandle) noexcept;

        void RestoreAll() noexcept;

        void ReapplyPatchedBrushes() noexcept;

        /// Theme/visual style: ClearValue → re-sample system brushes → Apply (не stale original*).
        void RecaptureAfterThemeChange(Registry::Settings settings) noexcept;

        void UpdateSettings(Registry::Settings settings) noexcept;

    private:
        struct TaskbarInfo
        {
            winrt::Windows::UI::Xaml::Shapes::Rectangle background{ nullptr };
            winrt::Windows::UI::Xaml::Shapes::Rectangle border{ nullptr };
            winrt::Windows::UI::Xaml::Media::Brush originalBackgroundFill{ nullptr };
            winrt::Windows::UI::Xaml::Media::Brush originalBorderFill{ nullptr };
            bool                                      borderLayoutCaptured{};
            winrt::Windows::UI::Xaml::Thickness     originalMargin{};
            double                                    originalHeight{};
            winrt::Windows::UI::Xaml::Media::Brush    originalStroke{ nullptr };
            double                                    originalStrokeThickness{};
            winrt::Windows::UI::Xaml::VerticalAlignment   originalVerticalAlignment{};
            winrt::Windows::UI::Xaml::HorizontalAlignment originalHorizontalAlignment{};
            std::uint32_t programmaticFillNesting{};

            bool                     backgroundFillWatcherRegistered{};
            int64_t                  backgroundFillWatcherToken{};
            bool                     borderFillWatcherRegistered{};
            int64_t                  borderFillWatcherToken{};

            Zones::FrameZoneState zones{};
            // Single-tile: CornerRadius + опционально perimeter Border на RootGrid.
            winrt::Windows::UI::Xaml::Controls::Grid rootGrid{ nullptr };
            winrt::Windows::UI::Xaml::Media::Brush originalRootBackground{ nullptr };
            winrt::Windows::UI::Xaml::CornerRadius originalRootCornerRadius{};
            winrt::Windows::UI::Xaml::Media::Brush originalRootBorderBrush{ nullptr };
            winrt::Windows::UI::Xaml::Thickness originalRootBorderThickness{};
            winrt::Windows::UI::Xaml::Thickness originalRootPadding{};
            bool rootCornersCaptured{};
            bool rootCornersPatched{};
            bool rootPaddingCaptured{};

            // Живая геометрия зон: LayoutUpdated → Apply; shrink-hold → debounce settle.
            // Revoker, а не сырой event_token: токен снимался через текущий
            // info.zones.rootGrid, и если Grid успевал смениться или обнулиться,
            // делегат оставался висеть на старом элементе. После FreeLibrary XAML
            // вызывал его в выгруженном образе — AV в CEventSourceBase::UntypedRaise
            // (дамп explorer 11.08). Revoker снимает подписку с того объекта,
            // на котором она была создана.
            winrt::Windows::UI::Xaml::FrameworkElement::LayoutUpdated_revoker zoneLayoutRevoker{};
            // Grid, на котором стоит подписка: панель может пересоздать RootGrid.
            winrt::Windows::UI::Xaml::Controls::Grid zoneLayoutHookedGrid{ nullptr };
            winrt::Windows::UI::Xaml::DispatcherTimer zoneShrinkFollowUpTimer{ nullptr };
            float lastCenterWidth{ -1.0f };
            bool zoneLayoutApplyNesting{};

            // Theme Recapture: после ClearValue Fill может ещё быть null/transparent —
            // отложенный re-sample, иначе Stop RestoreFill → дыра.
            winrt::Windows::UI::Xaml::DispatcherTimer themeSampleTimer{ nullptr };
            std::uint8_t themeSampleRetries{};
        };

        void DetachBackgroundFillWatcher(TaskbarInfo& info) noexcept;
        void DetachBorderFillWatcher(TaskbarInfo& info) noexcept;
        void DetachAllFillWatchers(TaskbarInfo& info) noexcept;

        void OnTrackedFillPropertyChanged(InstanceHandle frameHandle, bool patchBackgroundNotBorder) noexcept;

        void CaptureBorderOriginalLayout(TaskbarInfo& info) noexcept;
        void RestoreBorderOriginalLayout(TaskbarInfo& info) noexcept;

        void ApplyStrokePolicyToFrame(InstanceHandle frameHandle, TaskbarInfo& info) noexcept;
        void ApplyStrokePolicyToAllFrames() noexcept;
        void RegisterBorderFillWatcher(InstanceHandle frameHandle, TaskbarInfo& info) noexcept;

        void ApplyFillPolicyToFrame(InstanceHandle frameHandle, TaskbarInfo& info) noexcept;
        void ApplyFillPolicyToAllFrames() noexcept;
        void RestoreFrameVisuals(TaskbarInfo& info) noexcept;

        [[nodiscard]] winrt::Windows::UI::Xaml::Media::Brush MakeActiveBackdropBrush(
            TaskbarInfo& info) noexcept;

        void ApplySingleTileCorners(TaskbarInfo& info) noexcept;
        void RestoreSingleTileCorners(TaskbarInfo& info) noexcept;

        void EnsureZoneLayoutHook(InstanceHandle frameHandle, TaskbarInfo& info) noexcept;
        void DetachZoneLayoutHook(TaskbarInfo& info) noexcept;
        void OnZoneLayoutUpdated(InstanceHandle frameHandle) noexcept;
        void ApplyZonesForFrame(InstanceHandle frameHandle, TaskbarInfo& info) noexcept;
        void ArmZoneShrinkSettle(InstanceHandle frameHandle, TaskbarInfo& info) noexcept;
        void CancelZoneShrinkFollowUp(TaskbarInfo& info) noexcept;

        void CancelThemeSampleTimer(TaskbarInfo& info) noexcept;
        [[nodiscard]] bool TrySampleOriginalsAfterThemeClear(
            InstanceHandle frameHandle,
            TaskbarInfo& info) noexcept;
        void ArmThemeSampleRetry(InstanceHandle frameHandle, TaskbarInfo& info) noexcept;

        struct ProgrammaticFillScope final
        {
            TaskbarInfo* info{};
            explicit ProgrammaticFillScope(TaskbarInfo& i) noexcept
                : info(&i)
            {
                ++info->programmaticFillNesting;
            }
            ~ProgrammaticFillScope() noexcept
            {
                if (info && info->programmaticFillNesting > 0)
                    --info->programmaticFillNesting;
            }
            ProgrammaticFillScope(const ProgrammaticFillScope&) = delete;
            ProgrammaticFillScope& operator=(const ProgrammaticFillScope&) = delete;
        };

        static void RestoreFill(
            winrt::Windows::UI::Xaml::Shapes::Rectangle& shape,
            winrt::Windows::UI::Xaml::Media::Brush& originalFill) noexcept;

        Registry::Settings m_settings;
        std::map<InstanceHandle, TaskbarInfo> m_taskbars;
    };
}
