#pragma once

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <xamlOM.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>

#include "config/UiRegistry.h"

namespace UICompat
{
    // Local-value снимок DP: Unset/Binding → restore через ClearValue.
    struct NcLocalPropCapture
    {
        winrt::Windows::Foundation::IInspectable value{ nullptr };
        bool captured{ false };
        bool clearOnRestore{ false }; // Unset или BindingExpression*
    };

    // Фон + системный Border* / CornerRadius NC Grids (модель Windhawk NC Styler).
    // Capture через ReadLocalValue (+ BindingExpression → ClearValue на restore).
    // Отложенные тела — только weak_ptr + shutdown; restore через heap-payload.
    class NotificationCenterPanelRegistry
    {
    public:
        explicit NotificationCenterPanelRegistry(Registry::Settings settings) noexcept;
        ~NotificationCenterPanelRegistry() = default;

        NotificationCenterPanelRegistry(const NotificationCenterPanelRegistry&) = delete;
        NotificationCenterPanelRegistry& operator=(const NotificationCenterPanelRegistry&) = delete;

        void RegisterPanel(
            InstanceHandle handle,
            winrt::Windows::UI::Xaml::Controls::Grid grid) noexcept;

        // Toast (SEH): Border#ToastBackgroundBorder* — blur/chrome как NC Grid.
        void RegisterToastBorder(
            InstanceHandle handle,
            winrt::Windows::UI::Xaml::Controls::Border border) noexcept;

        // Toast: FlexibleNormalToastView — только Transparent Background.
        void RegisterToastViewClear(
            InstanceHandle handle,
            winrt::Windows::UI::Xaml::FrameworkElement element) noexcept;

        void Unregister(InstanceHandle handle) noexcept;
        void RestoreAll() noexcept;
        void ReapplyPatchedBrushes() noexcept;
        void RecaptureAfterThemeChange(Registry::Settings settings) noexcept;
        /// Theme: сброс keepExistingBg + новый brush; без ClearValue / без null originals.
        void RefreshBrushesAfterThemeChange(Registry::Settings settings) noexcept;
        void UpdateSettings(Registry::Settings settings) noexcept;
        [[nodiscard]] unsigned PanelCount() const noexcept;

        // Thunk UI-потока (Win32 marshal / CoreDispatcher) — public из‑за lambda access.
        static void RestoreMarshalThunk(void* param) noexcept;

    private:
        struct RegistryState
        {
            std::atomic<bool> shutdown{ false };
            std::atomic<int> pendingAsync{ 0 };
        };

        enum class SlotKind : uint8_t
        {
            NcGrid = 0,
            ToastBorder,
            ToastViewClear,
        };

        struct PanelSlot
        {
            SlotKind kind{ SlotKind::NcGrid };
            winrt::Windows::UI::Xaml::Controls::Grid grid{ nullptr };
            winrt::Windows::UI::Xaml::Controls::Border border{ nullptr };
            winrt::Windows::UI::Xaml::FrameworkElement element{ nullptr };
            NcLocalPropCapture originalBackground{};
            NcLocalPropCapture originalBorderBrush{};
            NcLocalPropCapture originalBorderThickness{};
            NcLocalPropCapture originalCornerRadius{};
            // Fallback для Apply (толщина рамки), если local thickness concrete.
            winrt::Windows::UI::Xaml::Thickness originalBorderThicknessValue{};
            winrt::Windows::UI::Xaml::Controls::Border l1OuterBorder{ nullptr };
            winrt::Windows::UI::Xaml::Visibility originalL1Visibility{
                winrt::Windows::UI::Xaml::Visibility::Visible };
            bool chromeCaptured{ false };
            bool chromePatched{ false };
            // Background подменён (backdrop / Transparent Root / toast clear).
            bool backgroundPatched{ false };
            // Visual style: Stop всегда ClearValue BG (watcher не должен сбрасывать).
            bool themeClearBackgroundOnRestore{ false };
            // Запрошенный EffectType из settings (до remap Acrylic→Blur).
            DWORD appliedRequestedEffect{ 0xFFFFFFFFu };
            DWORD appliedEffectType{ 0xFFFFFFFFu };
            DWORD appliedTintArgb{ 0 };
            bool l1Patched{ false };
            bool reassertArmed{ false };
            std::uint32_t backdropRetryCount{ 0 };
            // http(s) обложки MediaTransportControlsRoot — rehydrate после Stop,
            // если ImageBrush.ImageSource был сброшен retry-логикой.
            winrt::hstring mediaAlbumImageUri{};
            // Debug: чужой сброс Background после нашего Apply.
            bool bgWatchRegistered{ false };
            int64_t bgWatchToken{ 0 };
            std::uint32_t programmaticBgNesting{ 0 };

            void ClearWinrtRefs() noexcept
            {
                if (bgWatchRegistered && grid)
                {
                    try
                    {
                        grid.UnregisterPropertyChangedCallback(
                            winrt::Windows::UI::Xaml::Controls::Panel::BackgroundProperty(),
                            bgWatchToken);
                    }
                    catch (...)
                    {
                    }
                }
                bgWatchRegistered = false;
                bgWatchToken = 0;
                grid = nullptr;
                border = nullptr;
                element = nullptr;
                originalBackground = {};
                originalBorderBrush = {};
                originalBorderThickness = {};
                originalCornerRadius = {};
                l1OuterBorder = nullptr;
                mediaAlbumImageUri = {};
                backgroundPatched = false;
                appliedRequestedEffect = 0xFFFFFFFFu;
                appliedEffectType = 0xFFFFFFFFu;
                appliedTintArgb = 0;
            }
        };

        struct MarshalRestoreJob
        {
            std::shared_ptr<PanelSlot> slot;
            bool restoreBackground{ true };
            bool clearRefs{ true }; // Stop: true; live XAML→DWM keepAlive: false
            bool hadOrig{ false };
            HRESULT hr{ S_OK };
        };

        static void CaptureChrome(PanelSlot& slot) noexcept;
        static void RestoreChrome(PanelSlot& slot) noexcept;
        static void RestorePanelVisuals(PanelSlot& slot, bool restoreBackground, bool& hadOrig) noexcept;

        // clearRefs=true — Stop (слот уходит); false — live DWM-switch (TAP жив).
        [[nodiscard]] HRESULT RestoreSlotOnUiThread(
            std::shared_ptr<PanelSlot> slot,
            bool restoreBackground,
            bool& hadOrig,
            bool& usedDispatcher,
            bool clearRefs = true) noexcept;
        void ApplyPanelVisuals(PanelSlot& slot, const Registry::Settings& settings) noexcept;
        // true — нужен отложенный ретрай кисти (≤ kMaxBackdropRetries).
        [[nodiscard]] bool ApplyPanelVisualsEx(
            PanelSlot& slot,
            const Registry::Settings& settings) noexcept;
        void ApplyPanelGuarded(std::shared_ptr<PanelSlot> slot) noexcept;
        void ScheduleDeferredReassert(
            std::shared_ptr<PanelSlot> slot,
            std::shared_ptr<RegistryState> state,
            Registry::Settings settings) noexcept;

        Registry::Settings m_settings;
        std::shared_ptr<RegistryState> m_state{ std::make_shared<RegistryState>() };
        mutable std::mutex m_mx;
        std::map<InstanceHandle, std::shared_ptr<PanelSlot>> m_panels;
    };
}
