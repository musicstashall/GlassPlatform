#pragma once
#include <atomic>
#include <memory>
#include <string_view>
#include <unknwn.h>
#include <xamlOM.h>

#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>

#include "config/UiRegistry.h"

struct IXamlDiagnostics;

namespace UICompat
{
    class NotificationCenterPanelRegistry;
    class ShellSurfacePanelRegistry;
    class NcMenuFlyoutPanelRegistry;
    class JumpListPanelRegistry;
    class SearchWebViewPanelRegistry;
    class ShellTooltipPanelRegistry;
    class ShellTooltipPanelRegistry;

    // VTC для CoreWindow hosts: NC grids + Start/Search + MenuFlyout (NC/QS/Start/Search) + Jump List.
    class UiVisualTreeWatcher final : public IVisualTreeServiceCallback2
    {
    public:
        UiVisualTreeWatcher(IXamlDiagnostics* diagnostics, Registry::Settings settings) noexcept;

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) noexcept override;
        ULONG STDMETHODCALLTYPE AddRef() noexcept override;
        ULONG STDMETHODCALLTYPE Release() noexcept override;

        HRESULT STDMETHODCALLTYPE OnVisualTreeChange(
            ParentChildRelation relation,
            VisualElement element,
            VisualMutationType mutationType) noexcept override;

        HRESULT STDMETHODCALLTYPE OnElementStateChanged(
            InstanceHandle element,
            VisualElementState elementState,
            LPCWSTR context) noexcept override;

        void RestoreAllFills() noexcept;
        void ReapplyPatchedBrushes() noexcept;
        void RecaptureAfterThemeChange() noexcept;
        /// Theme: invalidate keepExisting / force new brushes; originals не ClearValue.
        void RefreshBrushesAfterThemeChange() noexcept;
        void UpdateSettings(Registry::Settings settings) noexcept;
        // Если Advise подключён после построения дерева, Add не приходит: обходим Window.Current.
        [[nodiscard]] unsigned CatchUpExistingNcPanels() noexcept;
        [[nodiscard]] unsigned CatchUpExistingShellSurfacePanels() noexcept;
        [[nodiscard]] unsigned NcPanelCount() const noexcept;
        [[nodiscard]] unsigned ShellSurfacePanelCount() const noexcept;
        [[nodiscard]] unsigned MenuFlyoutPanelCount() const noexcept;
        [[nodiscard]] bool MenuThemePreferNeutral() const noexcept;
        void FlushPendingShellPrepares() noexcept;

    private:
        ~UiVisualTreeWatcher();

        HRESULT OnVisualTreeChangeImpl(
            ParentChildRelation relation,
            VisualElement element,
            VisualMutationType mutationType) noexcept;

        template<typename T>
        T FromHandle(InstanceHandle handle) noexcept;

        [[nodiscard]] bool TryRegisterNcPanel(
            InstanceHandle handle,
            std::wstring_view type,
            std::wstring_view name) noexcept;

        // Toast SEH: Border#ToastBackgroundBorder* / FlexibleNormalToastView.
        [[nodiscard]] bool TryRegisterToastTarget(
            InstanceHandle handle,
            std::wstring_view type,
            std::wstring_view name) noexcept;

        // NC/QS/Start/Search MenuFlyout: MenuFlyoutPresenter > Border
        // (PopupHost или in-tree в Search CoreWindow). User card / FlyoutPresenter — out of scope.
        [[nodiscard]] bool TryRegisterMenuFlyout(
            InstanceHandle handle,
            std::wstring_view type) noexcept;

        // SEH Jump List: Border#JumpListRestyledAcrylic.
        [[nodiscard]] bool TryRegisterJumpList(
            InstanceHandle handle,
            std::wstring_view type,
            std::wstring_view name) noexcept;

        [[nodiscard]] bool TryRegisterShellSurfaceBorder(
            InstanceHandle handle,
            std::wstring_view type,
            std::wstring_view name) noexcept;

        // Search WebView2: CSS .contextMenu (донор webViewTargetStyles).
        [[nodiscard]] bool TryRegisterSearchWebView(
            InstanceHandle handle,
            std::wstring_view type,
            std::wstring_view name) noexcept;

        // Start/Search ToolTip: WUX ToolTip в Xaml_WindowedPopupClass (SMEH/SearchHost).
        [[nodiscard]] bool TryRegisterShellTooltip(
            InstanceHandle handle,
            std::wstring_view type) noexcept;

        // Донор ждёт VTC Add WebView2Standalone; на SearchHost он часто
        // только внутри HostedWebView2Control — ищем по get_class_name в subtree.
        // Не на стеке Advise: Post через Dispatcher (AV WebView2Standalone).
        void DiscoverSearchWebViewSubtree(
            winrt::Windows::UI::Xaml::DependencyObject const& root) noexcept;
        void ScheduleDiscoverSearchWebViewSubtree(
            winrt::Windows::UI::Xaml::FrameworkElement const& host) noexcept;

        void CatchUpWalk(
            winrt::Windows::UI::Xaml::DependencyObject const& node,
            int depth,
            unsigned& registeredNc,
            unsigned& registeredShell) noexcept;

        std::atomic<ULONG> m_ref{ 1 };
        std::atomic<bool> m_diagUsable{ true };
        IXamlDiagnostics* m_xamlDiagnostics{ nullptr };
        std::unique_ptr<NotificationCenterPanelRegistry> m_ncRegistry;
        std::unique_ptr<ShellSurfacePanelRegistry> m_shellRegistry;
        std::unique_ptr<NcMenuFlyoutPanelRegistry> m_menuFlyoutRegistry;
        std::unique_ptr<JumpListPanelRegistry> m_jumpListRegistry;
        std::unique_ptr<SearchWebViewPanelRegistry> m_searchWebViewRegistry;
        std::unique_ptr<ShellTooltipPanelRegistry> m_shellTooltipRegistry;
    };
}
