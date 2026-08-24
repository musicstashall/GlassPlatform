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

#include "config/UiRegistry.h"

struct IXamlDiagnostics;

namespace UICompat
{
    // XAML-патч тултипов Пуска и Поиска (WUX ToolTip / Xaml_WindowedPopupClass в SMEH/SearchHost).
    // Параметры Tray\Xaml\Tooltip; chrome на ToolTip + первый внутренний Border → Transparent (анти-двойной blur).
    // SMEH: CornerRadius на Border через WinRT → fail-fast, только VTS3 (как NcMenuFlyout).
    // Apply синхронно на VTC-потоке; отложенные тела только через weak_ptr + shutdown.
    class ShellTooltipPanelRegistry
    {
    public:
        ShellTooltipPanelRegistry(
            IXamlDiagnostics* diagnostics,
            Registry::Settings settings) noexcept;
        ~ShellTooltipPanelRegistry() = default;

        ShellTooltipPanelRegistry(const ShellTooltipPanelRegistry&) = delete;
        ShellTooltipPanelRegistry& operator=(const ShellTooltipPanelRegistry&) = delete;

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

            // VTS3 handles для CornerRadius (SMEH WinRT → fail-fast).
            InstanceHandle tipHandle{ 0 };
            unsigned tipCornerPropIndex{ UINT_MAX };
            winrt::com_ptr<::IInspectable> originalTipCornerVts3;
            InstanceHandle innerHandle{ 0 };
            unsigned innerCornerPropIndex{ UINT_MAX };
            winrt::com_ptr<::IInspectable> originalInnerCornerVts3;

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
                tipHandle = 0;
                tipCornerPropIndex = UINT_MAX;
                originalTipCornerVts3 = nullptr;
                innerHandle = 0;
                innerCornerPropIndex = UINT_MAX;
                originalInnerCornerVts3 = nullptr;
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

        // VTS3 методы для CornerRadius (SMEH WinRT → fail-fast).
        [[nodiscard]] bool EnsureTipHandle(PanelSlot& slot) noexcept;
        [[nodiscard]] bool EnsureInnerHandle(PanelSlot& slot) noexcept;
        [[nodiscard]] bool CaptureTipCornerVts3(PanelSlot& slot) noexcept;
        [[nodiscard]] bool CaptureInnerCornerVts3(PanelSlot& slot) noexcept;
        [[nodiscard]] bool SetTipCornerVts3(
            PanelSlot& slot,
            winrt::Windows::UI::Xaml::CornerRadius const& cr) noexcept;
        [[nodiscard]] bool SetInnerCornerVts3(
            PanelSlot& slot,
            winrt::Windows::UI::Xaml::CornerRadius const& cr) noexcept;
        [[nodiscard]] bool RestoreTipCornerVts3(PanelSlot& slot) noexcept;
        [[nodiscard]] bool RestoreInnerCornerVts3(PanelSlot& slot) noexcept;
        [[nodiscard]] bool ClearPropByIndex(
            InstanceHandle element,
            unsigned propIndex) noexcept;

        Registry::Settings m_settings;
        winrt::com_ptr<IXamlDiagnostics> m_diagnostics;
        winrt::com_ptr<IVisualTreeService3> m_vts3;
        std::shared_ptr<RegistryState> m_state{ std::make_shared<RegistryState>() };
        mutable std::mutex m_mx;
        // Theme Recapture → Stop: Neutral/ThemeResource (не stale original).
        std::atomic<bool> m_themePreferNeutralRestore{ false };
        std::map<InstanceHandle, std::shared_ptr<PanelSlot>> m_panels;
        std::atomic<unsigned> m_unresolved{ 0 };
    };
}
