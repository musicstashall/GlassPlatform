#pragma once

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <xamlOM.h>

#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>

#include "config/TaskbarRegistry.h"

namespace TaskbarCompat
{
    // XAML-патч тултипов панели задач (WUX ToolTip / Xaml_WindowedPopupClass host).
    // Параметры — Tray\Xaml\*; MaxRound: R от высоты трея; popupH < 2R → Increased.
    // Apply синхронно на VTC-потоке; отложенные тела только через weak_ptr + shutdown.
    class TrayTooltipPanelRegistry
    {
    public:
        explicit TrayTooltipPanelRegistry(Registry::Settings settings) noexcept;
        ~TrayTooltipPanelRegistry() = default;

        TrayTooltipPanelRegistry(const TrayTooltipPanelRegistry&) = delete;
        TrayTooltipPanelRegistry& operator=(const TrayTooltipPanelRegistry&) = delete;

        void RegisterPanel(
            InstanceHandle handle,
            winrt::Windows::UI::Xaml::FrameworkElement root) noexcept;

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
            winrt::Windows::UI::Xaml::Controls::ToolTip tip{ nullptr };
            winrt::Windows::UI::Xaml::Controls::Border innerBorder{ nullptr };

            winrt::Windows::UI::Xaml::Media::Brush originalBackground{ nullptr };
            winrt::Windows::UI::Xaml::Media::Brush originalInnerBackground{ nullptr };
            winrt::Windows::UI::Xaml::Media::Brush originalBorderBrush{ nullptr };
            winrt::Windows::UI::Xaml::Thickness originalBorderThickness{};
            winrt::Windows::UI::Xaml::CornerRadius originalCornerRadius{};
            winrt::Windows::UI::Xaml::Media::Shadow originalShadow{ nullptr };

            bool chromeCaptured{ false };
            bool chromePatched{ false };
            bool visualsResolved{ false };
            bool appliedOk{ false };
            bool needsRetry{ false };
            bool sizeHooked{ false };
            bool sizeApplyBusy{ false };
            bool sizeApplyDone{ false };
            ULONGLONG firstSeenTick{ 0 };

            winrt::Windows::UI::Xaml::FrameworkElement::SizeChanged_revoker sizeRevoker{};

            void ClearWinrtRefs() noexcept
            {
                sizeRevoker = {};
                sizeHooked = false;
                tip = nullptr;
                innerBorder = nullptr;
                originalBackground = nullptr;
                originalInnerBackground = nullptr;
                originalBorderBrush = nullptr;
                originalShadow = nullptr;
            }
        };

        [[nodiscard]] bool ResolveVisuals(PanelSlot& slot) noexcept;
        void CaptureChrome(PanelSlot& slot) noexcept;
        void EnsureSizeHook(std::shared_ptr<PanelSlot> slot) noexcept;
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
        // Theme Recapture → Stop: Neutral/ThemeResource (не stale original).
        std::atomic<bool> m_themePreferNeutralRestore{ false };
        std::map<InstanceHandle, std::shared_ptr<PanelSlot>> m_panels;
        std::atomic<unsigned> m_unresolved{ 0 };
    };
}
