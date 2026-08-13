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
    // XAML-патч TopLevelWindowForOverflowXamlIsland (SystemTray.NotificationAreaOverflow) —
    // Windhawk Taskbar Styler parity. Apply синхронно на VTC-потоке; отложенные тела
    // только через weak_ptr + shutdown.
    class TrayOverflowPanelRegistry
    {
    public:
        explicit TrayOverflowPanelRegistry(Registry::Settings settings) noexcept;
        ~TrayOverflowPanelRegistry() = default;

        TrayOverflowPanelRegistry(const TrayOverflowPanelRegistry&) = delete;
        TrayOverflowPanelRegistry& operator=(const TrayOverflowPanelRegistry&) = delete;

        // Якорь: SystemTray.NotificationAreaOverflow.
        void RegisterPanel(
            InstanceHandle handle,
            winrt::Windows::UI::Xaml::FrameworkElement root) noexcept;

        // Border#OverflowFlyoutBackgroundBorder (может прийти раньше/без якоря).
        void RegisterBackgroundBorder(
            InstanceHandle handle,
            winrt::Windows::UI::Xaml::Controls::Border border) noexcept;

        // Событийный ретрай: вызывать на каждом VTC Add, пока есть нерешённые slot-ы.
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
            winrt::Windows::UI::Xaml::Controls::Grid rootGrid{ nullptr };

            InstanceHandle borderHandle{ 0 };

            winrt::Windows::UI::Xaml::Media::Brush originalBackground{ nullptr };
            winrt::Windows::UI::Xaml::Media::Brush originalRootGridBackground{ nullptr };
            winrt::Windows::UI::Xaml::Media::Brush originalBorderBrush{ nullptr };
            winrt::Windows::UI::Xaml::Thickness originalBorderThickness{};
            winrt::Windows::UI::Xaml::CornerRadius originalCornerRadius{};
            winrt::Windows::UI::Xaml::Media::Shadow originalShadow{ nullptr };

            bool chromeCaptured{ false };
            bool chromePatched{ false };
            bool visualsResolved{ false };
            bool appliedOk{ false };
            bool needsRetry{ false };
            bool resolveFailLogged{ false };
            bool sizeHooked{ false };
            bool sizeApplyBusy{ false };
            bool sizeApplyDone{ false };
            ULONGLONG firstSeenTick{ 0 };

            // Одноразовый SizeChanged: CornerRadius только после layout > 0.
            winrt::Windows::UI::Xaml::FrameworkElement::SizeChanged_revoker sizeRevoker{};

            void ClearWinrtRefs() noexcept
            {
                sizeRevoker = {};
                sizeHooked = false;
                root = nullptr;
                backgroundBorder = nullptr;
                rootGrid = nullptr;
                originalBackground = nullptr;
                originalRootGridBackground = nullptr;
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

        // Sync на вызывающем потоке (VTC UI overflow).
        void ApplyPanelVisualsSync(PanelSlot& slot) noexcept;
        // Reload с marshal-STA → RunAsync с weak_ptr + shutdown.
        void ApplyPanelGuarded(std::shared_ptr<PanelSlot> slot) noexcept;

        Registry::Settings m_settings;
        std::shared_ptr<RegistryState> m_state{ std::make_shared<RegistryState>() };
        mutable std::mutex m_mx;
        // После theme Recapture: Stop → ClearValue (не SetValue stale brush).
        bool m_themePreferClearRestore{ false };
        std::map<InstanceHandle, std::shared_ptr<PanelSlot>> m_panels;
        std::atomic<unsigned> m_unresolved{ 0 };
    };
}
