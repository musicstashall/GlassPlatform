#pragma once

#include <Windows.h>
#include <array>
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
    // Пуск (StartMenuExperienceHost) + Поиск (SearchHost/SearchApp):
    // подтверждённые Border-цели AcrylicBorder/AcrylicOverlay/AppBorder/LayerBorder/DropShadow.
    // Параметры общие с треем: Tray\BorderDrawMode + Tray\Xaml\*.
    class ShellSurfacePanelRegistry
    {
    public:
        ShellSurfacePanelRegistry(
            IXamlDiagnostics* diagnostics,
            Registry::Settings settings) noexcept;
        ~ShellSurfacePanelRegistry() = default;

        ShellSurfacePanelRegistry(const ShellSurfacePanelRegistry&) = delete;
        ShellSurfacePanelRegistry& operator=(const ShellSurfacePanelRegistry&) = delete;

        void RegisterBorder(
            InstanceHandle handle,
            winrt::Windows::UI::Xaml::Controls::Border border) noexcept;

        void Unregister(InstanceHandle handle) noexcept;
        void RestoreAll() noexcept;
        void ReapplyPatchedBrushes() noexcept;
        void RecaptureAfterThemeChange(Registry::Settings settings) noexcept;
        void RefreshBrushesAfterThemeChange(Registry::Settings settings) noexcept;
        void UpdateSettings(Registry::Settings settings) noexcept;
        [[nodiscard]] unsigned PanelCount() const noexcept;
        void FlushPendingPrepares() noexcept;

    private:
        enum class Role : std::uint8_t
        {
            AcrylicBorder,   // основной фон Пуск
            AcrylicOverlay,  // наложение Пуск — Collapsed при paint
            AppBorder,       // фон рамки Поиск
            LayerBorder,     // слой Поиск — Visibility
            DropShadow,      // тень — Opacity=0
        };

        struct RegistryState
        {
            std::atomic<bool> shutdown{ false };
            std::atomic<int> pendingAsync{ 0 };
            std::atomic<bool> flushing{ false };
        };

        struct PanelSlot
        {
            struct OriginalProperty
            {
                unsigned index{ UINT_MAX };
                winrt::com_ptr<::IInspectable> value;
                bool captured{ false };
            };

            winrt::Windows::UI::Xaml::Controls::Border border{ nullptr };
            InstanceHandle handle{ 0 };
            Role role{ Role::AcrylicBorder };
            std::array<OriginalProperty, 4> originals{};
            bool chromeCaptured{ false };
            bool chromePatched{ false };
            bool pendingPrepare{ false };
            // После visual style: Stop не SetValue stale Background — VTS3 ClearProperty.
            bool clearBackgroundOnRestore{ false };

            void ClearWinrtRefs() noexcept
            {
                border = nullptr;
                for (auto& original : originals)
                    original.value = nullptr;
            }
        };

        [[nodiscard]] static Role ClassifyBorder(
            winrt::Windows::UI::Xaml::Controls::Border const& border) noexcept;

        void CaptureChrome(PanelSlot& slot) noexcept;
        void RestoreChrome(PanelSlot& slot) noexcept;
        void RestorePanelVisuals(PanelSlot& slot) noexcept;
        void ApplyPanelVisuals(PanelSlot& slot, const Registry::Settings& settings) noexcept;
        // Classify/Capture/Apply — только вне стека Advise (после FlushPendingPrepares).
        void PrepareAndApplyDeferred(
            std::shared_ptr<PanelSlot> slot,
            const Registry::Settings& settings) noexcept;

        Registry::Settings m_settings;
        winrt::com_ptr<IXamlDiagnostics> m_diagnostics;
        winrt::com_ptr<IVisualTreeService3> m_vts3;
        std::shared_ptr<RegistryState> m_state{ std::make_shared<RegistryState>() };
        mutable std::mutex m_mx;
        std::map<InstanceHandle, std::shared_ptr<PanelSlot>> m_panels;
    };
}
