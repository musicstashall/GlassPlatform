#pragma once

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>
#include <xamlOM.h>

#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>

#include "config/TaskbarRegistry.h"

namespace TaskbarCompat
{
    // XAML-патч HoverFlyout — Windhawk Taskbar Styler (только XAML, без DWM на host).
    // Backdrop: Rectangle#BackgroundFill; chrome: Border#HoverFlyoutBackground
    // (BorderBrush / CornerRadius; Stroke Collapsed); Control — гасим Border*, CR при wantCorners.
    class TrayHoverFlyoutPanelRegistry
    {
    public:
        explicit TrayHoverFlyoutPanelRegistry(Registry::Settings settings) noexcept;
        ~TrayHoverFlyoutPanelRegistry() = default;

        TrayHoverFlyoutPanelRegistry(const TrayHoverFlyoutPanelRegistry&) = delete;
        TrayHoverFlyoutPanelRegistry& operator=(const TrayHoverFlyoutPanelRegistry&) = delete;

        // Якорь: Taskbar.TaskbarBackground#HoverFlyoutBackgroundControl или Grid#HoverFlyoutGrid.
        void RegisterPanel(
            InstanceHandle handle,
            winrt::Windows::UI::Xaml::FrameworkElement root) noexcept;

        // Border#HoverFlyoutBackground.
        void RegisterBackgroundBorder(
            InstanceHandle handle,
            winrt::Windows::UI::Xaml::Controls::Border border) noexcept;

        // Grid#HoverFlyoutGrid — CornerRadius (Border.CornerRadius → E_NOINTERFACE).
        void RegisterChromeGrid(
            InstanceHandle handle,
            winrt::Windows::UI::Xaml::Controls::Grid grid) noexcept;

        // Rectangle#BackgroundFill под HoverFlyoutBackgroundControl.
        void RegisterBackgroundFill(
            InstanceHandle handle,
            winrt::Windows::UI::Xaml::Shapes::Rectangle fill) noexcept;

        // Rectangle#BackgroundStroke под HoverFlyoutBackgroundControl (опционально).
        void RegisterBackgroundStroke(
            InstanceHandle handle,
            winrt::Windows::UI::Xaml::Shapes::Rectangle stroke) noexcept;

        void RetryUnresolvedPanels() noexcept;
        void Unregister(InstanceHandle handle) noexcept;
        void RestoreAll() noexcept;
        void ReapplyPatchedBrushes() noexcept;
        void RecaptureAfterThemeChange(Registry::Settings settings) noexcept;
        void UpdateSettings(Registry::Settings settings) noexcept;
        [[nodiscard]] unsigned PanelCount() const noexcept;
        [[nodiscard]] unsigned UnresolvedCount() const noexcept;

    private:
        struct RegistryState
        {
            std::atomic<bool> shutdown{ false };
            std::atomic<int> pendingAsync{ 0 };
        };

        struct PanelSlot
        {
            winrt::Windows::UI::Xaml::FrameworkElement root{ nullptr };
            winrt::Windows::UI::Xaml::Controls::Border backgroundBorder{ nullptr };
            winrt::Windows::UI::Xaml::Controls::Grid hoverFlyoutGrid{ nullptr };
            // Внутренний Grid под HoverFlyoutBackgroundControl — системный clip углов.
            winrt::Windows::UI::Xaml::Controls::Grid systemInnerGrid{ nullptr };
            winrt::Windows::UI::Xaml::Shapes::Rectangle backgroundFill{ nullptr };
            winrt::Windows::UI::Xaml::Shapes::Rectangle backgroundStroke{ nullptr };
            // Shadow plate: отдельный Rectangle в systemInnerGrid (не BackgroundStroke —
            // тот top-edge; post-fix16 → чёрная черта сверху).
            winrt::Windows::UI::Xaml::Shapes::Rectangle shadowPlate{ nullptr };
            // Windhawk: ContentPresenter#HoverFlyoutContent — CR; Border* гасим (chrome на Border).
            winrt::Windows::UI::Xaml::Controls::ContentPresenter hoverContent{ nullptr };

            InstanceHandle rootHandle{ 0 };
            InstanceHandle borderHandle{ 0 };
            InstanceHandle gridHandle{ 0 };
            InstanceHandle fillHandle{ 0 };
            InstanceHandle strokeHandle{ 0 };
            winrt::Windows::UI::Xaml::Media::Brush originalFill{ nullptr };
            winrt::Windows::UI::Xaml::Media::Brush originalBorderBackground{ nullptr };
            winrt::Windows::UI::Xaml::Media::Brush originalBorderBrush{ nullptr };
            winrt::Windows::UI::Xaml::Thickness originalBorderThickness{};
            winrt::Windows::UI::Xaml::CornerRadius originalCornerRadius{};
            winrt::Windows::UI::Xaml::CornerRadius originalGridCornerRadius{};
            winrt::Windows::UI::Xaml::CornerRadius originalRootCornerRadius{};
            winrt::Windows::UI::Xaml::CornerRadius originalSystemInnerCornerRadius{};
            winrt::Windows::UI::Xaml::CornerRadius originalContentCornerRadius{};
            winrt::Windows::UI::Xaml::Thickness originalRootBorderThickness{};
            winrt::Windows::UI::Xaml::Thickness originalContentBorderThickness{};
            winrt::Windows::UI::Xaml::Media::Brush originalRootBackground{ nullptr };
            winrt::Windows::UI::Xaml::Media::Brush originalRootBorderBrush{ nullptr };
            winrt::Windows::UI::Xaml::Media::Brush originalHoverGridBackground{ nullptr };
            winrt::Windows::UI::Xaml::Media::Brush originalContentBorderBrush{ nullptr };
            winrt::Windows::UI::Xaml::Media::Shadow originalShadow{ nullptr };
            bool gridCornerCaptured{ false };
            bool rootCornerCaptured{ false };
            bool rootCornerPatched{ false };
            bool rootChromeCleared{ false };
            bool hoverGridBgCleared{ false };
            bool systemInnerCornerCaptured{ false };
            bool systemInnerCornerPatched{ false };
            bool contentCornerCaptured{ false };
            bool contentChromePatched{ false };
            bool fillStrokePatched{ false };
            bool fillShadowPatched{ false };
            bool borderShadowPatched{ false };
            bool strokeShadowPatched{ false };
            bool shadowPlateInserted{ false };
            winrt::Windows::UI::Xaml::Visibility originalStrokeVisibility{
                winrt::Windows::UI::Xaml::Visibility::Visible };
            bool strokeVisibilityCaptured{ false };
            winrt::Windows::UI::Xaml::Visibility originalFillVisibility{
                winrt::Windows::UI::Xaml::Visibility::Visible };
            bool fillVisibilityCaptured{ false };
            bool fillCollapsed{ false };

            bool chromeCaptured{ false };
            bool chromePatched{ false };
            bool fillPatched{ false };
            bool fillRadiiPatched{ false };
            double originalFillRadiusX{ 0.0 };
            double originalFillRadiusY{ 0.0 };
            bool strokePatched{ false };
            bool hoverGridCornerPatched{ false };
            bool visualsResolved{ false };
            bool appliedOk{ false };
            bool needsRetry{ false };
            bool resolveFailLogged{ false };
            bool sizeHooked{ false };
            bool sizeApplyBusy{ false };
            bool sizeApplyDone{ false };
            ULONGLONG firstSeenTick{ 0 };

            winrt::Windows::UI::Xaml::FrameworkElement::SizeChanged_revoker sizeRevoker{};

            void ClearWinrtRefs() noexcept
            {
                sizeRevoker = {};
                sizeHooked = false;
                root = nullptr;
                backgroundBorder = nullptr;
                hoverFlyoutGrid = nullptr;
                systemInnerGrid = nullptr;
                backgroundFill = nullptr;
                backgroundStroke = nullptr;
                shadowPlate = nullptr;
                hoverContent = nullptr;
                originalFill = nullptr;
                originalBorderBackground = nullptr;
                originalBorderBrush = nullptr;
                originalRootBackground = nullptr;
                originalRootBorderBrush = nullptr;
                originalHoverGridBackground = nullptr;
                originalContentBorderBrush = nullptr;
                originalShadow = nullptr;
                gridCornerCaptured = false;
                rootCornerCaptured = false;
                rootCornerPatched = false;
                rootChromeCleared = false;
                hoverGridBgCleared = false;
                systemInnerCornerCaptured = false;
                systemInnerCornerPatched = false;
                contentCornerCaptured = false;
                contentChromePatched = false;
                fillStrokePatched = false;
                fillShadowPatched = false;
                borderShadowPatched = false;
                strokeShadowPatched = false;
                shadowPlateInserted = false;
            }
        };

        // Windhawk: Fill/Stroke под HoverFlyoutBackgroundControl.
        // Shallow GetChild по Name — без VTC Rectangle Add и без глубокого walk.
        [[nodiscard]] bool TryAttachNamedRect(
            PanelSlot& slot,
            std::wstring_view name,
            winrt::Windows::UI::Xaml::Shapes::Rectangle& outRect,
            winrt::Windows::UI::Xaml::Media::Brush* outOriginalFill) noexcept;
        [[nodiscard]] bool TryAttachFill(PanelSlot& slot) noexcept;
        [[nodiscard]] bool TryAttachStroke(PanelSlot& slot) noexcept;
        void TryAttachChromeGrid(PanelSlot& slot) noexcept;
        void TryAttachSystemInnerGrid(PanelSlot& slot) noexcept;
        void TryAttachHoverContent(PanelSlot& slot) noexcept;
        [[nodiscard]] bool ResolveVisuals(PanelSlot& slot) noexcept;
        void CaptureChrome(PanelSlot& slot) noexcept;
        // Не SizeChanged на Border (QI crash). Углы/tint — Dispatcher.RunAsync + RetryUnresolved.
        void ScheduleDeferredApply(std::shared_ptr<PanelSlot> slot) noexcept;
        void RestoreChrome(PanelSlot& slot) noexcept;
        void RestorePanelVisuals(PanelSlot& slot, bool restoreBackground, bool& hadOrig) noexcept;
        [[nodiscard]] HRESULT RestoreSlotOnUiThread(
            std::shared_ptr<PanelSlot> slot,
            bool restoreBackground,
            bool& hadOrig,
            bool& usedDispatcher) noexcept;

        void ApplyPanelVisualsSync(PanelSlot& slot) noexcept;
        void ApplyPanelGuarded(std::shared_ptr<PanelSlot> slot) noexcept;

        Registry::Settings m_settings;
        std::shared_ptr<RegistryState> m_state{ std::make_shared<RegistryState>() };
        mutable std::mutex m_mx;
        std::map<InstanceHandle, std::shared_ptr<PanelSlot>> m_panels;
        std::atomic<unsigned> m_unresolved{ 0 };
    };
}
