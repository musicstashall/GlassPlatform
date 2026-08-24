#include "UiPch.h"
#include "xaml/ShellTooltipPanelRegistry.hpp"
#include "xaml/XamlDiagInit.hpp"
#include "xaml/brushes/XamlBackdropFactory.hpp"

#include <RootBackdropPolicy.h>
#include <TrayXamlPaint.h>

#include <DebugLog.h>

#include <vector>

#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>

namespace UICompat
{
    namespace
    {
        namespace wux = winrt::Windows::UI::Xaml;

        constexpr DWORD kRestoreWaitMs = 250;
        constexpr int kMaxResolveDepth = 4;

        [[nodiscard]] winrt::Windows::UI::Color ColorFromArgb(DWORD argb) noexcept
        {
            return winrt::Windows::UI::Color{
                static_cast<uint8_t>((argb >> 24) & 0xFFu),
                static_cast<uint8_t>((argb >> 16) & 0xFFu),
                static_cast<uint8_t>((argb >> 8) & 0xFFu),
                static_cast<uint8_t>(argb & 0xFFu),
            };
        }

        [[nodiscard]] bool ThicknessAnyPositive(const wux::Thickness& t) noexcept
        {
            return t.Left > 0.0 || t.Top > 0.0 || t.Right > 0.0 || t.Bottom > 0.0;
        }

        [[nodiscard]] double QueryVisibleHeightDip(wux::FrameworkElement const& fe) noexcept
        {
            if (!fe)
                return 0.0;
            try
            {
                const double h = fe.ActualHeight();
                return h > 0.0 ? h : 0.0;
            }
            catch (...)
            {
                return 0.0;
            }
        }

        [[nodiscard]] bool TipLayoutReady(wux::Controls::ToolTip const& tip) noexcept
        {
            if (!tip)
                return false;
            try
            {
                return tip.ActualWidth() >= 1.0 && tip.ActualHeight() >= 1.0;
            }
            catch (...)
            {
                return false;
            }
        }

        // Первый Border в шаблоне ToolTip — системный chrome (анти-двойной blur).
        [[nodiscard]] wux::Controls::Border FindInnerBorder(
            wux::DependencyObject root,
            int depth) noexcept
        {
            if (!root)
                return nullptr;
            try
            {
                const auto walk = [&](auto&& self, wux::DependencyObject node, int d)
                    -> wux::Controls::Border
                {
                    if (!node || d > kMaxResolveDepth)
                        return nullptr;
                    if (d >= 1)
                    {
                        if (const auto border = node.try_as<wux::Controls::Border>())
                            return border;
                    }
                    const int n = wux::Media::VisualTreeHelper::GetChildrenCount(node);
                    for (int i = 0; i < n; ++i)
                    {
                        if (const auto found =
                                self(self, wux::Media::VisualTreeHelper::GetChild(node, i), d + 1))
                            return found;
                    }
                    return nullptr;
                };
                return walk(walk, root, depth);
            }
            catch (...)
            {
                return nullptr;
            }
        }

        void RestoreControlBackground(
            wux::Controls::Control const& ctrl,
            wux::Media::Brush const& originalBackground,
            bool& hadOrig,
            bool preferNeutral) noexcept
        {
            hadOrig = static_cast<bool>(originalBackground);
            try
            {
                // Лог T1 preferNeutral:0 hadOrig:1 — SetValue(stale) до/без epoch
                // → тултип со старой темой после Stop. Никогда не SetValue(original)
                // на Background: ClearValue; после theme — Neutral (ThemeResource Tint).
                ctrl.ClearValue(wux::Controls::Control::BackgroundProperty());
                if (preferNeutral)
                {
                    if (const auto neut = CreateNeutralAcrylicBackdropBrush())
                    {
                        ctrl.Background(neut);
                        hadOrig = true;
                    }
                }
                ctrl.InvalidateArrange();
                ctrl.InvalidateMeasure();
                ctrl.UpdateLayout();
            }
            catch (...)
            {
            }
        }

        void RestoreBorderBackground(
            wux::Controls::Border const& border,
            wux::Media::Brush const& originalBackground,
            bool& hadOrig,
            bool preferNeutral) noexcept
        {
            hadOrig = static_cast<bool>(originalBackground);
            try
            {
                border.ClearValue(wux::Controls::Border::BackgroundProperty());
                if (preferNeutral)
                {
                    if (const auto neut = CreateNeutralAcrylicBackdropBrush())
                    {
                        border.Background(neut);
                        hadOrig = true;
                    }
                }
                border.InvalidateArrange();
                border.InvalidateMeasure();
                border.UpdateLayout();
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
                    L"[UICompat][ShellTooltip] PIN failed GLE=%u pendingAsync=%d",
                    static_cast<unsigned>(GetLastError()),
                    pending);
                return;
            }
            DebugLog(
                DBG_WARNING,
                L"[UICompat][ShellTooltip] module PIN (pendingAsync=%d)",
                pending);
        }

        void ReleasePropertyChain(
            PropertyChainSource* sources,
            unsigned sourceCount,
            PropertyChainValue* values,
            unsigned valueCount) noexcept
        {
            if (sources)
            {
                for (unsigned i = 0; i < sourceCount; ++i)
                {
                    SysFreeString(sources[i].TargetType);
                    SysFreeString(sources[i].Name);
                }
                CoTaskMemFree(sources);
            }
            if (values)
            {
                for (unsigned i = 0; i < valueCount; ++i)
                {
                    SysFreeString(values[i].Type);
                    SysFreeString(values[i].DeclaringType);
                    SysFreeString(values[i].ValueType);
                    SysFreeString(values[i].ItemType);
                    SysFreeString(values[i].Value);
                    SysFreeString(values[i].PropertyName);
                }
                CoTaskMemFree(values);
            }
        }

        [[nodiscard]] ::IInspectable* DetachInspectable(
            winrt::Windows::Foundation::IInspectable const& value) noexcept
        {
            if (!value)
                return nullptr;
            void* raw = nullptr;
            winrt::copy_to_abi(value, raw);
            return reinterpret_cast<::IInspectable*>(raw);
        }
    }

    ShellTooltipPanelRegistry::ShellTooltipPanelRegistry(
        IXamlDiagnostics* diagnostics,
        Registry::Settings settings) noexcept
        : m_settings(settings)
    {
        if (!diagnostics)
            return;
        m_diagnostics.copy_from(diagnostics);
        (void)diagnostics->QueryInterface(
            __uuidof(IVisualTreeService3), m_vts3.put_void());
    }

    unsigned ShellTooltipPanelRegistry::PanelCount() const noexcept
    {
        std::lock_guard lock(m_mx);
        return static_cast<unsigned>(m_panels.size());
    }

    unsigned ShellTooltipPanelRegistry::UnresolvedCount() const noexcept
    {
        return m_unresolved.load(std::memory_order_acquire);
    }

    bool ShellTooltipPanelRegistry::EnsureTipHandle(PanelSlot& slot) noexcept
    {
        if (slot.tipHandle != 0)
            return true;
        if (!slot.tip || !m_diagnostics)
            return false;
        try
        {
            ::IInspectable* raw = DetachInspectable(
                slot.tip.as<winrt::Windows::Foundation::IInspectable>());
            if (!raw)
                return false;
            InstanceHandle h = 0;
            const HRESULT hr = m_diagnostics->GetHandleFromIInspectable(raw, &h);
            raw->Release();
            if (FAILED(hr) || h == 0)
                return false;
            slot.tipHandle = h;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool ShellTooltipPanelRegistry::EnsureInnerHandle(PanelSlot& slot) noexcept
    {
        if (slot.innerHandle != 0)
            return true;
        if (!slot.innerBorder || !m_diagnostics)
            return false;
        try
        {
            ::IInspectable* raw = DetachInspectable(
                slot.innerBorder.as<winrt::Windows::Foundation::IInspectable>());
            if (!raw)
                return false;
            InstanceHandle h = 0;
            const HRESULT hr = m_diagnostics->GetHandleFromIInspectable(raw, &h);
            raw->Release();
            if (FAILED(hr) || h == 0)
                return false;
            slot.innerHandle = h;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool ShellTooltipPanelRegistry::ClearPropByIndex(
        InstanceHandle element,
        unsigned propIndex) noexcept
    {
        if (!m_vts3 || element == 0 || propIndex == UINT_MAX)
            return false;
        return SUCCEEDED(m_vts3->ClearProperty(element, propIndex));
    }

    bool ShellTooltipPanelRegistry::CaptureTipCornerVts3(PanelSlot& slot) noexcept
    {
        if (!m_diagnostics || !m_vts3)
            return false;
        if (!EnsureTipHandle(slot))
            return false;
        if (slot.tipCornerPropIndex != UINT_MAX && slot.originalTipCornerVts3)
            return true;

        unsigned sourceCount = 0;
        unsigned propertyCount = 0;
        PropertyChainSource* sources = nullptr;
        PropertyChainValue* properties = nullptr;
        const HRESULT chainHr = m_vts3->GetPropertyValuesChain(
            slot.tipHandle,
            &sourceCount,
            &sources,
            &propertyCount,
            &properties);

        unsigned propertyIndex = UINT_MAX;
        if (SUCCEEDED(chainHr) && properties)
        {
            for (unsigned p = 0; p < propertyCount; ++p)
            {
                if (properties[p].PropertyName
                    && wcscmp(properties[p].PropertyName, L"CornerRadius") == 0)
                {
                    propertyIndex = properties[p].Index;
                    break;
                }
            }
        }

        InstanceHandle propertyHandle = 0;
        ::IInspectable* inspectable = nullptr;
        HRESULT propertyHr = (propertyIndex != UINT_MAX) ? S_OK : E_FAIL;
        if (SUCCEEDED(propertyHr))
            propertyHr = m_vts3->GetProperty(
                slot.tipHandle, propertyIndex, &propertyHandle);
        if (SUCCEEDED(propertyHr) && propertyHandle != 0)
        {
            propertyHr = m_diagnostics->GetIInspectableFromHandle(
                propertyHandle, &inspectable);
        }

        ReleasePropertyChain(sources, sourceCount, properties, propertyCount);

        if (SUCCEEDED(propertyHr) && inspectable)
        {
            slot.tipCornerPropIndex = propertyIndex;
            slot.originalTipCornerVts3.attach(inspectable);
            return true;
        }
        if (inspectable)
            inspectable->Release();
        return false;
    }

    bool ShellTooltipPanelRegistry::CaptureInnerCornerVts3(PanelSlot& slot) noexcept
    {
        if (!m_diagnostics || !m_vts3)
            return false;
        if (!EnsureInnerHandle(slot))
            return false;
        if (slot.innerCornerPropIndex != UINT_MAX && slot.originalInnerCornerVts3)
            return true;

        unsigned sourceCount = 0;
        unsigned propertyCount = 0;
        PropertyChainSource* sources = nullptr;
        PropertyChainValue* properties = nullptr;
        const HRESULT chainHr = m_vts3->GetPropertyValuesChain(
            slot.innerHandle,
            &sourceCount,
            &sources,
            &propertyCount,
            &properties);

        unsigned propertyIndex = UINT_MAX;
        if (SUCCEEDED(chainHr) && properties)
        {
            for (unsigned p = 0; p < propertyCount; ++p)
            {
                if (properties[p].PropertyName
                    && wcscmp(properties[p].PropertyName, L"CornerRadius") == 0)
                {
                    propertyIndex = properties[p].Index;
                    break;
                }
            }
        }

        InstanceHandle propertyHandle = 0;
        ::IInspectable* inspectable = nullptr;
        HRESULT propertyHr = (propertyIndex != UINT_MAX) ? S_OK : E_FAIL;
        if (SUCCEEDED(propertyHr))
            propertyHr = m_vts3->GetProperty(
                slot.innerHandle, propertyIndex, &propertyHandle);
        if (SUCCEEDED(propertyHr) && propertyHandle != 0)
        {
            propertyHr = m_diagnostics->GetIInspectableFromHandle(
                propertyHandle, &inspectable);
        }

        ReleasePropertyChain(sources, sourceCount, properties, propertyCount);

        if (SUCCEEDED(propertyHr) && inspectable)
        {
            slot.innerCornerPropIndex = propertyIndex;
            slot.originalInnerCornerVts3.attach(inspectable);
            return true;
        }
        if (inspectable)
            inspectable->Release();
        return false;
    }

    bool ShellTooltipPanelRegistry::SetTipCornerVts3(
        PanelSlot& slot,
        wux::CornerRadius const& cr) noexcept
    {
        if (!m_diagnostics || !m_vts3)
            return false;
        if (!EnsureTipHandle(slot))
            return false;
        if (slot.tipCornerPropIndex == UINT_MAX && !CaptureTipCornerVts3(slot))
            return false;
        if (slot.tipCornerPropIndex == UINT_MAX)
            return false;

        ::IInspectable* crInsp = DetachInspectable(winrt::box_value(cr));
        if (!crInsp)
            return false;
        InstanceHandle valueHandle = 0;
        const HRESULT hrHandle =
            m_diagnostics->GetHandleFromIInspectable(crInsp, &valueHandle);
        const HRESULT hrSet =
            (SUCCEEDED(hrHandle) && valueHandle != 0)
            ? m_vts3->SetProperty(slot.tipHandle, valueHandle, slot.tipCornerPropIndex)
            : E_FAIL;
        crInsp->Release();
        return SUCCEEDED(hrSet);
    }

    bool ShellTooltipPanelRegistry::SetInnerCornerVts3(
        PanelSlot& slot,
        wux::CornerRadius const& cr) noexcept
    {
        if (!m_diagnostics || !m_vts3)
            return false;
        if (!EnsureInnerHandle(slot))
            return false;
        if (slot.innerCornerPropIndex == UINT_MAX && !CaptureInnerCornerVts3(slot))
            return false;
        if (slot.innerCornerPropIndex == UINT_MAX)
            return false;

        ::IInspectable* crInsp = DetachInspectable(winrt::box_value(cr));
        if (!crInsp)
            return false;
        InstanceHandle valueHandle = 0;
        const HRESULT hrHandle =
            m_diagnostics->GetHandleFromIInspectable(crInsp, &valueHandle);
        const HRESULT hrSet =
            (SUCCEEDED(hrHandle) && valueHandle != 0)
            ? m_vts3->SetProperty(slot.innerHandle, valueHandle, slot.innerCornerPropIndex)
            : E_FAIL;
        crInsp->Release();
        return SUCCEEDED(hrSet);
    }

    bool ShellTooltipPanelRegistry::RestoreTipCornerVts3(PanelSlot& slot) noexcept
    {
        if (!m_diagnostics || !m_vts3)
            return false;
        if (slot.tipHandle == 0 || slot.tipCornerPropIndex == UINT_MAX)
            return false;

        if (slot.originalTipCornerVts3)
        {
            InstanceHandle valueHandle = 0;
            const HRESULT hrHandle = m_diagnostics->GetHandleFromIInspectable(
                slot.originalTipCornerVts3.get(), &valueHandle);
            if (SUCCEEDED(hrHandle) && valueHandle != 0
                && SUCCEEDED(m_vts3->SetProperty(
                    slot.tipHandle, valueHandle, slot.tipCornerPropIndex)))
            {
                return true;
            }
        }
        return ClearPropByIndex(slot.tipHandle, slot.tipCornerPropIndex);
    }

    bool ShellTooltipPanelRegistry::RestoreInnerCornerVts3(PanelSlot& slot) noexcept
    {
        if (!m_diagnostics || !m_vts3)
            return false;
        if (slot.innerHandle == 0 || slot.innerCornerPropIndex == UINT_MAX)
            return false;

        if (slot.originalInnerCornerVts3)
        {
            InstanceHandle valueHandle = 0;
            const HRESULT hrHandle = m_diagnostics->GetHandleFromIInspectable(
                slot.originalInnerCornerVts3.get(), &valueHandle);
            if (SUCCEEDED(hrHandle) && valueHandle != 0
                && SUCCEEDED(m_vts3->SetProperty(
                    slot.innerHandle, valueHandle, slot.innerCornerPropIndex)))
            {
                return true;
            }
        }
        return ClearPropByIndex(slot.innerHandle, slot.innerCornerPropIndex);
    }

    bool ShellTooltipPanelRegistry::ResolveVisuals(PanelSlot& slot) noexcept
    {
        if (!slot.tip)
            return false;

        try
        {
            const auto found = FindInnerBorder(slot.tip, 0);
            const bool borderChanged =
                (!slot.innerBorder && found)
                || (slot.innerBorder && found
                    && winrt::get_abi(slot.innerBorder) != winrt::get_abi(found));
            slot.innerBorder = found;

            if (!slot.originalBackground)
                slot.originalBackground = slot.tip.Background();

            if (borderChanged)
            {
                slot.appliedOk = false;
                slot.needsRetry = true;
                try
                {
                    if (slot.innerBorder)
                        slot.originalInnerBackground = slot.innerBorder.Background();
                }
                catch (...)
                {
                }
            }
            else if (slot.innerBorder && !slot.originalInnerBackground)
            {
                slot.originalInnerBackground = slot.innerBorder.Background();
            }

            CaptureChrome(slot);
            slot.visualsResolved = true;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    void ShellTooltipPanelRegistry::CaptureChrome(PanelSlot& slot) noexcept
    {
        if (!slot.tip || slot.chromeCaptured)
            return;
        try
        {
            slot.originalBorderBrush = slot.tip.BorderBrush();
            slot.originalBorderThickness = slot.tip.BorderThickness();
            // SMEH: CornerRadius() GET → fail-fast. Только VTS3.
            if (Shared::RootBackdropPolicy::IsStartMenuHostProcess())
            {
                (void)CaptureTipCornerVts3(slot);
                if (slot.innerBorder)
                    (void)CaptureInnerCornerVts3(slot);
                slot.originalCornerRadius = {};
            }
            else
            {
                slot.originalCornerRadius = slot.tip.CornerRadius();
            }
            try
            {
                slot.originalShadow = slot.tip.Shadow();
            }
            catch (...)
            {
                slot.originalShadow = nullptr;
            }
            slot.chromeCaptured = true;
        }
        catch (...)
        {
            slot.originalBorderBrush = nullptr;
            slot.originalBorderThickness = {};
            slot.originalCornerRadius = {};
            slot.originalShadow = nullptr;
            slot.chromeCaptured = true;
        }
    }

    void ShellTooltipPanelRegistry::EnsureSizeHook(std::shared_ptr<PanelSlot> slot) noexcept
    {
        if (!slot || !slot->tip || slot->sizeHooked)
            return;
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;
        if (TipLayoutReady(slot->tip) && slot->appliedOk && !slot->needsRetry)
            return;

        try
        {
            std::weak_ptr<PanelSlot> weakSlot = slot;
            std::weak_ptr<RegistryState> weakState = m_state;
            ShellTooltipPanelRegistry* self = this;
            slot->sizeRevoker = slot->tip.SizeChanged(
                winrt::auto_revoke,
                [weakSlot, weakState, self](
                    winrt::Windows::Foundation::IInspectable const&,
                    wux::SizeChangedEventArgs const&) noexcept
                {
                    try
                    {
                        auto state = weakState.lock();
                        auto s = weakSlot.lock();
                        if (!state || !self)
                            return;
                        if (state->shutdown.load(std::memory_order_acquire))
                            return;
                        if (!s || !s->tip)
                            return;
                        if (s->sizeApplyDone || s->sizeApplyBusy)
                            return;
                        if (!TipLayoutReady(s->tip))
                            return;
                        s->sizeApplyBusy = true;
                        s->needsRetry = true;
                        s->appliedOk = false;
                        self->ApplyPanelVisualsSync(*s);
                        if (s->appliedOk)
                            s->sizeApplyDone = true;
                        s->sizeApplyBusy = false;
                    }
                    catch (...)
                    {
                        if (auto s = weakSlot.lock())
                            s->sizeApplyBusy = false;
                    }
                });
            slot->sizeHooked = true;
        }
        catch (...)
        {
        }
    }

    void ShellTooltipPanelRegistry::RestoreChrome(PanelSlot& slot) noexcept
    {
        if (!slot.tip || !slot.chromeCaptured || !slot.chromePatched)
        {
            slot.chromePatched = false;
            return;
        }
        try
        {
            if (Shared::RootBackdropPolicy::IsStartMenuHostProcess())
            {
                // SMEH: WinRT set → fail-fast. Только VTS3.
                try
                {
                    slot.tip.BorderBrush(slot.originalBorderBrush);
                    slot.tip.BorderThickness(slot.originalBorderThickness);
                }
                catch (...)
                {
                }
                slot.tipHandle = 0;
                if (EnsureTipHandle(slot))
                    (void)RestoreTipCornerVts3(slot);
                if (slot.innerBorder)
                {
                    slot.innerHandle = 0;
                    if (EnsureInnerHandle(slot))
                        (void)RestoreInnerCornerVts3(slot);
                }
            }
            else
            {
                slot.tip.BorderBrush(slot.originalBorderBrush);
                slot.tip.BorderThickness(slot.originalBorderThickness);
                slot.tip.CornerRadius(slot.originalCornerRadius);
            }
            try
            {
                slot.tip.Shadow(slot.originalShadow);
            }
            catch (...)
            {
            }
        }
        catch (...)
        {
        }
        slot.chromePatched = false;
    }

    void ShellTooltipPanelRegistry::RestorePanelVisuals(
        PanelSlot& slot,
        bool restoreBackground,
        bool& hadOrig) noexcept
    {
        hadOrig = false;
        RestoreChrome(slot);

        const bool preferNeutral =
            m_themePreferNeutralRestore.load(std::memory_order_acquire);

        if (restoreBackground && slot.innerBorder)
        {
            bool innerHad = false;
            RestoreBorderBackground(
                slot.innerBorder, slot.originalInnerBackground, innerHad, preferNeutral);
        }

        if (restoreBackground && slot.tip)
        {
            RestoreControlBackground(
                slot.tip, slot.originalBackground, hadOrig, preferNeutral);
        }
        else
            hadOrig = static_cast<bool>(slot.originalBackground);
    }

    HRESULT ShellTooltipPanelRegistry::RestoreSlotOnUiThread(
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
            if (!slot->tip)
                return S_FALSE;

            const auto dispatcher = slot->tip.Dispatcher();

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
                    bool preferNeutral{ false };
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
                job->preferNeutral =
                    m_themePreferNeutralRestore.load(std::memory_order_acquire);

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
                                        auto& s = *job->slot;
                                        if (s.tip && s.chromeCaptured && s.chromePatched)
                                        {
                                            try
                                            {
                                                const bool smeh =
                                                    Shared::RootBackdropPolicy::IsStartMenuHostProcess();
                                                if (smeh)
                                                {
                                                    s.tip.BorderBrush(s.originalBorderBrush);
                                                    s.tip.BorderThickness(s.originalBorderThickness);
                                                    s.tipHandle = 0;
                                                    s.innerHandle = 0;
                                                }
                                                else
                                                {
                                                    s.tip.BorderBrush(s.originalBorderBrush);
                                                    s.tip.BorderThickness(s.originalBorderThickness);
                                                    s.tip.CornerRadius(s.originalCornerRadius);
                                                }
                                                try
                                                {
                                                    s.tip.Shadow(s.originalShadow);
                                                }
                                                catch (...)
                                                {
                                                }
                                            }
                                            catch (...)
                                            {
                                            }
                                            s.chromePatched = false;
                                        }
                                        if (job->restoreBackground && s.innerBorder)
                                        {
                                            bool innerHad = false;
                                            RestoreBorderBackground(
                                                s.innerBorder,
                                                s.originalInnerBackground,
                                                innerHad,
                                                job->preferNeutral);
                                        }
                                        if (job->restoreBackground && s.tip)
                                        {
                                            RestoreControlBackground(
                                                s.tip,
                                                s.originalBackground,
                                                job->hadOrig,
                                                job->preferNeutral);
                                        }
                                        else
                                        {
                                            job->hadOrig =
                                                static_cast<bool>(s.originalBackground);
                                        }
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
                    L"[UICompat][ShellTooltip] Restore wait timeout (%u ms)",
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

    void ShellTooltipPanelRegistry::ApplyPanelVisualsSync(PanelSlot& slot) noexcept
    {
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;
        if (!slot.tip)
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

        CaptureChrome(slot);

        if (!settings.paintPathXaml)
        {
            bool hadOrig = false;
            RestorePanelVisuals(slot, true, hadOrig);
            slot.needsRetry = false;
            slot.appliedOk = true;
            return;
        }

        // CreateBackdropBrush на slot.tip (не GetElementVisual popup HWND).
        wux::Media::Brush brush = CreateBackdropBrush(
            slot.tip,
            settings.xamlEffectType,
            settings.tintArgb);

        if (!brush)
        {
            slot.needsRetry = true;
            return;
        }

        try
        {
            slot.tip.Background(brush);

            // Анти-двойной blur: innerBorder → Transparent.
            if (slot.innerBorder)
            {
                winrt::Windows::UI::Color clear{};
                clear.A = 0;
                wux::Media::SolidColorBrush solid;
                solid.Color(clear);
                slot.innerBorder.Background(solid);
            }

            try
            {
                slot.tip.Shadow(nullptr);
            }
            catch (...)
            {
            }

            const bool wantCorners =
                settings.cornersPresent && settings.cornerType != 0;
            const bool wantTintBorder =
                settings.patchBorder && settings.borderArgb != 0;

            const double trayH = Shared::TrayXamlPaint::QueryTaskbarHeightDip();
            const double popupH = QueryVisibleHeightDip(slot.tip);
            double appliedR = 0.0;

            if (wantCorners)
            {
                if (!TipLayoutReady(slot.tip))
                {
                    slot.needsRetry = true;
                    slot.appliedOk = false;
                    return;
                }

                appliedR = Shared::TrayXamlPaint::XamlPopupCornerRadiusDip(
                    settings.cornerType, trayH, popupH);
                const wux::CornerRadius cr{ appliedR, appliedR, appliedR, appliedR };

                if (Shared::RootBackdropPolicy::IsStartMenuHostProcess())
                {
                    // SMEH: WinRT set → fail-fast. Только VTS3.
                    if (!SetTipCornerVts3(slot, cr))
                        slot.needsRetry = true;
                    if (slot.innerBorder && !SetInnerCornerVts3(slot, cr))
                        slot.needsRetry = true;
                }
                else
                {
                    slot.tip.CornerRadius(cr);
                    if (slot.innerBorder)
                        slot.innerBorder.CornerRadius(cr);
                }
                slot.chromePatched = true;
            }
            else if (!wantTintBorder && !settings.patchBorder)
            {
                bool hadOrig = false;
                RestorePanelVisuals(slot, false, hadOrig);
            }
            else if (slot.chromeCaptured)
            {
                // cornerType=0 при patchBorder=true: восстановить системный CornerRadius.
                // Для SMEH: не трогать (VTS3 захватывает CornerRadius=0 из темы).
                // Для других: восстановить WinRT-свойство.
                if (!Shared::RootBackdropPolicy::IsStartMenuHostProcess())
                {
                    try
                    {
                        slot.tip.CornerRadius(slot.originalCornerRadius);
                        if (slot.innerBorder && slot.originalCornerRadius.TopLeft > 0.0)
                            slot.innerBorder.CornerRadius(slot.originalCornerRadius);
                    }
                    catch (...)
                    {
                    }
                }
            }

            if (wantTintBorder)
            {
                wux::Media::SolidColorBrush borderBrush;
                borderBrush.Color(ColorFromArgb(settings.borderArgb));
                slot.tip.BorderBrush(borderBrush);
                const wux::Thickness th =
                    ThicknessAnyPositive(slot.originalBorderThickness)
                    ? slot.originalBorderThickness
                    : wux::Thickness{ 1.0, 1.0, 1.0, 1.0 };
                slot.tip.BorderThickness(th);
                slot.chromePatched = true;
            }
            else if (settings.patchBorder || wantCorners)
            {
                slot.tip.BorderBrush(nullptr);
                slot.tip.BorderThickness(wux::Thickness{ 0.0, 0.0, 0.0, 0.0 });
                slot.chromePatched = true;
            }

            const bool firstApply = !slot.appliedOk;
            slot.appliedOk = true;
            slot.needsRetry = false;

            if (firstApply && slot.firstSeenTick != 0 && slot.appliedOk)
            {
                const ULONGLONG now = GetTickCount64();
                const unsigned delayMs = static_cast<unsigned>(
                    now >= slot.firstSeenTick ? (now - slot.firstSeenTick) : 0);
                DebugLog(
                    DBG_IMPORTANT,
                    L"[UICompat][ShellTooltip] applied delayMs=%u paintXaml=%u effect=%u "
                    L"corner=%u trayH=%.1f popupH=%.1f r=%.1f",
                    delayMs,
                    settings.paintPathXaml ? 1u : 0u,
                    settings.xamlEffectType,
                    settings.cornerType,
                    trayH,
                    popupH,
                    appliedR);
            }
        }
        catch (const winrt::hresult_error& ex)
        {
            slot.needsRetry = true;
            DebugLog(
                DBG_WARNING,
                L"[UICompat][ShellTooltip] ApplyPanelVisualsSync hr=0x%08X",
                static_cast<unsigned>(ex.code()));
        }
        catch (...)
        {
            slot.needsRetry = true;
        }
    }

    void ShellTooltipPanelRegistry::ApplyPanelGuarded(std::shared_ptr<PanelSlot> slot) noexcept
    {
        if (!slot || !slot->tip)
            return;
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;

        try
        {
            const auto dispatcher = slot->tip.Dispatcher();
            if (dispatcher && !dispatcher.HasThreadAccess())
            {
                const Registry::Settings settingsCopy = [this]() noexcept
                {
                    std::lock_guard lock(m_mx);
                    return m_settings;
                }();

                std::weak_ptr<PanelSlot> weakSlot = slot;
                std::weak_ptr<RegistryState> weakState = m_state;
                m_state->pendingAsync.fetch_add(1, std::memory_order_acq_rel);
                try
                {
                    (void)dispatcher.RunAsync(
                        winrt::Windows::UI::Core::CoreDispatcherPriority::High,
                        winrt::Windows::UI::Core::DispatchedHandler(
                            [weakSlot, weakState, settingsCopy]()
                            {
                                auto state = weakState.lock();
                                auto s = weakSlot.lock();
                                if (state)
                                {
                                    state->pendingAsync.fetch_sub(
                                        1, std::memory_order_acq_rel);
                                    if (state->shutdown.load(std::memory_order_acquire))
                                        return;
                                }
                                else
                                {
                                    return;
                                }
                                if (!s || !s->tip)
                                    return;

                                try
                                {
                                    if (!s->visualsResolved)
                                    {
                                        s->innerBorder = FindInnerBorder(s->tip, 0);
                                        if (!s->originalBackground)
                                            s->originalBackground = s->tip.Background();
                                        if (s->innerBorder && !s->originalInnerBackground)
                                            s->originalInnerBackground =
                                                s->innerBorder.Background();
                                        if (!s->chromeCaptured)
                                        {
                                            try
                                            {
                                                s->originalBorderBrush = s->tip.BorderBrush();
                                                s->originalBorderThickness =
                                                    s->tip.BorderThickness();
                                                const bool smeh =
                                                    Shared::RootBackdropPolicy::IsStartMenuHostProcess();
                                                if (smeh)
                                                {
                                                    s->originalCornerRadius = {};
                                                }
                                                else
                                                {
                                                    s->originalCornerRadius = s->tip.CornerRadius();
                                                }
                                                try
                                                {
                                                    s->originalShadow = s->tip.Shadow();
                                                }
                                                catch (...)
                                                {
                                                    s->originalShadow = nullptr;
                                                }
                                                s->chromeCaptured = true;
                                            }
                                            catch (...)
                                            {
                                                s->chromeCaptured = true;
                                            }
                                        }
                                        s->visualsResolved = true;
                                    }

                                    if (!settingsCopy.paintPathXaml)
                                    {
                                        if (s->tip && s->chromeCaptured && s->chromePatched)
                                        {
                                            const bool smeh =
                                                Shared::RootBackdropPolicy::IsStartMenuHostProcess();
                                            if (smeh)
                                            {
                                                s->tip.BorderBrush(s->originalBorderBrush);
                                                s->tip.BorderThickness(s->originalBorderThickness);
                                            }
                                            else
                                            {
                                                s->tip.BorderBrush(s->originalBorderBrush);
                                                s->tip.BorderThickness(s->originalBorderThickness);
                                                s->tip.CornerRadius(s->originalCornerRadius);
                                            }
                                            try
                                            {
                                                s->tip.Shadow(s->originalShadow);
                                            }
                                            catch (...)
                                            {
                                            }
                                            s->chromePatched = false;
                                        }
                                        if (s->innerBorder)
                                        {
                                            bool innerHad = false;
                                            RestoreBorderBackground(
                                                s->innerBorder,
                                                s->originalInnerBackground,
                                                innerHad,
                                                false);
                                        }
                                        if (s->tip)
                                        {
                                            bool had = false;
                                            RestoreControlBackground(
                                                s->tip, s->originalBackground, had, false);
                                        }
                                        s->needsRetry = false;
                                        s->appliedOk = true;
                                        return;
                                    }

                                    auto brush = CreateBackdropBrush(
                                        s->tip,
                                        settingsCopy.xamlEffectType,
                                        settingsCopy.tintArgb);
                                    if (!brush)
                                    {
                                        s->needsRetry = true;
                                        return;
                                    }
                                    s->tip.Background(brush);
                                    if (s->innerBorder)
                                    {
                                        winrt::Windows::UI::Color clear{};
                                        clear.A = 0;
                                        wux::Media::SolidColorBrush solid;
                                        solid.Color(clear);
                                        s->innerBorder.Background(solid);
                                    }
                                    try
                                    {
                                        s->tip.Shadow(nullptr);
                                    }
                                    catch (...)
                                    {
                                    }
                                    const bool wantCorners = settingsCopy.cornersPresent
                                        && settingsCopy.cornerType != 0;
                                    if (wantCorners)
                                    {
                                        if (!TipLayoutReady(s->tip))
                                        {
                                            s->needsRetry = true;
                                            s->appliedOk = false;
                                            return;
                                        }
                                        const double trayH =
                                            Shared::TrayXamlPaint::QueryTaskbarHeightDip();
                                        const double popupH = QueryVisibleHeightDip(s->tip);
                                        const double r =
                                            Shared::TrayXamlPaint::XamlPopupCornerRadiusDip(
                                                settingsCopy.cornerType, trayH, popupH);
                                        const wux::CornerRadius cr{ r, r, r, r };
                                        const bool smeh =
                                            Shared::RootBackdropPolicy::IsStartMenuHostProcess();
                                        if (smeh)
                                        {
                                            s->tipHandle = 0;
                                            s->innerHandle = 0;
                                        }
                                        else
                                        {
                                            s->tip.CornerRadius(cr);
                                            if (s->innerBorder)
                                                s->innerBorder.CornerRadius(cr);
                                        }
                                        if (settingsCopy.patchBorder
                                            && settingsCopy.borderArgb != 0)
                                        {
                                            wux::Media::SolidColorBrush borderBrush;
                                            borderBrush.Color(
                                                ColorFromArgb(settingsCopy.borderArgb));
                                            s->tip.BorderBrush(borderBrush);
                                            const wux::Thickness th =
                                                ThicknessAnyPositive(s->originalBorderThickness)
                                                ? s->originalBorderThickness
                                                : wux::Thickness{ 1.0, 1.0, 1.0, 1.0 };
                                            s->tip.BorderThickness(th);
                                        }
                                        else
                                        {
                                            s->tip.BorderBrush(nullptr);
                                            s->tip.BorderThickness(
                                                wux::Thickness{ 0.0, 0.0, 0.0, 0.0 });
                                        }
                                        s->chromePatched = true;
                                    }
                                    s->appliedOk = true;
                                    s->needsRetry = false;
                                }
                                catch (...)
                                {
                                    if (s)
                                        s->needsRetry = true;
                                }
                            }));
                }
                catch (...)
                {
                    m_state->pendingAsync.fetch_sub(1, std::memory_order_acq_rel);
                }
                return;
            }

            ApplyPanelVisualsSync(*slot);
        }
        catch (...)
        {
        }
    }

    void ShellTooltipPanelRegistry::RegisterPanel(
        InstanceHandle handle,
        wux::FrameworkElement root) noexcept
    {
        if (!root || handle == 0)
            return;
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;

        try
        {
            const auto tip = root.try_as<wux::Controls::ToolTip>();
            if (!tip)
                return;

            std::shared_ptr<PanelSlot> slot;
            {
                std::lock_guard lock(m_mx);
                auto& ref = m_panels[handle];
                if (!ref)
                    ref = std::make_shared<PanelSlot>();
                slot = ref;
                slot->tip = tip;
                if (slot->firstSeenTick == 0)
                    slot->firstSeenTick = GetTickCount64();
            }

            ApplyPanelVisualsSync(*slot);
            EnsureSizeHook(slot);
            if (slot->needsRetry)
                m_unresolved.store(1, std::memory_order_release);

            DebugLog(
                DBG_IMPORTANT,
                L"[UICompat][ShellTooltip] Panel registered handle=%llu paintXaml=%u "
                L"effect=%u corner=%u border=%u resolved=%u retry=%u",
                static_cast<unsigned long long>(handle),
                m_settings.paintPathXaml ? 1u : 0u,
                m_settings.xamlEffectType,
                m_settings.cornerType,
                m_settings.patchBorder ? 1u : 0u,
                slot->visualsResolved ? 1u : 0u,
                slot->needsRetry ? 1u : 0u);
        }
        catch (const winrt::hresult_error& ex)
        {
            DebugLog(
                DBG_WARNING,
                L"[UICompat][ShellTooltip] RegisterPanel hr=0x%08X msg=%s",
                static_cast<unsigned>(ex.code()),
                ex.message().c_str());
        }
        catch (...)
        {
            DebugLog(DBG_WARNING, L"[UICompat][ShellTooltip] RegisterPanel unknown exception");
        }
    }

    void ShellTooltipPanelRegistry::RetryUnresolvedPanels() noexcept
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
                if (s)
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
            ApplyPanelVisualsSync(*s);
            if (s->needsRetry || !s->appliedOk)
                ++still;
        }
        m_unresolved.store(still, std::memory_order_release);
    }

    void ShellTooltipPanelRegistry::Unregister(InstanceHandle handle) noexcept
    {
        std::shared_ptr<PanelSlot> slot;
        {
            std::lock_guard lock(m_mx);
            const auto it = m_panels.find(handle);
            if (it == m_panels.end())
                return;
            slot = it->second;
            m_panels.erase(it);
        }

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

    void ShellTooltipPanelRegistry::RestoreAll() noexcept
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
            L"[UICompat][ShellTooltip] RestoreAll total=%u ok=%u fail=%u noOrig=%u "
            L"disp=%u pendingAsync=%d",
            total,
            ok,
            fail,
            noOrig,
            viaDispatcher,
            pending);
        m_themePreferNeutralRestore.store(false, std::memory_order_release);
    }

    void ShellTooltipPanelRegistry::ReapplyPatchedBrushes() noexcept
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
                    snapshot.push_back(s);
            }
        }
        for (auto& s : snapshot)
            ApplyPanelGuarded(s);
    }

    void ShellTooltipPanelRegistry::RecaptureAfterThemeChange(Registry::Settings settings) noexcept
    {
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;
        {
            std::lock_guard lock(m_mx);
            m_settings = settings;
        }
        m_themePreferNeutralRestore.store(true, std::memory_order_release);

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
                if (slot->tip)
                    slot->tip.ClearValue(wux::Controls::Control::BackgroundProperty());
                if (slot->innerBorder)
                {
                    slot->innerBorder.ClearValue(wux::Controls::Border::BackgroundProperty());
                    slot->innerBorder.ClearValue(wux::Controls::Border::BorderBrushProperty());
                    slot->innerBorder.ClearValue(wux::Controls::Border::BorderThicknessProperty());
                    if (!Shared::RootBackdropPolicy::IsStartMenuHostProcess())
                        slot->innerBorder.ClearValue(wux::Controls::Border::CornerRadiusProperty());
                }
            }
            catch (...)
            {
            }
            slot->originalBackground = nullptr;
            slot->originalInnerBackground = nullptr;
            slot->originalBorderBrush = nullptr;
            slot->originalShadow = nullptr;
            slot->chromeCaptured = false;
            slot->chromePatched = false;
            slot->appliedOk = false;
            slot->needsRetry = true;
            slot->visualsResolved = false;
        }
        for (auto& s : snapshot)
            ApplyPanelGuarded(s);
    }

    void ShellTooltipPanelRegistry::UpdateSettings(Registry::Settings settings) noexcept
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
