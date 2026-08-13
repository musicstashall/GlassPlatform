#include "TaskbarPch.h"
#include "xaml/TrayHoverFlyoutPanelRegistry.hpp"
#include "xaml/brushes/XamlBackdropFactory.hpp"

#include <TrayXamlPaint.h>

#include <DebugLog.h>

#include <string>
#include <string_view>
#include <vector>

#include <windows.ui.xaml.shapes.h>

#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Markup.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>

namespace
{
    // put_RadiusX/Y на Taskbar BackgroundFill трея → AV.
    // HoverFlyout: Matter/guide — Fill+CR на Control / Fill на Rectangle; RadiusX на Fill — только
    // отдельные темы (не базовый Matter). Только ABI + __try (без C++ в SEH).
    [[nodiscard]] HRESULT SafePutRectangleRadiiXY(IUnknown* unk, double rx, double ry) noexcept
    {
        if (!unk)
            return E_POINTER;
        ABI::Windows::UI::Xaml::Shapes::IRectangle* rect = nullptr;
        const HRESULT qi = unk->QueryInterface(
            __uuidof(ABI::Windows::UI::Xaml::Shapes::IRectangle),
            reinterpret_cast<void**>(&rect));
        if (FAILED(qi) || !rect)
            return FAILED(qi) ? qi : E_NOINTERFACE;

        HRESULT hr = S_OK;
        __try
        {
            hr = rect->put_RadiusX(rx);
            if (SUCCEEDED(hr))
                hr = rect->put_RadiusY(ry);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            hr = E_FAIL;
        }
        rect->Release();
        return hr;
    }

    [[nodiscard]] HRESULT SafePutRectangleRadii(IUnknown* unk, double r) noexcept
    {
        return SafePutRectangleRadiiXY(unk, r, r);
    }

    [[nodiscard]] HRESULT SafeGetRectangleRadiiXY(
        IUnknown* unk, double& rx, double& ry) noexcept
    {
        rx = 0.0;
        ry = 0.0;
        if (!unk)
            return E_POINTER;
        ABI::Windows::UI::Xaml::Shapes::IRectangle* rect = nullptr;
        const HRESULT qi = unk->QueryInterface(
            __uuidof(ABI::Windows::UI::Xaml::Shapes::IRectangle),
            reinterpret_cast<void**>(&rect));
        if (FAILED(qi) || !rect)
            return FAILED(qi) ? qi : E_NOINTERFACE;
        HRESULT hr = S_OK;
        __try
        {
            hr = rect->get_RadiusX(&rx);
            if (SUCCEEDED(hr))
                hr = rect->get_RadiusY(&ry);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            hr = E_FAIL;
        }
        rect->Release();
        return hr;
    }
}

namespace TaskbarCompat
{
    namespace
    {
        namespace wux = winrt::Windows::UI::Xaml;

        constexpr DWORD kRestoreWaitMs = 250;


        // H59/H63 Matter отвергнуты. H72 plate-cover отвергнут (регресс backdrop).
        // Не читать ActualWidth/Height на HoverFlyout: winrt get_ActualHeight → AV 0xC0000005
        // (C++ catch не ловит). Донор (Taskbar Styler) задаёт CR константой через SetValue.

        [[nodiscard]] winrt::Windows::UI::Color ColorFromArgb(DWORD argb) noexcept
        {
            return winrt::Windows::UI::Color{
                static_cast<uint8_t>((argb >> 24) & 0xFFu),
                static_cast<uint8_t>((argb >> 16) & 0xFFu),
                static_cast<uint8_t>((argb >> 8) & 0xFFu),
                static_cast<uint8_t>(argb & 0xFFu),
            };
        }

        // Как Overflow: R от высоты трея (без измерения flyout element).
        [[nodiscard]] double HoverCornerRadiusDip(DWORD cornerType) noexcept
        {
            return Shared::TrayXamlPaint::XamlCornerRadiusDip(
                cornerType, Shared::TrayXamlPaint::QueryTaskbarHeightDip());
        }

        [[nodiscard]] bool ThicknessAnyPositive(const wux::Thickness& t) noexcept
        {
            return t.Left > 0.0 || t.Top > 0.0 || t.Right > 0.0 || t.Bottom > 0.0;
        }

        // Донор: CornerRadius на Border#HoverFlyoutBackground; fallback Grid#HoverFlyoutGrid.
        [[nodiscard]] bool TrySetCornerRadius(
            wux::Controls::Border const& border,
            wux::Controls::Grid const& grid,
            const wux::CornerRadius& cr,
            bool& borderOk,
            bool& gridOk) noexcept
        {
            borderOk = false;
            gridOk = false;
            if (border)
            {
                try
                {
                    border.CornerRadius(cr);
                    borderOk = true;
                }
                catch (...)
                {
                }
            }
            if (borderOk)
                return true;
            if (grid)
            {
                try
                {
                    grid.CornerRadius(cr);
                    gridOk = true;
                    return true;
                }
                catch (...)
                {
                }
            }
            return false;
        }

        void RestoreRectangleFill(
            wux::Shapes::Rectangle const& rect,
            wux::Media::Brush const& originalFill,
            bool& hadOrig) noexcept
        {
            hadOrig = static_cast<bool>(originalFill);
            try
            {
                rect.ClearValue(wux::Shapes::Shape::FillProperty());
                if (hadOrig)
                    rect.Fill(originalFill);
                try
                {
                    rect.Clip(nullptr);
                }
                catch (...)
                {
                }
                rect.InvalidateArrange();
                rect.InvalidateMeasure();
            }
            catch (...)
            {
            }
        }

        void RestoreBorderBackground(
            wux::Controls::Border const& border,
            wux::Media::Brush const& originalBackground,
            bool& hadOrig) noexcept
        {
            hadOrig = static_cast<bool>(originalBackground);
            try
            {
                border.ClearValue(wux::Controls::Border::BackgroundProperty());
                if (hadOrig)
                    border.Background(originalBackground);
                border.InvalidateArrange();
                border.InvalidateMeasure();
            }
            catch (...)
            {
            }
        }

        void PinModuleIfPendingAsync(int pending) noexcept
        {
            if (pending <= 0)
                return;
            HMODULE self = nullptr;
            if (!GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                        | GET_MODULE_HANDLE_EX_FLAG_PIN,
                    reinterpret_cast<LPCWSTR>(&PinModuleIfPendingAsync),
                    &self))
            {
                DebugLog(
                    DBG_WARNING,
                    L"[TaskbarCompat][HoverFlyout] PIN failed GLE=%u pendingAsync=%d",
                    static_cast<unsigned>(GetLastError()),
                    pending);
                return;
            }
            DebugLog(
                DBG_WARNING,
                L"[TaskbarCompat][HoverFlyout] module PIN (pendingAsync=%d)",
                pending);
        }
    }

    TrayHoverFlyoutPanelRegistry::TrayHoverFlyoutPanelRegistry(Registry::Settings settings) noexcept
        : m_settings(settings)
    {
    }

    unsigned TrayHoverFlyoutPanelRegistry::PanelCount() const noexcept
    {
        std::lock_guard lock(m_mx);
        return static_cast<unsigned>(m_panels.size());
    }

    unsigned TrayHoverFlyoutPanelRegistry::UnresolvedCount() const noexcept
    {
        return m_unresolved.load(std::memory_order_acquire);
    }

    bool TrayHoverFlyoutPanelRegistry::TryAttachNamedRect(
        PanelSlot& slot,
        std::wstring_view name,
        wux::Shapes::Rectangle& outRect,
        wux::Media::Brush* outOriginalFill) noexcept
    {
        if (outRect)
            return true;
        if (!slot.root)
            return false;

        const auto findNamed =
            [&name](auto&& self, wux::DependencyObject const& node, int depth) noexcept
            -> wux::Shapes::Rectangle
        {
            if (!node || depth > 3)
                return nullptr;
            try
            {
                if (const auto fe = node.try_as<wux::FrameworkElement>())
                {
                    try
                    {
                        if (fe.Name() == name)
                        {
                            if (const auto rect = fe.try_as<wux::Shapes::Rectangle>())
                                return rect;
                        }
                    }
                    catch (...)
                    {
                    }
                }
                const int n = wux::Media::VisualTreeHelper::GetChildrenCount(node);
                for (int i = 0; i < n; ++i)
                {
                    if (const auto found = self(
                            self, wux::Media::VisualTreeHelper::GetChild(node, i), depth + 1))
                        return found;
                }
            }
            catch (...)
            {
            }
            return nullptr;
        };

        try
        {
            if (const auto rect = findNamed(findNamed, slot.root, 0))
            {
                outRect = rect;
                if (outOriginalFill && !*outOriginalFill)
                {
                    try
                    {
                        *outOriginalFill = rect.Fill();
                    }
                    catch (...)
                    {
                    }
                }
                return true;
            }
        }
        catch (...)
        {
        }
        return static_cast<bool>(outRect);
    }

    bool TrayHoverFlyoutPanelRegistry::TryAttachFill(PanelSlot& slot) noexcept
    {
        // Только от якоря HoverFlyoutBackgroundControl (без GetParent от Border —
        // sibling-walk на VTC/частичном дереве давал SEH 0xC0000005).
        return TryAttachNamedRect(slot, L"BackgroundFill", slot.backgroundFill, &slot.originalFill);
    }

    bool TrayHoverFlyoutPanelRegistry::TryAttachStroke(PanelSlot& slot) noexcept
    {
        return TryAttachNamedRect(slot, L"BackgroundStroke", slot.backgroundStroke, nullptr);
    }

    void TrayHoverFlyoutPanelRegistry::TryAttachChromeGrid(PanelSlot& slot) noexcept
    {
        if (slot.hoverFlyoutGrid || !slot.backgroundBorder)
            return;
        try
        {
            // Windhawk: Grid#HoverFlyoutGrid > Border#HoverFlyoutBackground.
            const auto parent = wux::Media::VisualTreeHelper::GetParent(slot.backgroundBorder)
                                    .try_as<wux::Controls::Grid>();
            if (!parent)
                return;
            slot.hoverFlyoutGrid = parent;
            // Write-only: get_CornerRadius на HoverFlyout → AV 0xC0000005.
            if (!slot.gridCornerCaptured)
            {
                slot.originalGridCornerRadius = {};
                slot.gridCornerCaptured = true;
            }
        }
        catch (...)
        {
        }
    }

    void TrayHoverFlyoutPanelRegistry::TryAttachSystemInnerGrid(PanelSlot& slot) noexcept
    {
        if (slot.systemInnerGrid || !slot.root)
            return;
        try
        {
            // HoverFlyoutBackgroundControl > Grid > Rectangle#BackgroundFill|Stroke.
            const int n = wux::Media::VisualTreeHelper::GetChildrenCount(slot.root);
            for (int i = 0; i < n; ++i)
            {
                const auto child = wux::Media::VisualTreeHelper::GetChild(slot.root, i);
                if (const auto grid = child.try_as<wux::Controls::Grid>())
                {
                    slot.systemInnerGrid = grid;
                    if (!slot.systemInnerCornerCaptured)
                    {
                        slot.originalSystemInnerCornerRadius = {};
                        slot.systemInnerCornerCaptured = true;
                    }
                    break;
                }
            }
        }
        catch (...)
        {
        }
    }

    void TrayHoverFlyoutPanelRegistry::TryAttachHoverContent(PanelSlot& slot) noexcept
    {
        if (slot.hoverContent)
            return;
        // Windhawk: Grid#HoverFlyoutGrid > ContentPresenter#HoverFlyoutContent.
        wux::FrameworkElement searchRoot = slot.hoverFlyoutGrid
            ? slot.hoverFlyoutGrid.as<wux::FrameworkElement>()
            : slot.root;
        if (!searchRoot)
            return;
        try
        {
            const int n = wux::Media::VisualTreeHelper::GetChildrenCount(searchRoot);
            for (int i = 0; i < n; ++i)
            {
                const auto child = wux::Media::VisualTreeHelper::GetChild(searchRoot, i);
                const auto fe = child.try_as<wux::FrameworkElement>();
                if (!fe)
                    continue;
                try
                {
                    if (fe.Name() != L"HoverFlyoutContent")
                        continue;
                }
                catch (...)
                {
                    continue;
                }
                if (const auto cp = fe.try_as<wux::Controls::ContentPresenter>())
                {
                    slot.hoverContent = cp;
                    if (!slot.contentCornerCaptured)
                    {
                        // Write-only: getters CornerRadius/Border* на HoverFlyout небезопасны.
                        slot.originalContentCornerRadius = {};
                        slot.originalContentBorderThickness = {};
                        slot.originalContentBorderBrush = nullptr;
                        slot.contentCornerCaptured = true;
                    }
                    return;
                }
            }
        }
        catch (...)
        {
        }
    }

    bool TrayHoverFlyoutPanelRegistry::ResolveVisuals(PanelSlot& slot) noexcept
    {
        // Без getters Background/BorderBrush/CornerRadius (QI crash).
        // Backdrop Windhawk — на Fill; Border достаточно для chrome/dispatcher.
        if (!slot.backgroundBorder && !slot.backgroundFill && !slot.root)
            return false;
        (void)TryAttachFill(slot);
        (void)TryAttachStroke(slot);
        TryAttachChromeGrid(slot);
        TryAttachSystemInnerGrid(slot);
        TryAttachHoverContent(slot);
        if (!slot.backgroundBorder && !slot.backgroundFill)
            return false;
        slot.visualsResolved = true;
        return true;
    }

    void TrayHoverFlyoutPanelRegistry::CaptureChrome(PanelSlot& slot) noexcept
    {
        // Write-only: не читаем свойства Border (см. ResolveVisuals).
        if (!slot.backgroundBorder || slot.chromeCaptured)
            return;
        slot.originalBorderBackground = nullptr;
        slot.originalBorderBrush = nullptr;
        slot.originalBorderThickness = wux::Thickness{ 1.0, 1.0, 1.0, 1.0 };
        slot.originalCornerRadius = {};
        slot.originalShadow = nullptr;
        slot.chromeCaptured = true;
    }

    void TrayHoverFlyoutPanelRegistry::ScheduleDeferredApply(std::shared_ptr<PanelSlot> slot) noexcept
    {
        if (!slot || (!slot->backgroundBorder && !slot->backgroundFill && !slot->root))
            return;
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;
        // Не ставить в очередь, если уже ок (иначе флуд deferred сбрасывал appliedOk).
        if (slot->appliedOk && !slot->needsRetry)
            return;

        try
        {
            const auto dispatcher = slot->backgroundBorder
                ? slot->backgroundBorder.Dispatcher()
                : (slot->backgroundFill
                       ? slot->backgroundFill.Dispatcher()
                       : (slot->root ? slot->root.Dispatcher() : nullptr));
            if (!dispatcher)
                return;

            std::weak_ptr<PanelSlot> weakSlot = slot;
            std::weak_ptr<RegistryState> weakState = m_state;
            TrayHoverFlyoutPanelRegistry* self = this;
            m_state->pendingAsync.fetch_add(1, std::memory_order_acq_rel);
            (void)dispatcher.RunAsync(
                winrt::Windows::UI::Core::CoreDispatcherPriority::Low,
                winrt::Windows::UI::Core::DispatchedHandler(
                    [weakSlot, weakState, self]()
                    {
                        auto state = weakState.lock();
                        auto s = weakSlot.lock();
                        if (state)
                        {
                            state->pendingAsync.fetch_sub(1, std::memory_order_acq_rel);
                            if (state->shutdown.load(std::memory_order_acquire))
                                return;
                        }
                        else
                        {
                            return;
                        }
                        if (!s || !self)
                            return;
                        if (!s->backgroundBorder && !s->backgroundFill && !s->root)
                            return;
                        if (s->appliedOk && !s->needsRetry)
                            return;
                        if (!s->backgroundFill && !s->backgroundBorder && !s->root)
                        {
                            s->needsRetry = true;
                            return;
                        }
                        self->ApplyPanelVisualsSync(*s);
                    }));
        }
        catch (...)
        {
            m_state->pendingAsync.fetch_sub(1, std::memory_order_acq_rel);
        }
    }

    void TrayHoverFlyoutPanelRegistry::RestoreChrome(PanelSlot& slot) noexcept
    {
        if (slot.hoverGridCornerPatched && slot.hoverFlyoutGrid && slot.gridCornerCaptured)
        {
            try
            {
                slot.hoverFlyoutGrid.CornerRadius(slot.originalGridCornerRadius);
            }
            catch (...)
            {
            }
            slot.hoverGridCornerPatched = false;
        }

        if (slot.hoverGridBgCleared && slot.hoverFlyoutGrid)
        {
            try
            {
                if (slot.originalHoverGridBackground)
                    slot.hoverFlyoutGrid.Background(slot.originalHoverGridBackground);
                else
                    slot.hoverFlyoutGrid.ClearValue(wux::Controls::Panel::BackgroundProperty());
            }
            catch (...)
            {
            }
            slot.hoverGridBgCleared = false;
        }

        if ((slot.rootCornerPatched || slot.rootChromeCleared) && slot.rootCornerCaptured
            && slot.root)
        {
            try
            {
                if (const auto ctrl = slot.root.try_as<wux::Controls::Control>())
                {
                    if (slot.rootCornerPatched)
                        ctrl.CornerRadius(slot.originalRootCornerRadius);
                    if (slot.rootChromeCleared)
                    {
                        ctrl.BorderThickness(slot.originalRootBorderThickness);
                        if (slot.originalRootBorderBrush)
                            ctrl.BorderBrush(slot.originalRootBorderBrush);
                        else
                            ctrl.ClearValue(wux::Controls::Control::BorderBrushProperty());
                        if (slot.originalRootBackground)
                            ctrl.Background(slot.originalRootBackground);
                        else
                            ctrl.ClearValue(wux::Controls::Control::BackgroundProperty());
                    }
                }
            }
            catch (...)
            {
            }
            slot.rootCornerPatched = false;
            slot.rootChromeCleared = false;
        }

        if (slot.systemInnerCornerPatched && slot.systemInnerCornerCaptured
            && slot.systemInnerGrid)
        {
            try
            {
                slot.systemInnerGrid.CornerRadius(slot.originalSystemInnerCornerRadius);
            }
            catch (...)
            {
            }
            slot.systemInnerCornerPatched = false;
        }

        if (slot.contentChromePatched && slot.contentCornerCaptured && slot.hoverContent)
        {
            try
            {
                slot.hoverContent.CornerRadius(slot.originalContentCornerRadius);
                slot.hoverContent.BorderThickness(slot.originalContentBorderThickness);
                if (slot.originalContentBorderBrush)
                    slot.hoverContent.BorderBrush(slot.originalContentBorderBrush);
                else
                    slot.hoverContent.ClearValue(
                        wux::Controls::ContentPresenter::BorderBrushProperty());
            }
            catch (...)
            {
            }
            slot.contentChromePatched = false;
        }

        if (slot.fillStrokePatched && slot.backgroundFill)
        {
            try
            {
                slot.backgroundFill.StrokeThickness(0.0);
                slot.backgroundFill.ClearValue(wux::Shapes::Shape::StrokeProperty());
            }
            catch (...)
            {
            }
            slot.fillStrokePatched = false;
        }

        if (slot.fillShadowPatched && slot.backgroundFill)
        {
            try
            {
                slot.backgroundFill.ClearValue(wux::UIElement::ShadowProperty());
                slot.backgroundFill.Translation(
                    winrt::Windows::Foundation::Numerics::float3{ 0.0f, 0.0f, 0.0f });
            }
            catch (...)
            {
            }
            slot.fillShadowPatched = false;
        }

        if (slot.borderShadowPatched && slot.backgroundBorder)
        {
            try
            {
                slot.backgroundBorder.ClearValue(wux::UIElement::ShadowProperty());
                slot.backgroundBorder.Translation(
                    winrt::Windows::Foundation::Numerics::float3{ 0.0f, 0.0f, 0.0f });
            }
            catch (...)
            {
            }
            slot.borderShadowPatched = false;
        }

        if (slot.strokeShadowPatched && slot.backgroundStroke)
        {
            try
            {
                slot.backgroundStroke.ClearValue(wux::UIElement::ShadowProperty());
                slot.backgroundStroke.Translation(
                    winrt::Windows::Foundation::Numerics::float3{ 0.0f, 0.0f, 0.0f });
            }
            catch (...)
            {
            }
            slot.strokeShadowPatched = false;
        }

        if (slot.shadowPlateInserted && slot.shadowPlate && slot.systemInnerGrid)
        {
            try
            {
                slot.shadowPlate.ClearValue(wux::UIElement::ShadowProperty());
                slot.shadowPlate.Translation(
                    winrt::Windows::Foundation::Numerics::float3{ 0.0f, 0.0f, 0.0f });
                uint32_t idx = 0;
                if (slot.systemInnerGrid.Children().IndexOf(slot.shadowPlate, idx))
                    slot.systemInnerGrid.Children().RemoveAt(idx);
            }
            catch (...)
            {
            }
            slot.shadowPlate = nullptr;
            slot.shadowPlateInserted = false;
        }

        if (slot.backgroundBorder && slot.chromeCaptured && slot.chromePatched)
        {
            try
            {
                try
                {
                    slot.backgroundBorder.ClearValue(
                        wux::Controls::Border::CornerRadiusProperty());
                }
                catch (...)
                {
                }
                try
                {
                    slot.backgroundBorder.ClearValue(
                        wux::Controls::Border::BorderBrushProperty());
                    slot.backgroundBorder.ClearValue(
                        wux::Controls::Border::BorderThicknessProperty());
                }
                catch (...)
                {
                }
                // Shadow не трогали при apply.
            }
            catch (...)
            {
            }
            slot.chromePatched = false;
        }

        if (slot.backgroundStroke && slot.strokePatched && slot.strokeVisibilityCaptured)
        {
            try
            {
                slot.backgroundStroke.Visibility(slot.originalStrokeVisibility);
                try
                {
                    slot.backgroundStroke.Opacity(1.0);
                }
                catch (...)
                {
                }
            }
            catch (...)
            {
            }
            slot.strokePatched = false;
        }
    }

    void TrayHoverFlyoutPanelRegistry::RestorePanelVisuals(
        PanelSlot& slot,
        bool restoreBackground,
        bool& hadOrig) noexcept
    {
        hadOrig = false;
        RestoreChrome(slot);

        if (restoreBackground && slot.backgroundBorder)
        {
            bool borderHad = false;
            RestoreBorderBackground(
                slot.backgroundBorder, slot.originalBorderBackground, borderHad);
        }

        if (restoreBackground && slot.backgroundFill)
        {
            if (slot.fillRadiiPatched)
            {
                (void)SafePutRectangleRadiiXY(
                    static_cast<IUnknown*>(winrt::get_abi(slot.backgroundFill)),
                    slot.originalFillRadiusX,
                    slot.originalFillRadiusY);
                slot.fillRadiiPatched = false;
            }
            if (slot.fillCollapsed && slot.fillVisibilityCaptured)
            {
                try
                {
                    slot.backgroundFill.Visibility(slot.originalFillVisibility);
                }
                catch (...)
                {
                }
                slot.fillCollapsed = false;
            }
            RestoreRectangleFill(slot.backgroundFill, slot.originalFill, hadOrig);
            slot.fillPatched = false;
        }
        else
        {
            hadOrig = static_cast<bool>(slot.originalFill)
                || static_cast<bool>(slot.originalBorderBackground);
        }
    }

    HRESULT TrayHoverFlyoutPanelRegistry::RestoreSlotOnUiThread(
        std::shared_ptr<PanelSlot> slot,
        bool restoreBackground,
        bool& hadOrig,
        bool& usedDispatcher) noexcept
    {
        usedDispatcher = false;
        hadOrig = false;
        if (!slot)
            return S_FALSE;

        try
        {
            if (!slot->root && !slot->backgroundBorder && !slot->backgroundFill)
                return S_FALSE;

            const auto dispatcher = slot->root
                ? slot->root.Dispatcher()
                : (slot->backgroundBorder
                       ? slot->backgroundBorder.Dispatcher()
                       : (slot->backgroundFill ? slot->backgroundFill.Dispatcher() : nullptr));

            if (dispatcher && !dispatcher.HasThreadAccess())
            {
                usedDispatcher = true;
                const HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                if (!done)
                    return HRESULT_FROM_WIN32(GetLastError());

                struct Job
                {
                    std::shared_ptr<PanelSlot> slot;
                    std::shared_ptr<RegistryState> state;
                    HANDLE done{};
                    HRESULT hr{ S_OK };
                    bool hadOrig{ false };
                    bool restoreBackground{ true };
                    bool ownsEvent{ true };

                    ~Job() noexcept
                    {
                        if (ownsEvent && done)
                            CloseHandle(done);
                    }
                };

                auto job = std::make_shared<Job>();
                job->slot = std::move(slot);
                job->state = m_state;
                job->done = done;
                job->restoreBackground = restoreBackground;

                m_state->pendingAsync.fetch_add(1, std::memory_order_acq_rel);
                try
                {
                    (void)dispatcher.RunAsync(
                        winrt::Windows::UI::Core::CoreDispatcherPriority::High,
                        winrt::Windows::UI::Core::DispatchedHandler(
                            [job]()
                            {
                                try
                                {
                                    if (job->slot)
                                    {
                                        bool had = false;
                                        // Inline restore (без this).
                                        auto& s = *job->slot;
                                        if (s.hoverGridCornerPatched && s.hoverFlyoutGrid
                                            && s.gridCornerCaptured)
                                        {
                                            try
                                            {
                                                s.hoverFlyoutGrid.CornerRadius(
                                                    s.originalGridCornerRadius);
                                            }
                                            catch (...)
                                            {
                                            }
                                            s.hoverGridCornerPatched = false;
                                        }
                                        if (s.hoverGridBgCleared && s.hoverFlyoutGrid)
                                        {
                                            try
                                            {
                                                if (s.originalHoverGridBackground)
                                                    s.hoverFlyoutGrid.Background(
                                                        s.originalHoverGridBackground);
                                                else
                                                    s.hoverFlyoutGrid.ClearValue(
                                                        wux::Controls::Panel::BackgroundProperty());
                                            }
                                            catch (...)
                                            {
                                            }
                                            s.hoverGridBgCleared = false;
                                        }
                                        if ((s.rootCornerPatched || s.rootChromeCleared)
                                            && s.rootCornerCaptured && s.root)
                                        {
                                            try
                                            {
                                                if (const auto ctrl =
                                                        s.root.try_as<wux::Controls::Control>())
                                                {
                                                    if (s.rootCornerPatched)
                                                        ctrl.CornerRadius(
                                                            s.originalRootCornerRadius);
                                                    if (s.rootChromeCleared)
                                                    {
                                                        ctrl.BorderThickness(
                                                            s.originalRootBorderThickness);
                                                        if (s.originalRootBorderBrush)
                                                            ctrl.BorderBrush(
                                                                s.originalRootBorderBrush);
                                                        else
                                                            ctrl.ClearValue(
                                                                wux::Controls::Control::
                                                                    BorderBrushProperty());
                                                        if (s.originalRootBackground)
                                                            ctrl.Background(
                                                                s.originalRootBackground);
                                                        else
                                                            ctrl.ClearValue(
                                                                wux::Controls::Control::
                                                                    BackgroundProperty());
                                                    }
                                                }
                                            }
                                            catch (...)
                                            {
                                            }
                                            s.rootCornerPatched = false;
                                            s.rootChromeCleared = false;
                                        }
                                        if (s.contentChromePatched && s.contentCornerCaptured
                                            && s.hoverContent)
                                        {
                                            try
                                            {
                                                s.hoverContent.CornerRadius(
                                                    s.originalContentCornerRadius);
                                                s.hoverContent.BorderThickness(
                                                    s.originalContentBorderThickness);
                                                if (s.originalContentBorderBrush)
                                                    s.hoverContent.BorderBrush(
                                                        s.originalContentBorderBrush);
                                                else
                                                    s.hoverContent.ClearValue(
                                                        wux::Controls::ContentPresenter::
                                                            BorderBrushProperty());
                                            }
                                            catch (...)
                                            {
                                            }
                                            s.contentChromePatched = false;
                                        }
                                        if (s.fillStrokePatched && s.backgroundFill)
                                        {
                                            try
                                            {
                                                s.backgroundFill.StrokeThickness(0.0);
                                                s.backgroundFill.ClearValue(
                                                    wux::Shapes::Shape::StrokeProperty());
                                            }
                                            catch (...)
                                            {
                                            }
                                            s.fillStrokePatched = false;
                                        }
                                        if (s.backgroundBorder && s.chromeCaptured
                                            && s.chromePatched)
                                        {
                                            try
                                            {
                                                s.backgroundBorder.BorderBrush(
                                                    s.originalBorderBrush);
                                                s.backgroundBorder.BorderThickness(
                                                    s.originalBorderThickness);
                                                if (s.originalShadow)
                                                {
                                                    try
                                                    {
                                                        s.backgroundBorder.Shadow(
                                                            s.originalShadow);
                                                    }
                                                    catch (...)
                                                    {
                                                    }
                                                }
                                            }
                                            catch (...)
                                            {
                                            }
                                            s.chromePatched = false;
                                        }
                                        if (s.backgroundStroke && s.strokePatched
                                            && s.strokeVisibilityCaptured)
                                        {
                                            try
                                            {
                                                s.backgroundStroke.Visibility(
                                                    s.originalStrokeVisibility);
                                                try
                                                {
                                                    s.backgroundStroke.Opacity(1.0);
                                                }
                                                catch (...)
                                                {
                                                }
                                            }
                                            catch (...)
                                            {
                                            }
                                            s.strokePatched = false;
                                        }
                                        if (job->restoreBackground && s.backgroundBorder)
                                        {
                                            bool borderHad = false;
                                            RestoreBorderBackground(
                                                s.backgroundBorder,
                                                s.originalBorderBackground,
                                                borderHad);
                                        }
                                        if (job->restoreBackground && s.backgroundFill)
                                        {
                                            if (s.fillRadiiPatched)
                                            {
                                                (void)SafePutRectangleRadiiXY(
                                                    static_cast<IUnknown*>(
                                                        winrt::get_abi(s.backgroundFill)),
                                                    s.originalFillRadiusX,
                                                    s.originalFillRadiusY);
                                                s.fillRadiiPatched = false;
                                            }
                                            if (s.fillCollapsed && s.fillVisibilityCaptured)
                                            {
                                                try
                                                {
                                                    s.backgroundFill.Visibility(
                                                        s.originalFillVisibility);
                                                }
                                                catch (...)
                                                {
                                                }
                                                s.fillCollapsed = false;
                                            }
                                            RestoreRectangleFill(
                                                s.backgroundFill, s.originalFill, had);
                                            s.fillPatched = false;
                                        }
                                        else
                                        {
                                            had = static_cast<bool>(s.originalFill)
                                                || static_cast<bool>(
                                                    s.originalBorderBackground);
                                        }
                                        job->hadOrig = had;
                                        s.ClearWinrtRefs();
                                    }
                                }
                                catch (const winrt::hresult_error& ex)
                                {
                                    job->hr = ex.code();
                                }
                                catch (...)
                                {
                                    job->hr = E_FAIL;
                                }
                                if (job->state)
                                {
                                    job->state->pendingAsync.fetch_sub(
                                        1, std::memory_order_acq_rel);
                                }
                                if (job->done)
                                    SetEvent(job->done);
                            }));
                }
                catch (const winrt::hresult_error& ex)
                {
                    m_state->pendingAsync.fetch_sub(1, std::memory_order_acq_rel);
                    return ex.code();
                }
                catch (...)
                {
                    m_state->pendingAsync.fetch_sub(1, std::memory_order_acq_rel);
                    return E_FAIL;
                }

                const DWORD wr = WaitForSingleObject(done, kRestoreWaitMs);
                if (wr == WAIT_OBJECT_0)
                {
                    hadOrig = job->hadOrig;
                    const HRESULT hr = job->hr;
                    job->ownsEvent = false;
                    CloseHandle(done);
                    return hr;
                }

                DebugLog(
                    DBG_WARNING,
                    L"[TaskbarCompat][HoverFlyout] Restore wait timeout (%u ms)",
                    kRestoreWaitMs);
                return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            }

            RestorePanelVisuals(*slot, restoreBackground, hadOrig);
            slot->ClearWinrtRefs();
            return S_OK;
        }
        catch (const winrt::hresult_error& ex)
        {
            return ex.code();
        }
        catch (...)
        {
            return E_FAIL;
        }
    }

    void TrayHoverFlyoutPanelRegistry::ApplyPanelVisualsSync(PanelSlot& slot) noexcept
    {
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;
        if (!slot.root && !slot.backgroundBorder && !slot.backgroundFill)
            return;

        if (slot.appliedOk && !slot.needsRetry)
            return;

        const Registry::Settings settings = [this]() noexcept
        {
            std::lock_guard lock(m_mx);
            return m_settings;
        }();


        if (!ResolveVisuals(slot))
        {
            slot.needsRetry = true;
            return;
        }


        // Без ActualHeight: достаточно наличия Fill/Border/root (донор не ждёт layout).
        if (!slot.backgroundFill && !slot.backgroundBorder && !slot.root)
        {
            slot.needsRetry = true;
            slot.appliedOk = false;
            return;
        }

        if (slot.backgroundBorder)
            CaptureChrome(slot);

        // Stop ≡ XAML→DWM: тот же RestorePanelVisuals (не ClearValue-stub).
        if (!settings.paintPathXaml)
        {
            bool hadOrig = false;
            RestorePanelVisuals(slot, true, hadOrig);
            slot.needsRetry = false;
            slot.appliedOk = true;
            return;
        }

        // Disable/System — без подмены Fill; tint/углы всё равно применяем (паритет донора).
        const bool patchBackdrop =
            settings.xamlEffectType != 0 && settings.xamlEffectType != 5;
        const bool wantCorners =
            settings.cornersPresent && settings.cornerType != 0;
        const bool wantTintBorder =
            settings.patchBorderStroke && settings.borderColorArgb != 0;
        const bool patchChrome =
            wantCorners || wantTintBorder || settings.patchBorderStroke || patchBackdrop;

        if (!patchBackdrop && !patchChrome)
        {
            // System/Disable без chrome: всё равно снять кастомный Fill.
            if (slot.fillPatched && slot.backgroundFill)
            {
                bool hadOrig = false;
                RestoreRectangleFill(slot.backgroundFill, slot.originalFill, hadOrig);
                slot.fillPatched = false;
            }
            slot.needsRetry = false;
            slot.appliedOk = true;
            return;
        }

        // Blur: XamlBlurBrush (паритет WindhawkBlur). Fail → Acrylic fallback.
        const DWORD brushEffectRaw = settings.xamlEffectType;
        DWORD brushEffect = brushEffectRaw;

        try
        {
            TryAttachChromeGrid(slot);
            TryAttachSystemInnerGrid(slot);
            (void)TryAttachStroke(slot);
            TryAttachHoverContent(slot);

            // --- Backdrop (Fill) ---
            if (patchBackdrop)
            {
                wux::UIElement brushHost{ nullptr };
                if (slot.backgroundFill)
                    brushHost = slot.backgroundFill;
                else if (slot.backgroundBorder)
                    brushHost = slot.backgroundBorder;

                wux::Media::Brush brush = CreateBackdropBrush(
                    brushHost, brushEffect, settings.tintArgb);
                if (!brush && brushEffectRaw == 3)
                {
                    brushEffect = 4;
                    brush = CreateBackdropBrush(brushHost, brushEffect, settings.tintArgb);
                }
                if (!brush && brushEffect == 4)
                {
                    wux::Media::AcrylicBrush acrylic;
                    acrylic.BackgroundSource(
                        wux::Media::AcrylicBackgroundSource::Backdrop);
                    acrylic.TintColor(ColorFromArgb(settings.tintArgb));
                    brush = acrylic;
                }
                if (!brush)
                {
                    // Chrome (tint/CR) можно применить без Fill — не блокируем весь apply.
                    if (!patchChrome)
                    {
                        slot.needsRetry = true;
                        return;
                    }
                }
                else if (slot.backgroundFill)
                {
                    try
                    {
                        if (slot.fillCollapsed && slot.fillVisibilityCaptured)
                        {
                            slot.backgroundFill.Visibility(slot.originalFillVisibility);
                            slot.fillCollapsed = false;
                        }
                        if (!slot.originalFill)
                        {
                            try
                            {
                                slot.originalFill = slot.backgroundFill.Fill();
                            }
                            catch (...)
                            {
                            }
                        }
                        slot.backgroundFill.Fill(brush);
                        slot.fillPatched = true;

                        // Донор: delayed SetValue BackgroundFill (OnApplyTemplate перетирает).
                        try
                        {
                            const auto fillFe = slot.backgroundFill;
                            const auto brushCopy = brush;
                            const double radiiR =
                                wantCorners ? HoverCornerRadiusDip(settings.cornerType) : 0.0;
                            (void)fillFe.Dispatcher().TryRunAsync(
                                winrt::Windows::UI::Core::CoreDispatcherPriority::High,
                                winrt::Windows::UI::Core::DispatchedHandler(
                                    [fillFe, brushCopy, radiiR]()
                                    {
                                        try
                                        {
                                            if (!fillFe)
                                                return;
                                            fillFe.Fill(brushCopy);
                                            if (radiiR > 0.0)
                                            {
                                                (void)SafePutRectangleRadii(
                                                    static_cast<IUnknown*>(
                                                        winrt::get_abi(fillFe)),
                                                    radiiR);
                                            }
                                        }
                                        catch (...)
                                        {
                                        }
                                    }));
                        }
                        catch (...)
                        {
                        }
                    }
                    catch (...)
                    {
                    }
                }
            }
            else if (slot.fillPatched && slot.backgroundFill)
            {
                // System/Disable: вернуть system Fill, chrome ниже остаётся.
                bool hadOrig = false;
                if (slot.fillRadiiPatched)
                {
                    (void)SafePutRectangleRadiiXY(
                        static_cast<IUnknown*>(winrt::get_abi(slot.backgroundFill)),
                        slot.originalFillRadiusX,
                        slot.originalFillRadiusY);
                    slot.fillRadiiPatched = false;
                }
                RestoreRectangleFill(slot.backgroundFill, slot.originalFill, hadOrig);
                slot.fillPatched = false;
            }

            // RadiusX на Fill — только wantCorners (геометрия плиты). Без Control/Inner CR
            // (клип срезал бордюр). Без углов — вернуть system radii.
            if (!wantCorners && slot.backgroundFill && slot.fillRadiiPatched)
            {
                (void)SafePutRectangleRadiiXY(
                    static_cast<IUnknown*>(winrt::get_abi(slot.backgroundFill)),
                    slot.originalFillRadiusX,
                    slot.originalFillRadiusY);
                slot.fillRadiiPatched = false;
            }

            if (!patchChrome)
            {
                slot.appliedOk = true;
                slot.needsRetry = false;
                m_unresolved.store(0, std::memory_order_release);
                return;
            }

            // Без тинта — гасим Fill.Stroke. С тинтом — Stroke+RadiusX ниже (донор).
            if (slot.backgroundFill && !wantTintBorder)
            {
                try
                {
                    slot.backgroundFill.StrokeThickness(0.0);
                    slot.backgroundFill.ClearValue(wux::Shapes::Shape::StrokeProperty());
                    slot.fillStrokePatched = true;
                }
                catch (...)
                {
                }
            }

            // Control / systemInner Border*: только при wantCorners (свой периметр).
            // CornerType=0 — не трогаем system chrome grid (тень/контур остаются).
            if (wantCorners && slot.root)
            {
                try
                {
                    if (const auto ctrl = slot.root.try_as<wux::Controls::Control>())
                    {
                        if (!slot.rootCornerCaptured)
                        {
                            // Write-only: get_CornerRadius/Border* → AV на HoverFlyout.
                            slot.originalRootCornerRadius = {};
                            slot.originalRootBorderThickness = {};
                            slot.originalRootBorderBrush = nullptr;
                            slot.originalRootBackground = nullptr;
                            slot.rootCornerCaptured = true;
                        }
                        ctrl.BorderThickness(wux::Thickness{ 0.0, 0.0, 0.0, 0.0 });
                        ctrl.ClearValue(wux::Controls::Control::BorderBrushProperty());
                        ctrl.ClearValue(wux::Controls::Control::BackgroundProperty());
                        slot.rootChromeCleared = true;
                        slot.chromePatched = true;
                    }
                }
                catch (...)
                {
                }
            }

            // HoverFlyoutGrid: фон не красим; Shadow не сбрасываем.
            if (slot.hoverFlyoutGrid && slot.hoverGridBgCleared)
            {
                try
                {
                    if (slot.originalHoverGridBackground)
                        slot.hoverFlyoutGrid.Background(slot.originalHoverGridBackground);
                    else
                        slot.hoverFlyoutGrid.ClearValue(
                            wux::Controls::Panel::BackgroundProperty());
                    slot.hoverGridBgCleared = false;
                }
                catch (...)
                {
                }
            }

            // systemInnerGrid Border*: гасим только при кастомных углах.
            if (wantCorners && slot.systemInnerGrid)
            {
                try
                {
                    if (!slot.systemInnerCornerCaptured)
                    {
                        slot.originalSystemInnerCornerRadius = {};
                        slot.systemInnerCornerCaptured = true;
                    }
                    try
                    {
                        slot.systemInnerGrid.BorderThickness(
                            wux::Thickness{ 0.0, 0.0, 0.0, 0.0 });
                        slot.systemInnerGrid.ClearValue(
                            wux::Controls::Grid::BorderBrushProperty());
                    }
                    catch (...)
                    {
                    }
                }
                catch (...)
                {
                }
            }

            // BackgroundStroke = системная верхняя кромка (не full plate).
            // post-fix16: Fill+ThemeShadow на нём → чёрная черта сверху. Всегда Collapsed.
            if (slot.backgroundStroke)
            {
                try
                {
                    if (!slot.strokeVisibilityCaptured)
                    {
                        slot.originalStrokeVisibility = slot.backgroundStroke.Visibility();
                        slot.strokeVisibilityCaptured = true;
                    }
                    slot.backgroundStroke.Visibility(wux::Visibility::Collapsed);
                    try
                    {
                        slot.backgroundStroke.Fill(nullptr);
                    }
                    catch (...)
                    {
                    }
                    try
                    {
                        slot.backgroundStroke.StrokeThickness(0.0);
                        slot.backgroundStroke.ClearValue(
                            wux::Shapes::Shape::StrokeProperty());
                        slot.backgroundStroke.ClearValue(wux::UIElement::ShadowProperty());
                        slot.backgroundStroke.Translation(
                            winrt::Windows::Foundation::Numerics::float3{ 0.0f, 0.0f, 0.0f });
                    }
                    catch (...)
                    {
                    }
                    try
                    {
                        slot.backgroundStroke.Opacity(0.0);
                    }
                    catch (...)
                    {
                    }
                    slot.strokeShadowPatched = false;
                    slot.strokePatched = true;
                }
                catch (...)
                {
                }
            }

            // Политика HoverFlyout (донор + runtime):
            //   CornerType≠0 → Shadow:= + свой бордюр (Fill.Stroke + RadiusX).
            //   CornerType=0 → тень не гасим; тинт на Border* (system CR);
            //     если Border* недоступен → Fill.Stroke без смены RadiusX.
            // Plate/ThemeShadow на Fill под кастомный R — запрещены (post-fix17/18).
            bool borderTintOk = false;
            double appliedR = 0.0;

            if (wantCorners)
                appliedR = HoverCornerRadiusDip(settings.cornerType);

            // Откат CR на Control/Inner/Grid — клип (всегда, если патчили ранее).
            if (slot.rootCornerPatched && slot.root)
            {
                try
                {
                    if (const auto ctrl = slot.root.try_as<wux::Controls::Control>())
                        ctrl.ClearValue(wux::Controls::Control::CornerRadiusProperty());
                }
                catch (...)
                {
                }
                slot.rootCornerPatched = false;
            }
            if (slot.systemInnerCornerPatched && slot.systemInnerGrid)
            {
                try
                {
                    slot.systemInnerGrid.ClearValue(
                        wux::Controls::Grid::CornerRadiusProperty());
                }
                catch (...)
                {
                }
                slot.systemInnerCornerPatched = false;
            }
            if (slot.hoverGridCornerPatched && slot.hoverFlyoutGrid)
            {
                try
                {
                    slot.hoverFlyoutGrid.ClearValue(
                        wux::Controls::Grid::CornerRadiusProperty());
                }
                catch (...)
                {
                }
                slot.hoverGridCornerPatched = false;
            }

            // Убрать чёрный plate от post-fix17, если остался в дереве.
            if (slot.shadowPlateInserted && slot.shadowPlate && slot.systemInnerGrid)
            {
                try
                {
                    slot.shadowPlate.ClearValue(wux::UIElement::ShadowProperty());
                    slot.shadowPlate.Translation(
                        winrt::Windows::Foundation::Numerics::float3{ 0.0f, 0.0f, 0.0f });
                    uint32_t idx = 0;
                    if (slot.systemInnerGrid.Children().IndexOf(slot.shadowPlate, idx))
                        slot.systemInnerGrid.Children().RemoveAt(idx);
                }
                catch (...)
                {
                }
                slot.shadowPlate = nullptr;
                slot.shadowPlateInserted = false;
            }

            if (wantCorners)
            {
                // --- Кастомные углы: свой периметр, Shadow:= ---
                if (slot.backgroundFill)
                {
                    if (!slot.fillRadiiPatched)
                    {
                        double ox = 0.0;
                        double oy = 0.0;
                        if (SUCCEEDED(SafeGetRectangleRadiiXY(
                                static_cast<IUnknown*>(winrt::get_abi(slot.backgroundFill)),
                                ox,
                                oy)))
                        {
                            slot.originalFillRadiusX = ox;
                            slot.originalFillRadiusY = oy;
                        }
                    }
                    const HRESULT putHr = SafePutRectangleRadii(
                        static_cast<IUnknown*>(winrt::get_abi(slot.backgroundFill)), appliedR);
                    if (SUCCEEDED(putHr))
                        slot.fillRadiiPatched = true;
                }

                if (wantTintBorder && slot.backgroundFill)
                {
                    try
                    {
                        wux::Media::SolidColorBrush strokeBrush;
                        strokeBrush.Color(ColorFromArgb(settings.borderColorArgb));
                        slot.backgroundFill.Stroke(strokeBrush);
                        slot.backgroundFill.StrokeThickness(1.0);
                        slot.fillStrokePatched = true;
                    }
                    catch (...)
                    {
                    }
                }

                if (slot.backgroundBorder)
                {
                    try
                    {
                        slot.backgroundBorder.ClearValue(
                            wux::Controls::Border::CornerRadiusProperty());
                    }
                    catch (...)
                    {
                    }
                    try
                    {
                        wux::Media::SolidColorBrush transparent;
                        transparent.Color(winrt::Windows::UI::Color{ 0, 0, 0, 0 });
                        slot.backgroundBorder.Background(transparent);
                    }
                    catch (...)
                    {
                    }
                    try
                    {
                        slot.backgroundBorder.Shadow(nullptr);
                        slot.backgroundBorder.Translation(
                            winrt::Windows::Foundation::Numerics::float3{
                                0.0f, 0.0f, 0.0f });
                        slot.borderShadowPatched = true;
                    }
                    catch (...)
                    {
                        try
                        {
                            slot.backgroundBorder.ClearValue(
                                wux::UIElement::ShadowProperty());
                            slot.borderShadowPatched = true;
                        }
                        catch (...)
                        {
                        }
                    }
                    try
                    {
                        slot.backgroundBorder.ClearValue(
                            wux::Controls::Border::BorderBrushProperty());
                        slot.backgroundBorder.SetValue(
                            wux::Controls::Border::BorderThicknessProperty(),
                            winrt::box_value(wux::Thickness{ 0.0, 0.0, 0.0, 0.0 }));
                    }
                    catch (...)
                    {
                    }
                    slot.chromePatched = true;
                }

                if (slot.backgroundFill)
                {
                    try
                    {
                        slot.backgroundFill.Shadow(nullptr);
                        slot.backgroundFill.Translation(
                            winrt::Windows::Foundation::Numerics::float3{
                                0.0f, 0.0f, 0.0f });
                        slot.fillShadowPatched = true;
                    }
                    catch (...)
                    {
                        try
                        {
                            slot.backgroundFill.ClearValue(
                                wux::UIElement::ShadowProperty());
                            slot.backgroundFill.Translation(
                                winrt::Windows::Foundation::Numerics::float3{
                                    0.0f, 0.0f, 0.0f });
                            slot.fillShadowPatched = true;
                        }
                        catch (...)
                        {
                            slot.fillShadowPatched = false;
                        }
                    }
                }

                if (slot.hoverContent)
                {
                    try
                    {
                        if (!slot.contentCornerCaptured)
                        {
                            slot.originalContentCornerRadius = {};
                            slot.originalContentBorderThickness = {};
                            slot.originalContentBorderBrush = nullptr;
                            slot.contentCornerCaptured = true;
                        }
                        slot.hoverContent.ClearValue(
                            wux::Controls::ContentPresenter::CornerRadiusProperty());
                        slot.hoverContent.SetValue(
                            wux::Controls::ContentPresenter::BorderThicknessProperty(),
                            winrt::box_value(wux::Thickness{ 0.0, 0.0, 0.0, 0.0 }));
                        slot.hoverContent.ClearValue(
                            wux::Controls::ContentPresenter::BorderBrushProperty());
                        slot.contentChromePatched = true;
                    }
                    catch (...)
                    {
                    }
                }
            }
            else
            {
                // --- CornerType=0: system углы + system ThemeShadow ---
                // Вернуть Fill radii / снять наш Shadow:= если патчили ранее.
                if (slot.backgroundFill && slot.fillRadiiPatched)
                {
                    (void)SafePutRectangleRadiiXY(
                        static_cast<IUnknown*>(winrt::get_abi(slot.backgroundFill)),
                        slot.originalFillRadiusX,
                        slot.originalFillRadiusY);
                    slot.fillRadiiPatched = false;
                }
                if (slot.fillShadowPatched && slot.backgroundFill)
                {
                    try
                    {
                        slot.backgroundFill.ClearValue(wux::UIElement::ShadowProperty());
                        slot.backgroundFill.Translation(
                            winrt::Windows::Foundation::Numerics::float3{
                                0.0f, 0.0f, 0.0f });
                    }
                    catch (...)
                    {
                    }
                    slot.fillShadowPatched = false;
                }
                if (slot.borderShadowPatched && slot.backgroundBorder)
                {
                    try
                    {
                        slot.backgroundBorder.ClearValue(
                            wux::UIElement::ShadowProperty());
                        slot.backgroundBorder.Translation(
                            winrt::Windows::Foundation::Numerics::float3{
                                0.0f, 0.0f, 0.0f });
                    }
                    catch (...)
                    {
                    }
                    slot.borderShadowPatched = false;
                }

                if (wantTintBorder && slot.backgroundBorder)
                {
                    // Предпочтительно: тинт system Border* (Shadow не трогаем).
                    try
                    {
                        wux::Media::SolidColorBrush transparent;
                        transparent.Color(winrt::Windows::UI::Color{ 0, 0, 0, 0 });
                        slot.backgroundBorder.Background(transparent);
                    }
                    catch (...)
                    {
                    }
                    // CR не ставим и не ClearValue — оставляем system.
                    try
                    {
                        wux::Media::SolidColorBrush brush;
                        brush.Color(ColorFromArgb(settings.borderColorArgb));
                        slot.backgroundBorder.SetValue(
                            wux::Controls::Border::BorderBrushProperty(),
                            brush);
                        slot.backgroundBorder.SetValue(
                            wux::Controls::Border::BorderThicknessProperty(),
                            winrt::box_value(wux::Thickness{ 1.0, 1.0, 1.0, 1.0 }));
                        borderTintOk = true;
                        slot.chromePatched = true;
                    }
                    catch (...)
                    {
                        borderTintOk = false;
                    }

                    // Fallback: system Border* недоступен → свой Stroke, system RadiusX.
                    if (!borderTintOk && slot.backgroundFill)
                    {
                        try
                        {
                            slot.backgroundBorder.ClearValue(
                                wux::Controls::Border::BorderBrushProperty());
                            slot.backgroundBorder.SetValue(
                                wux::Controls::Border::BorderThicknessProperty(),
                                winrt::box_value(wux::Thickness{ 0.0, 0.0, 0.0, 0.0 }));
                        }
                        catch (...)
                        {
                        }
                        try
                        {
                            wux::Media::SolidColorBrush strokeBrush;
                            strokeBrush.Color(ColorFromArgb(settings.borderColorArgb));
                            slot.backgroundFill.Stroke(strokeBrush);
                            slot.backgroundFill.StrokeThickness(1.0);
                            slot.fillStrokePatched = true;
                            slot.chromePatched = true;
                        }
                        catch (...)
                        {
                        }
                    }
                    else if (borderTintOk && slot.backgroundFill && slot.fillStrokePatched)
                    {
                        try
                        {
                            slot.backgroundFill.StrokeThickness(0.0);
                            slot.backgroundFill.ClearValue(
                                wux::Shapes::Shape::StrokeProperty());
                        }
                        catch (...)
                        {
                        }
                        slot.fillStrokePatched = false;
                    }
                }
                else if (slot.backgroundBorder && slot.chromePatched)
                {
                    // Без тинта — вернуть Border* к ClearValue (system).
                    try
                    {
                        slot.backgroundBorder.ClearValue(
                            wux::Controls::Border::BorderBrushProperty());
                        slot.backgroundBorder.ClearValue(
                            wux::Controls::Border::BorderThicknessProperty());
                        slot.backgroundBorder.ClearValue(
                            wux::Controls::Border::CornerRadiusProperty());
                    }
                    catch (...)
                    {
                    }
                }

                // ContentPresenter: не клипать; при тинте через Border — без своего Border*.
                if (slot.hoverContent && wantTintBorder)
                {
                    try
                    {
                        if (!slot.contentCornerCaptured)
                        {
                            slot.originalContentCornerRadius = {};
                            slot.originalContentBorderThickness = {};
                            slot.originalContentBorderBrush = nullptr;
                            slot.contentCornerCaptured = true;
                        }
                        slot.hoverContent.ClearValue(
                            wux::Controls::ContentPresenter::CornerRadiusProperty());
                        slot.hoverContent.ClearValue(
                            wux::Controls::ContentPresenter::BorderThicknessProperty());
                        slot.hoverContent.ClearValue(
                            wux::Controls::ContentPresenter::BorderBrushProperty());
                        slot.contentChromePatched = true;
                    }
                    catch (...)
                    {
                    }
                }
            }

            slot.appliedOk = true;
            slot.needsRetry = false;
            m_unresolved.store(0, std::memory_order_release);

            DebugLog(
                DBG_IMPORTANT,
                L"[TaskbarCompat][HoverFlyout] applied effect=%u fill=%u chrome=%u "
                L"corners=%u tint=%u",
                patchBackdrop ? brushEffect : settings.xamlEffectType,
                slot.fillPatched ? 1u : 0u,
                slot.chromePatched ? 1u : 0u,
                wantCorners ? 1u : 0u,
                wantTintBorder ? 1u : 0u);
        }
        catch (const winrt::hresult_error& ex)
        {
            slot.needsRetry = true;
            DebugLog(
                DBG_WARNING,
                L"[TaskbarCompat][HoverFlyout] ApplyPanelVisualsSync hr=0x%08X",
                static_cast<unsigned>(ex.code()));
        }
        catch (...)
        {
            slot.needsRetry = true;
        }
    }

    void TrayHoverFlyoutPanelRegistry::ApplyPanelGuarded(std::shared_ptr<PanelSlot> slot) noexcept
    {
        if (!slot || (!slot->root && !slot->backgroundBorder && !slot->backgroundFill))
            return;
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;

        try
        {
            const auto dispatcher = slot->root
                ? slot->root.Dispatcher()
                : (slot->backgroundBorder
                       ? slot->backgroundBorder.Dispatcher()
                       : slot->backgroundFill.Dispatcher());
            if (dispatcher && !dispatcher.HasThreadAccess())
            {
                slot->appliedOk = false;
                slot->needsRetry = true;
                m_unresolved.store(1, std::memory_order_release);
                ScheduleDeferredApply(slot);
                return;
            }

            ScheduleDeferredApply(slot);
        }
        catch (...)
        {
        }
    }

    void TrayHoverFlyoutPanelRegistry::RegisterPanel(
        InstanceHandle handle,
        wux::FrameworkElement root) noexcept
    {
        if (!root || handle == 0)
            return;
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;

        try
        {
            std::shared_ptr<PanelSlot> slot;
            bool canApply = false;
            {
                std::lock_guard lock(m_mx);
                for (auto it = m_panels.begin(); it != m_panels.end();)
                {
                    if (it->first == handle)
                    {
                        ++it;
                        continue;
                    }
                    auto& other = it->second;
                    if (other
                        && ((other->backgroundBorder && !other->root)
                            || (other->backgroundFill && !other->root)))
                    {
                        slot = other;
                        it = m_panels.erase(it);
                        m_panels[handle] = slot;
                        break;
                    }
                    ++it;
                }
                if (!slot)
                {
                    auto& ref = m_panels[handle];
                    if (!ref)
                        ref = std::make_shared<PanelSlot>();
                    slot = ref;
                }
                slot->root = root;
                slot->rootHandle = handle;
                if (slot->firstSeenTick == 0)
                    slot->firstSeenTick = GetTickCount64();
                // Apply только когда уже есть Border — без VisualTree walk на Add.
                canApply = static_cast<bool>(slot->backgroundBorder);
                if (!canApply)
                    slot->needsRetry = true;
            }


            if (canApply)
            {
                m_unresolved.store(1, std::memory_order_release);
                ScheduleDeferredApply(slot);
            }
            else
            {
                m_unresolved.store(1, std::memory_order_release);
            }

            DebugLog(
                DBG_IMPORTANT,
                L"[TaskbarCompat][HoverFlyout] Panel registered handle=%llu paintXaml=%u "
                L"effect=%u corner=%u border=%u resolved=%u retry=%u",
                static_cast<unsigned long long>(handle),
                m_settings.paintPathXaml ? 1u : 0u,
                m_settings.xamlEffectType,
                m_settings.cornerType,
                m_settings.patchBorderStroke ? 1u : 0u,
                slot->visualsResolved ? 1u : 0u,
                slot->needsRetry ? 1u : 0u);
        }
        catch (const winrt::hresult_error& ex)
        {
            DebugLog(
                DBG_WARNING,
                L"[TaskbarCompat][HoverFlyout] RegisterPanel hr=0x%08X msg=%s",
                static_cast<unsigned>(ex.code()),
                ex.message().c_str());
        }
        catch (...)
        {
            DebugLog(
                DBG_WARNING, L"[TaskbarCompat][HoverFlyout] RegisterPanel unknown exception");
        }
    }

    void TrayHoverFlyoutPanelRegistry::RegisterBackgroundBorder(
        InstanceHandle handle,
        wux::Controls::Border border) noexcept
    {
        if (!border || handle == 0)
            return;
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;

        try
        {
            std::shared_ptr<PanelSlot> slot;
            bool alreadyApplied = false;
            {
                std::lock_guard lock(m_mx);
                for (auto& [h, s] : m_panels)
                {
                    (void)h;
                    if (!s)
                        continue;
                    if (s->backgroundBorder
                        && winrt::get_abi(s->backgroundBorder) == winrt::get_abi(border))
                    {
                        slot = s;
                        break;
                    }
                    // Без VisualTree walk (FindDescendant → SEH/crash на VTC Add).
                    // Сливаем с якорем, у которого ещё нет Border.
                    if (s->root && !s->backgroundBorder)
                    {
                        slot = s;
                        break;
                    }
                    if (s->borderHandle == handle)
                    {
                        slot = s;
                        break;
                    }
                }
                if (!slot && m_panels.size() == 1)
                    slot = m_panels.begin()->second;

                if (!slot)
                {
                    auto& ref = m_panels[handle];
                    if (!ref)
                        ref = std::make_shared<PanelSlot>();
                    slot = ref;
                }

                alreadyApplied = slot->appliedOk && !slot->needsRetry;
                slot->borderHandle = handle;
                if (!slot->backgroundBorder)
                    slot->backgroundBorder = border;
                if (slot->firstSeenTick == 0)
                    slot->firstSeenTick = GetTickCount64();
            }


            if (alreadyApplied)
                return;

            // На VTC Add: только store. SizeChanged/sync Apply → wil::ResultException / QI crash.
            // Apply — Dispatcher.RunAsync + RetryUnresolvedPanels после layout.
            slot->needsRetry = true;
            slot->appliedOk = false;
            m_unresolved.store(1, std::memory_order_release);
            ScheduleDeferredApply(slot);

            DebugLog(
                DBG_IMPORTANT,
                L"[TaskbarCompat][HoverFlyout] Border registered handle=%llu paintXaml=%u "
                L"effect=%u resolved=%u retry=%u",
                static_cast<unsigned long long>(handle),
                m_settings.paintPathXaml ? 1u : 0u,
                m_settings.xamlEffectType,
                slot->visualsResolved ? 1u : 0u,
                slot->needsRetry ? 1u : 0u);
        }
        catch (const winrt::hresult_error& ex)
        {
            DebugLog(
                DBG_WARNING,
                L"[TaskbarCompat][HoverFlyout] RegisterBackgroundBorder hr=0x%08X msg=%s",
                static_cast<unsigned>(ex.code()),
                ex.message().c_str());
        }
        catch (...)
        {
            DebugLog(
                DBG_WARNING,
                L"[TaskbarCompat][HoverFlyout] RegisterBackgroundBorder unknown exception");
        }
    }

    void TrayHoverFlyoutPanelRegistry::RegisterChromeGrid(
        InstanceHandle handle,
        wux::Controls::Grid grid) noexcept
    {
        if (!grid || handle == 0)
            return;
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;

        try
        {
            std::shared_ptr<PanelSlot> slot;
            bool alreadyApplied = false;
            {
                std::lock_guard lock(m_mx);
                for (auto& [h, s] : m_panels)
                {
                    (void)h;
                    if (!s)
                        continue;
                    if (s->hoverFlyoutGrid
                        && winrt::get_abi(s->hoverFlyoutGrid) == winrt::get_abi(grid))
                    {
                        slot = s;
                        break;
                    }
                    if (s->root || s->backgroundBorder || s->backgroundFill)
                    {
                        slot = s;
                        break;
                    }
                }
                if (!slot && m_panels.size() == 1)
                    slot = m_panels.begin()->second;
                if (!slot)
                {
                    auto& ref = m_panels[handle];
                    if (!ref)
                        ref = std::make_shared<PanelSlot>();
                    slot = ref;
                }

                alreadyApplied = slot->appliedOk && !slot->needsRetry;
                slot->gridHandle = handle;
                if (!slot->hoverFlyoutGrid)
                {
                    slot->hoverFlyoutGrid = grid;
                    if (!slot->gridCornerCaptured)
                    {
                        slot->originalGridCornerRadius = {};
                        slot->gridCornerCaptured = true;
                    }
                }
                if (slot->firstSeenTick == 0)
                    slot->firstSeenTick = GetTickCount64();
            }


            if (alreadyApplied)
                return;

            slot->needsRetry = true;
            slot->appliedOk = false;
            m_unresolved.store(1, std::memory_order_release);
            ScheduleDeferredApply(slot);
        }
        catch (...)
        {
        }
    }

    void TrayHoverFlyoutPanelRegistry::RegisterBackgroundFill(
        InstanceHandle handle,
        wux::Shapes::Rectangle fill) noexcept
    {
        if (!fill || handle == 0)
            return;
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;

        try
        {
            std::shared_ptr<PanelSlot> slot;
            bool alreadyApplied = false;
            {
                std::lock_guard lock(m_mx);
                for (auto& [h, s] : m_panels)
                {
                    (void)h;
                    if (!s)
                        continue;
                    if (s->backgroundFill
                        && winrt::get_abi(s->backgroundFill) == winrt::get_abi(fill))
                    {
                        slot = s;
                        break;
                    }
                    if (s->fillHandle == handle)
                    {
                        slot = s;
                        break;
                    }
                    if (s->root || s->backgroundBorder)
                    {
                        slot = s;
                        break;
                    }
                }
                if (!slot && m_panels.size() == 1)
                    slot = m_panels.begin()->second;

                if (!slot)
                {
                    auto& ref = m_panels[handle];
                    if (!ref)
                        ref = std::make_shared<PanelSlot>();
                    slot = ref;
                }

                alreadyApplied = slot->appliedOk && !slot->needsRetry;
                slot->fillHandle = handle;
                if (!slot->backgroundFill)
                {
                    slot->backgroundFill = fill;
                    try
                    {
                        if (!slot->originalFill)
                            slot->originalFill = fill.Fill();
                    }
                    catch (...)
                    {
                    }
                }
                if (slot->firstSeenTick == 0)
                    slot->firstSeenTick = GetTickCount64();
            }

            if (alreadyApplied)
                return;

            slot->needsRetry = true;
            slot->appliedOk = false;
            m_unresolved.store(1, std::memory_order_release);
            ScheduleDeferredApply(slot);

            DebugLog(
                DBG_IMPORTANT,
                L"[TaskbarCompat][HoverFlyout] Fill registered handle=%llu paintXaml=%u "
                L"effect=%u resolved=%u retry=%u",
                static_cast<unsigned long long>(handle),
                m_settings.paintPathXaml ? 1u : 0u,
                m_settings.xamlEffectType,
                slot->visualsResolved ? 1u : 0u,
                slot->needsRetry ? 1u : 0u);
        }
        catch (const winrt::hresult_error& ex)
        {
            DebugLog(
                DBG_WARNING,
                L"[TaskbarCompat][HoverFlyout] RegisterBackgroundFill hr=0x%08X msg=%s",
                static_cast<unsigned>(ex.code()),
                ex.message().c_str());
        }
        catch (...)
        {
            DebugLog(
                DBG_WARNING,
                L"[TaskbarCompat][HoverFlyout] RegisterBackgroundFill unknown exception");
        }
    }

    void TrayHoverFlyoutPanelRegistry::RegisterBackgroundStroke(
        InstanceHandle handle,
        wux::Shapes::Rectangle stroke) noexcept
    {
        if (!stroke || handle == 0)
            return;
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;

        try
        {
            std::shared_ptr<PanelSlot> slot;
            {
                std::lock_guard lock(m_mx);
                for (auto& [h, s] : m_panels)
                {
                    (void)h;
                    if (!s)
                        continue;
                    if (s->backgroundStroke
                        && winrt::get_abi(s->backgroundStroke) == winrt::get_abi(stroke))
                    {
                        slot = s;
                        break;
                    }
                    if (s->strokeHandle == handle)
                    {
                        slot = s;
                        break;
                    }
                    if (s->root || s->backgroundBorder || s->backgroundFill)
                    {
                        slot = s;
                        break;
                    }
                }
                if (!slot && m_panels.size() == 1)
                    slot = m_panels.begin()->second;

                if (!slot)
                {
                    auto& ref = m_panels[handle];
                    if (!ref)
                        ref = std::make_shared<PanelSlot>();
                    slot = ref;
                }

                slot->strokeHandle = handle;
                if (!slot->backgroundStroke)
                {
                    slot->backgroundStroke = stroke;
                    try
                    {
                        slot->originalStrokeVisibility = stroke.Visibility();
                        slot->strokeVisibilityCaptured = true;
                    }
                    catch (...)
                    {
                    }
                }
                if (slot->firstSeenTick == 0)
                    slot->firstSeenTick = GetTickCount64();
            }

            if (slot->backgroundFill || slot->backgroundBorder)
            {
                slot->appliedOk = false;
                slot->needsRetry = true;
                m_unresolved.store(1, std::memory_order_release);
                ScheduleDeferredApply(slot);
            }
        }
        catch (...)
        {
        }
    }

    void TrayHoverFlyoutPanelRegistry::RetryUnresolvedPanels() noexcept
    {
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;

        std::vector<std::shared_ptr<PanelSlot>> snapshot;
        {
            std::lock_guard lock(m_mx);
            if (m_panels.empty())
                return;
            snapshot.reserve(m_panels.size());
            for (auto& [h, s] : m_panels)
            {
                (void)h;
                if (s && (s->needsRetry || !s->appliedOk || !s->visualsResolved))
                    snapshot.push_back(s);
            }
        }
        if (snapshot.empty())
        {
            m_unresolved.store(0, std::memory_order_release);
            return;
        }

        unsigned still = 0;
        for (auto& s : snapshot)
        {
            if (s->appliedOk && !s->needsRetry)
                continue;
            if (!s->backgroundBorder && !s->backgroundFill && !s->root)
            {
                ++still;
                continue;
            }
            ScheduleDeferredApply(s);
            if (s->needsRetry || !s->appliedOk)
                ++still;
        }
        m_unresolved.store(still, std::memory_order_release);
    }

    void TrayHoverFlyoutPanelRegistry::Unregister(InstanceHandle handle) noexcept
    {
        std::shared_ptr<PanelSlot> slot;
        {
            std::lock_guard lock(m_mx);
            const auto it = m_panels.find(handle);
            if (it != m_panels.end())
            {
                slot = it->second;
                m_panels.erase(it);
            }
            else
            {
                for (auto pit = m_panels.begin(); pit != m_panels.end(); ++pit)
                {
                    if (!pit->second)
                        continue;
                    if (pit->second->borderHandle == handle
                        || pit->second->gridHandle == handle
                        || pit->second->fillHandle == handle
                        || pit->second->strokeHandle == handle)
                    {
                        slot = pit->second;
                        m_panels.erase(pit);
                        break;
                    }
                }
            }
        }
        if (!slot)
            return;

        try
        {
            bool hadOrig = false;
            bool usedDisp = false;
            (void)RestoreSlotOnUiThread(std::move(slot), true, hadOrig, usedDisp);
        }
        catch (...)
        {
        }
    }

    void TrayHoverFlyoutPanelRegistry::RestoreAll() noexcept
    {
        m_state->shutdown.store(true, std::memory_order_release);

        std::vector<std::shared_ptr<PanelSlot>> snapshot;
        {
            std::lock_guard lock(m_mx);
            snapshot.reserve(m_panels.size());
            for (auto& [h, s] : m_panels)
            {
                (void)h;
                if (s)
                    snapshot.push_back(std::move(s));
            }
            m_panels.clear();
        }
        m_unresolved.store(0, std::memory_order_release);

        const unsigned total = static_cast<unsigned>(snapshot.size());
        unsigned ok = 0;
        unsigned fail = 0;
        unsigned noOrig = 0;
        unsigned viaDispatcher = 0;
        for (auto& slot : snapshot)
        {
            bool hadOrig = false;
            bool usedDispatcher = false;
            const HRESULT hr = RestoreSlotOnUiThread(slot, true, hadOrig, usedDispatcher);
            if (usedDispatcher)
                ++viaDispatcher;
            if (!hadOrig)
                ++noOrig;
            if (SUCCEEDED(hr))
                ++ok;
            else
                ++fail;
        }

        const int pending = m_state->pendingAsync.load(std::memory_order_acquire);
        PinModuleIfPendingAsync(pending);

        DebugLog(
            DBG_IMPORTANT,
            L"[TaskbarCompat][HoverFlyout] RestoreAll total=%u ok=%u fail=%u noOrig=%u "
            L"disp=%u pendingAsync=%d",
            total,
            ok,
            fail,
            noOrig,
            viaDispatcher,
            pending);
    }

    void TrayHoverFlyoutPanelRegistry::ReapplyPatchedBrushes() noexcept
    {
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;

        std::vector<std::shared_ptr<PanelSlot>> snapshot;
        {
            std::lock_guard lock(m_mx);
            snapshot.reserve(m_panels.size());
            for (auto& [h, s] : m_panels)
            {
                (void)h;
                if (s)
                {
                    s->appliedOk = false;
                    s->needsRetry = true;
                    snapshot.push_back(s);
                }
            }
        }
        for (auto& s : snapshot)
            ApplyPanelGuarded(s);
    }

    void TrayHoverFlyoutPanelRegistry::RecaptureAfterThemeChange(Registry::Settings settings) noexcept
    {
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;
        {
            std::lock_guard lock(m_mx);
            m_settings = settings;
        }

        std::vector<std::shared_ptr<PanelSlot>> snapshot;
        {
            std::lock_guard lock(m_mx);
            snapshot.reserve(m_panels.size());
            for (auto& [h, s] : m_panels)
            {
                (void)h;
                if (s)
                    snapshot.push_back(s);
            }
        }
        for (auto& slot : snapshot)
        {
            if (!slot)
                continue;
            try
            {
                if (slot->backgroundFill)
                    slot->backgroundFill.ClearValue(wux::Shapes::Shape::FillProperty());
                if (slot->backgroundBorder)
                {
                    slot->backgroundBorder.ClearValue(wux::Controls::Border::BackgroundProperty());
                    slot->backgroundBorder.ClearValue(wux::Controls::Border::BorderBrushProperty());
                    slot->backgroundBorder.ClearValue(wux::Controls::Border::BorderThicknessProperty());
                    slot->backgroundBorder.ClearValue(wux::Controls::Border::CornerRadiusProperty());
                }
                if (slot->hoverFlyoutGrid)
                    slot->hoverFlyoutGrid.ClearValue(wux::Controls::Panel::BackgroundProperty());
            }
            catch (...)
            {
            }
            slot->originalFill = nullptr;
            slot->originalBorderBackground = nullptr;
            slot->originalBorderBrush = nullptr;
            slot->originalHoverGridBackground = nullptr;
            slot->originalShadow = nullptr;
            slot->chromeCaptured = false;
            slot->chromePatched = false;
            slot->fillPatched = false;
            slot->appliedOk = false;
            slot->needsRetry = true;
            slot->visualsResolved = false;
        }
        for (auto& s : snapshot)
            ApplyPanelGuarded(s);
    }

    void TrayHoverFlyoutPanelRegistry::UpdateSettings(Registry::Settings settings) noexcept
    {
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;
        {
            std::lock_guard lock(m_mx);
            m_settings = settings;
        }
        ReapplyPatchedBrushes();
    }
}
