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
    // XAML-патч Shell_InputSwitchTopLevelWindow (InputSwitcher) — Windhawk Taskbar Styler.
    // Apply синхронно на VTC-потоке; отложенные тела только через weak_ptr + shutdown.
    class InputSwitchPanelRegistry
    {
    public:
        explicit InputSwitchPanelRegistry(Registry::Settings settings) noexcept;
        ~InputSwitchPanelRegistry() = default;

        InputSwitchPanelRegistry(const InputSwitchPanelRegistry&) = delete;
        InputSwitchPanelRegistry& operator=(const InputSwitchPanelRegistry&) = delete;

        void RegisterPanel(
            InstanceHandle handle,
            winrt::Windows::UI::Xaml::FrameworkElement root) noexcept;

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
            winrt::Windows::UI::Xaml::Controls::Grid contentGrid{ nullptr };
            winrt::Windows::UI::Xaml::Controls::Grid innerGrid{ nullptr };

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
            ULONGLONG firstSeenTick{ 0 };

            void ClearWinrtRefs() noexcept
            {
                root = nullptr;
                contentGrid = nullptr;
                innerGrid = nullptr;
                originalBackground = nullptr;
                originalInnerBackground = nullptr;
                originalBorderBrush = nullptr;
                originalShadow = nullptr;
            }
        };

        [[nodiscard]] bool ResolveVisuals(PanelSlot& slot) noexcept;
        void CaptureChrome(PanelSlot& slot) noexcept;
        void RestoreChrome(PanelSlot& slot) noexcept;
        void RestorePanelVisuals(PanelSlot& slot, bool restoreBackground, bool& hadOrig) noexcept;
        [[nodiscard]] HRESULT RestoreSlotOnUiThread(
            std::shared_ptr<PanelSlot> slot,
            bool restoreBackground,
            bool& hadOrig,
            bool& usedDispatcher) noexcept;

        // Sync на вызывающем потоке (VTC UI InputSwitcher).
        void ApplyPanelVisualsSync(PanelSlot& slot) noexcept;
        // Reload с marshal-STA → RunAsync с weak_ptr + shutdown.
        void ApplyPanelGuarded(std::shared_ptr<PanelSlot> slot) noexcept;

        Registry::Settings m_settings;
        std::shared_ptr<RegistryState> m_state{ std::make_shared<RegistryState>() };
        mutable std::mutex m_mx;
        std::map<InstanceHandle, std::shared_ptr<PanelSlot>> m_panels;
        std::atomic<unsigned> m_unresolved{ 0 };
    };
}
