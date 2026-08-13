#pragma once

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <xamlOM.h>

#include <winrt/base.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>

#include "config/UiRegistry.h"

struct IXamlDiagnostics;

namespace UICompat
{
    // XAML-патч контекстных меню NC/QS, Пуск и Поиск (WUX MenuFlyoutPresenter).
    // NC/QS/Start: Xaml_WindowedPopupClass; Search: in-tree в CoreWindow.
    // Chrome — дочерний Border (не GetElementVisual на Presenter — ломает hit-test).
    // Параметры Tray\Xaml\*; MaxRound: R от высоты трея; popupH < 2R → Increased.
    // Start (SMEH): CornerRadius Border + Presenter только через VTS3 (WinRT → fail-fast).
    // Search: Blur (effect 3) → Acrylic (4), как ShellSurface (AV GetElementVisual).
    // Как TrayMenuFlyout / Windhawk: системный MenuFlyoutPresenter > Border (+ Presenter CR).
    // FlyoutPresenter / user card — не трогаем.
    // DWM-путь: restore только если слот уже патчили; VTS3 fail → ClearProperty.
    class NcMenuFlyoutPanelRegistry
    {
    public:
        NcMenuFlyoutPanelRegistry(
            IXamlDiagnostics* diagnostics,
            Registry::Settings settings) noexcept;
        ~NcMenuFlyoutPanelRegistry() = default;

        NcMenuFlyoutPanelRegistry(const NcMenuFlyoutPanelRegistry&) = delete;
        NcMenuFlyoutPanelRegistry& operator=(const NcMenuFlyoutPanelRegistry&) = delete;

        void RegisterPanel(
            InstanceHandle handle,
            winrt::Windows::UI::Xaml::FrameworkElement root) noexcept;

        void RetryUnresolvedPanels() noexcept;

        void Unregister(InstanceHandle handle) noexcept;
        void RestoreAll() noexcept;
        void ReapplyPatchedBrushes() noexcept;
        void RecaptureAfterThemeChange(Registry::Settings settings) noexcept;
        void RefreshBrushesAfterThemeChange(Registry::Settings settings) noexcept;
        void UpdateSettings(Registry::Settings settings) noexcept;
        [[nodiscard]] unsigned PanelCount() const noexcept;
        [[nodiscard]] unsigned UnresolvedCount() const noexcept;
        [[nodiscard]] bool ThemePreferNeutralRestore() const noexcept
        {
            return m_themePreferNeutralRestore.load(std::memory_order_acquire);
        }

    private:
        struct RegistryState
        {
            std::atomic<bool> shutdown{ false };
            std::atomic<int> pendingAsync{ 0 };
        };

        struct PanelSlot
        {
            winrt::Windows::UI::Xaml::FrameworkElement root{ nullptr };
            winrt::Windows::UI::Xaml::Controls::Border chromeBorder{ nullptr };
            InstanceHandle chromeHandle{ 0 };
            unsigned cornerPropIndex{ UINT_MAX };
            winrt::com_ptr<::IInspectable> originalCornerVts3;
            InstanceHandle presenterHandle{ 0 };
            unsigned presenterCornerPropIndex{ UINT_MAX };
            winrt::com_ptr<::IInspectable> originalPresenterCornerVts3;

            winrt::Windows::UI::Xaml::Media::Brush originalPresenterBackground{ nullptr };
            winrt::Windows::UI::Xaml::Media::Brush originalBorderBackground{ nullptr };
            // SMEH: WinRT Background() часто null — системный acrylic только через VTS3.
            unsigned borderBackgroundPropIndex{ UINT_MAX };
            winrt::com_ptr<::IInspectable> originalBorderBackgroundVts3;
            winrt::Windows::UI::Xaml::Media::Brush originalBorderBrush{ nullptr };
            winrt::Windows::UI::Xaml::Thickness originalBorderThickness{};
            winrt::Windows::UI::Xaml::CornerRadius originalCornerRadius{};
            winrt::Windows::UI::Xaml::CornerRadius originalPresenterCornerRadius{};
            winrt::Windows::UI::Xaml::Media::Shadow originalShadow{ nullptr };

            bool chromeCaptured{ false };
            bool chromePatched{ false };
            // SMEH: Background патчим только после capture original (иначе ClearValue → прозрачность).
            bool backgroundPatched{ false };
            // После visual style: Stop ClearValue BG (не SetValue stale theme brush).
            bool clearBackgroundOnRestore{ false };
            bool presenterCornerCaptured{ false };
            bool presenterCornerPatched{ false };
            bool visualsResolved{ false };
            bool appliedOk{ false };
            bool needsRetry{ false };
            bool sizeHooked{ false };
            bool sizeApplyBusy{ false };
            bool sizeApplyDone{ false };
            ULONGLONG firstSeenTick{ 0 };
            ULONGLONG lastBgRetryTick{ 0 };

            // SizeChanged: CornerRadius после первого layout (иначе только после submenu).
            winrt::Windows::UI::Xaml::FrameworkElement::SizeChanged_revoker sizeRevoker{};

            void ClearWinrtRefs() noexcept
            {
                sizeRevoker = {};
                sizeHooked = false;
                root = nullptr;
                chromeBorder = nullptr;
                chromeHandle = 0;
                cornerPropIndex = UINT_MAX;
                originalCornerVts3 = nullptr;
                presenterHandle = 0;
                presenterCornerPropIndex = UINT_MAX;
                originalPresenterCornerVts3 = nullptr;
                originalPresenterBackground = nullptr;
                originalBorderBackground = nullptr;
                borderBackgroundPropIndex = UINT_MAX;
                originalBorderBackgroundVts3 = nullptr;
                backgroundPatched = false;
                originalBorderBrush = nullptr;
                originalShadow = nullptr;
            }

            void ResetCornerCapture() noexcept
            {
                chromeHandle = 0;
                cornerPropIndex = UINT_MAX;
                originalCornerVts3 = nullptr;
                presenterHandle = 0;
                presenterCornerPropIndex = UINT_MAX;
                originalPresenterCornerVts3 = nullptr;
                originalCornerRadius = {};
                presenterCornerCaptured = false;
                presenterCornerPatched = false;
            }
        };

        struct MarshalRestoreJob
        {
            NcMenuFlyoutPanelRegistry* self{ nullptr };
            std::shared_ptr<PanelSlot> slot;
            bool restoreBackground{ true };
            bool hadOrig{ false };
            HRESULT hr{ S_OK };
        };

        [[nodiscard]] bool ResolveVisuals(PanelSlot& slot) noexcept;
        void CaptureChrome(PanelSlot& slot) noexcept;
        void EnsureSizeHook(std::shared_ptr<PanelSlot> slot) noexcept;
        void RestoreChrome(PanelSlot& slot) noexcept;
        void RestorePresenterCorner(PanelSlot& slot) noexcept;
        void RestorePanelVisuals(PanelSlot& slot, bool restoreBackground, bool& hadOrig) noexcept;
        static void RestoreMarshalThunk(void* param) noexcept;
        [[nodiscard]] HRESULT RestoreSlotOnUiThread(
            std::shared_ptr<PanelSlot> slot,
            bool restoreBackground,
            bool& hadOrig,
            bool& usedDispatcher) noexcept;

        void ApplyPanelVisualsSync(PanelSlot& slot) noexcept;
        void ApplyPanelGuarded(std::shared_ptr<PanelSlot> slot) noexcept;

        [[nodiscard]] bool EnsureChromeHandle(PanelSlot& slot) noexcept;
        [[nodiscard]] bool EnsurePresenterHandle(PanelSlot& slot) noexcept;
        [[nodiscard]] bool CaptureCornerVts3(PanelSlot& slot) noexcept;
        [[nodiscard]] bool CaptureBackgroundVts3(PanelSlot& slot) noexcept;
        [[nodiscard]] bool RestoreBackgroundVts3(PanelSlot& slot) noexcept;
        [[nodiscard]] bool CapturePresenterCornerVts3(PanelSlot& slot) noexcept;
        [[nodiscard]] bool SetCornerVts3(
            PanelSlot& slot,
            winrt::Windows::UI::Xaml::CornerRadius const& cr) noexcept;
        [[nodiscard]] bool SetPresenterCornerVts3(
            PanelSlot& slot,
            winrt::Windows::UI::Xaml::CornerRadius const& cr) noexcept;
        [[nodiscard]] bool RestoreCornerVts3(PanelSlot& slot) noexcept;
        [[nodiscard]] bool RestorePresenterCornerVts3(PanelSlot& slot) noexcept;
        [[nodiscard]] bool ClearPropByIndex(
            InstanceHandle element,
            unsigned propIndex) noexcept;

        Registry::Settings m_settings;
        winrt::com_ptr<IXamlDiagnostics> m_diagnostics;
        winrt::com_ptr<IVisualTreeService3> m_vts3;
        std::shared_ptr<RegistryState> m_state{ std::make_shared<RegistryState>() };
        mutable std::mutex m_mx;
        std::map<InstanceHandle, std::shared_ptr<PanelSlot>> m_panels;
        std::atomic<unsigned> m_unresolved{ 0 };
        // Theme refresh при закрытых меню (пустой snapshot) — новые Register
        // без clearBackgroundOnRestore → Stop SetValue stale (лог TrayMenu epoch).
        std::atomic<bool> m_themePreferNeutralRestore{ false };
    };
}
