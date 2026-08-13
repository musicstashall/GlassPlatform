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
    // XAML-патч контекстных меню трея / Пуск (WUX MenuFlyoutPresenter).
    // Chrome — дочерний Border (не GetElementVisual на Presenter — ломает hit-test).
    // Параметры Tray\Xaml\*; MaxRound: R от высоты трея; popupH < 2R → Increased.
    class TrayMenuFlyoutPanelRegistry
    {
    public:
        explicit TrayMenuFlyoutPanelRegistry(Registry::Settings settings) noexcept;
        ~TrayMenuFlyoutPanelRegistry() = default;

        TrayMenuFlyoutPanelRegistry(const TrayMenuFlyoutPanelRegistry&) = delete;
        TrayMenuFlyoutPanelRegistry& operator=(const TrayMenuFlyoutPanelRegistry&) = delete;

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
            // Live XAML→DWM: async SizeChanged/RunAsync читают актуальный путь, не stale copy.
            std::atomic<bool> paintPathXaml{ true };
        };

        struct PanelSlot
        {
            winrt::Windows::UI::Xaml::FrameworkElement root{ nullptr };
            winrt::Windows::UI::Xaml::Controls::Border chromeBorder{ nullptr };

            winrt::Windows::UI::Xaml::Media::Brush originalPresenterBackground{ nullptr };
            winrt::Windows::UI::Xaml::Media::Brush originalBorderBackground{ nullptr };
            winrt::Windows::UI::Xaml::Media::Brush originalBorderBrush{ nullptr };
            winrt::Windows::UI::Xaml::Thickness originalBorderThickness{};
            winrt::Windows::UI::Xaml::CornerRadius originalCornerRadius{};
            winrt::Windows::UI::Xaml::CornerRadius originalPresenterCornerRadius{};
            winrt::Windows::UI::Xaml::Media::Shadow originalShadow{ nullptr };

            bool chromeCaptured{ false };
            bool chromePatched{ false };
            // Фон Border/Presenter подменён CreateBackdropBrush (+ transparent Presenter).
            bool backgroundPatched{ false };
            // После theme Recapture: Stop → Neutral Acrylic (не keepPrev старой темы).
            bool preferNeutralBackgroundOnRestore{ false };
            bool presenterCornerCaptured{ false };
            bool presenterCornerPatched{ false };
            bool visualsResolved{ false };
            bool appliedOk{ false };
            bool needsRetry{ false };
            bool sizeHooked{ false };
            bool sizeApplyBusy{ false };
            bool sizeApplyDone{ false };
            ULONGLONG firstSeenTick{ 0 };

            // SizeChanged: CornerRadius после первого layout (иначе только после submenu).
            winrt::Windows::UI::Xaml::FrameworkElement::SizeChanged_revoker sizeRevoker{};

            void ClearWinrtRefs() noexcept
            {
                sizeRevoker = {};
                sizeHooked = false;
                root = nullptr;
                chromeBorder = nullptr;
                originalPresenterBackground = nullptr;
                originalBorderBackground = nullptr;
                originalBorderBrush = nullptr;
                originalShadow = nullptr;
                backgroundPatched = false;
                presenterCornerCaptured = false;
                presenterCornerPatched = false;
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
        std::map<InstanceHandle, std::shared_ptr<PanelSlot>> m_panels;
        std::atomic<unsigned> m_unresolved{ 0 };
        // Theme Recapture при закрытых меню (пустой m_panels) — лог
        // preferNeutral:0 usedOrig:1. Флаг на весь registry + новые Register.
        std::atomic<bool> m_themePreferNeutralRestore{ false };
    };
}
