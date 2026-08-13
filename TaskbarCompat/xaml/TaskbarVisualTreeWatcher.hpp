#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <unknwn.h>
#include <vector>
#include <xamlOM.h>

#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>

#include "config/TaskbarRegistry.h"
#include "xaml/InputSwitchPanelRegistry.hpp"
#include "xaml/TrayOverflowPanelRegistry.hpp"
#include "xaml/TrayHoverFlyoutPanelRegistry.hpp"
#include "xaml/TrayTooltipPanelRegistry.hpp"
#include "xaml/TrayMenuFlyoutPanelRegistry.hpp"

struct IXamlDiagnostics;

namespace TaskbarCompat
{
    class TaskbarBackgroundRegistry;

    // UWP Visual Tree Watcher для панели задач Win11.
    //
    // Минимальная схема: реагируем только на Add Rectangle name=BackgroundFill /
    // BackgroundStroke. Через FindParent("TaskbarFrame", parentElement) поднимаемся
    // до фрейма, получаем его frameHandle через GetHandleFromIInspectable и
    // upsert-ом регистрируем в TaskbarBackgroundRegistry. На Remove —
    // UnregisterFrame по handle.
    //
    // Ветки Add DesktopWindowXamlSource и Add Taskbar.TaskbarFrame намеренно
    // пропускаются: HWND острова в TaskbarCompat не используется (SWCA / эффекты
    // задаёт GlassCore на Shell_TrayWnd), а ABI-вызовы внутри них (FromHandle,
    // Content(), QueryInterface(IDesktopWindowXamlSourceNative)) на Win11 24H2
    // детерминированно роняли explorer.exe.
    class TaskbarVisualTreeWatcher final : public IVisualTreeServiceCallback2
    {
    public:
        TaskbarVisualTreeWatcher(IXamlDiagnostics* diagnostics, Registry::Settings settings) noexcept;

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

        // Восстановить все оригинальные кисти — вызывается перед UnadviseVisualTreeChange
        // (TaskbarTapSite::ShutdownWatcher).
        void RestoreAllFills() noexcept;

        // Повторно выставить кисти (ручной вызов; обычно не нужно — см. callbacks Fill на Rectangle).
        void ReapplyPatchedBrushes() noexcept;

        /// Theme/visual style: ClearValue → re-sample originals → Apply (не stale original*).
        void RecaptureAfterThemeChange(Registry::Settings barSettings) noexcept;

        void UpdateSettings(Registry::Settings settings) noexcept;

        // Применить патч Rectangle по diagnostic handles (STA / маршалинг с потока VTC).
        [[nodiscard]] bool ApplyRectangleAddFromHandles(
            InstanceHandle parentHandle,
            InstanceHandle elementHandle,
            bool isBackgroundFill) noexcept;

        /// Повторить отложенные Rectangle, у которых ещё не было TaskbarFrame в дереве.
        void RetryPendingRectanglePatches() noexcept;

    private:
        struct PendingRectanglePatch
        {
            InstanceHandle parent{};
            InstanceHandle element{};
            bool           isBackgroundFill = false;
        };

        void EnqueuePendingRectanglePatch(
            InstanceHandle parentHandle,
            InstanceHandle elementHandle,
            bool isBackgroundFill) noexcept;

        ~TaskbarVisualTreeWatcher();

        // Тело коллбэка (SEH — во внешней обёртке OnVisualTreeChange).
        HRESULT OnVisualTreeChangeImpl(
            ParentChildRelation relation,
            VisualElement element,
            VisualMutationType mutationType) noexcept;

        template<typename T>
        T FromHandle(InstanceHandle handle) noexcept;

        winrt::Windows::UI::Xaml::FrameworkElement FindParent(
            std::wstring_view name,
            winrt::Windows::UI::Xaml::FrameworkElement element) noexcept;

        std::atomic<ULONG> m_ref{ 1 };
        // false после SEH/AV на IXamlDiagnostics — дальше FromHandle no-op (не ронять explorer).
        std::atomic<bool> m_diagUsable{ true };
        IXamlDiagnostics* m_xamlDiagnostics{ nullptr };
        std::unique_ptr<TaskbarBackgroundRegistry> m_registry;
        std::unique_ptr<InputSwitchPanelRegistry> m_inputSwitchRegistry;
        std::unique_ptr<TrayOverflowPanelRegistry> m_overflowRegistry;
        std::unique_ptr<TrayHoverFlyoutPanelRegistry> m_hoverFlyoutRegistry;
        std::unique_ptr<TrayTooltipPanelRegistry> m_tooltipRegistry;
        std::unique_ptr<TrayMenuFlyoutPanelRegistry> m_menuFlyoutRegistry;
        std::mutex m_pendingMx;
        std::vector<PendingRectanglePatch> m_pendingRectangles;
    };
}
