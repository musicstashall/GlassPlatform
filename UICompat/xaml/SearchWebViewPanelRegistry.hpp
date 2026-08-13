#pragma once

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <xamlOM.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Xaml.h>

#include "config/UiRegistry.h"

namespace UICompat
{
    // Search WebView CSS — путь Windhawk Start Menu Styler:
    // ApplyCustomizationsIfWebView по get_class_name
    // (Windows.UI.Xaml.Controls.WebView | WebView2Standalone.Controls.WebView2).
    class SearchWebViewPanelRegistry
    {
    public:
        explicit SearchWebViewPanelRegistry(Registry::Settings settings) noexcept;
        ~SearchWebViewPanelRegistry() = default;

        SearchWebViewPanelRegistry(const SearchWebViewPanelRegistry&) = delete;
        SearchWebViewPanelRegistry& operator=(const SearchWebViewPanelRegistry&) = delete;

        // Как донор ApplyCustomizationsIfWebView: решение только по get_class_name.
        void TryRegisterIfWebView(
            InstanceHandle handle,
            winrt::Windows::UI::Xaml::FrameworkElement element) noexcept;
        void Unregister(InstanceHandle handle) noexcept;
        void RestoreAll() noexcept;
        void ReapplyPatchedBrushes() noexcept;
        void UpdateSettings(Registry::Settings settings) noexcept;
        [[nodiscard]] unsigned PanelCount() const noexcept;

    private:
        struct PanelSlot
        {
            winrt::Windows::UI::Xaml::FrameworkElement element{ nullptr };
            InstanceHandle handle{ 0 };
            bool isWebView2{ false };
            bool cssApplied{ false };
            winrt::event_token navToken{};
            bool navHooked{ false };
            bool finishScheduled{ false };
        };

        void ApplyCss(PanelSlot& slot) noexcept;
        void ClearCss(PanelSlot& slot) noexcept;
        // QI / ExecuteScript / NavigationCompleted — только после выхода из Advise.
        void ScheduleFinishRegister(std::shared_ptr<PanelSlot> slot) noexcept;
        void FinishRegisterWebView(std::shared_ptr<PanelSlot> slot) noexcept;

        Registry::Settings m_settings;
        std::shared_ptr<std::atomic<bool>> m_shutdown{
            std::make_shared<std::atomic<bool>>(false) };
        mutable std::mutex m_mx;
        std::map<InstanceHandle, std::shared_ptr<PanelSlot>> m_panels;
    };
}
