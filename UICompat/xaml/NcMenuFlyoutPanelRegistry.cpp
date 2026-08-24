#include "UiPch.h"
#include "xaml/NcMenuFlyoutPanelRegistry.hpp"
#include "xaml/XamlDiagInit.hpp"
#include "xaml/brushes/XamlBackdropFactory.hpp"

#include <RootBackdropPolicy.h>
#include <TrayXamlPaint.h>

#include <DebugLog.h>

#include <cstdio>
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

        [[nodiscard]] bool BorderLayoutReady(wux::Controls::Border const& border) noexcept
        {
            if (!border)
                return false;
            try
            {
                return border.ActualWidth() >= 1.0 && border.ActualHeight() >= 1.0;
            }
            catch (...)
            {
                return false;
            }
        }

        // WinRT Border.CornerRadius get/set/ClearValue в SMEH → fail-fast (мимо catch).
        [[nodiscard]] bool AvoidWinrtBorderCornerRadius() noexcept
        {
            return Shared::RootBackdropPolicy::IsStartMenuHostProcess();
        }

        // Chrome MenuFlyout: первый Border. GetElementVisual только на нём (не на Presenter).
        [[nodiscard]] wux::Controls::Border FindChromeBorder(
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
            bool& hadOrig) noexcept
        {
            hadOrig = static_cast<bool>(originalBackground);
            try
            {
                ctrl.ClearValue(wux::Controls::Control::BackgroundProperty());
                if (hadOrig)
                    ctrl.Background(originalBackground);
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
                    L"[UICompat][MenuFlyout] PIN failed GLE=%u pendingAsync=%d",
                    static_cast<unsigned>(GetLastError()),
                    pending);
                return;
            }
            DebugLog(
                DBG_WARNING,
                L"[UICompat][MenuFlyout] module PIN (pendingAsync=%d)",
                pending);
        }

        [[nodiscard]] bool WantTintBorder(const Registry::Settings& settings) noexcept
        {
            // Tint не зависит от углов (паритет TaskbarCompat Menu/Tooltip).
            return settings.patchBorder && !settings.noBorder && settings.borderArgb != 0;
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

        // Win32-памп HWND надёжнее CoreDispatcher на Stop.
        // PopupHost (Xaml_WindowedPopupClass) → ControlCenter → CoreWindow.
        [[nodiscard]] HWND FindMenuFlyoutRestoreHwnd() noexcept
        {
            struct Ctx
            {
                DWORD pid{};
                DWORD preferTid{};
                HWND popupByTid{};
                HWND anyPopup{};
                HWND controlCenter{};
                HWND byShellTid{};
                HWND largeCore{};
                HWND anyCore{};
                LONG largeArea{};
            } ctx{ GetCurrentProcessId(), GetShellSurfaceThreadId(),
                   nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0 };

            EnumWindows(
                [](HWND hwnd, LPARAM lp) -> BOOL
                {
                    auto* c = reinterpret_cast<Ctx*>(lp);
                    DWORD pid = 0;
                    const DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
                    if (pid != c->pid)
                        return TRUE;
                    wchar_t cls[256]{};
                    if (GetClassNameW(hwnd, cls, 256) <= 0)
                        return TRUE;

                    if (_wcsicmp(cls, L"Xaml_WindowedPopupClass") == 0)
                    {
                        if (!c->anyPopup)
                            c->anyPopup = hwnd;
                        if (c->preferTid != 0 && tid == c->preferTid && !c->popupByTid)
                            c->popupByTid = hwnd;
                        return TRUE;
                    }
                    if (_wcsicmp(cls, L"ControlCenterWindow") == 0)
                    {
                        c->controlCenter = hwnd;
                        return TRUE;
                    }
                    if (_wcsicmp(cls, L"Windows.UI.Core.CoreWindow") != 0)
                        return TRUE;

                    if (!c->anyCore)
                        c->anyCore = hwnd;
                    if (c->preferTid != 0 && tid == c->preferTid && !c->byShellTid)
                        c->byShellTid = hwnd;

                    RECT rc{};
                    if (GetWindowRect(hwnd, &rc))
                    {
                        const LONG w = rc.right - rc.left;
                        const LONG h = rc.bottom - rc.top;
                        const LONG area = (w > 0 && h > 0) ? (w * h) : 0;
                        // Start CoreWindow и NC ≥600; fallback когда popup уже уничтожен.
                        if (h >= 600 && area > c->largeArea)
                        {
                            c->largeArea = area;
                            c->largeCore = hwnd;
                        }
                    }
                    return TRUE;
                },
                reinterpret_cast<LPARAM>(&ctx));

            if (ctx.popupByTid)
                return ctx.popupByTid;
            if (ctx.anyPopup)
                return ctx.anyPopup;
            if (ctx.controlCenter)
                return ctx.controlCenter;
            if (ctx.byShellTid)
                return ctx.byShellTid;
            if (ctx.largeCore)
                return ctx.largeCore;
            return ctx.anyCore;
        }
    }

    NcMenuFlyoutPanelRegistry::NcMenuFlyoutPanelRegistry(
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

    unsigned NcMenuFlyoutPanelRegistry::PanelCount() const noexcept
    {
        std::lock_guard lock(m_mx);
        return static_cast<unsigned>(m_panels.size());
    }

    unsigned NcMenuFlyoutPanelRegistry::UnresolvedCount() const noexcept
    {
        return m_unresolved.load(std::memory_order_acquire);
    }

    bool NcMenuFlyoutPanelRegistry::EnsureChromeHandle(PanelSlot& slot) noexcept
    {
        if (slot.chromeHandle != 0)
            return true;
        if (!slot.chromeBorder || !m_diagnostics)
            return false;
        try
        {
            ::IInspectable* raw = DetachInspectable(
                slot.chromeBorder.as<winrt::Windows::Foundation::IInspectable>());
            if (!raw)
                return false;
            InstanceHandle h = 0;
            const HRESULT hr = m_diagnostics->GetHandleFromIInspectable(raw, &h);
            raw->Release();
            if (FAILED(hr) || h == 0)
                return false;
            slot.chromeHandle = h;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool NcMenuFlyoutPanelRegistry::EnsurePresenterHandle(PanelSlot& slot) noexcept
    {
        if (slot.presenterHandle != 0)
            return true;
        if (!slot.root || !m_diagnostics)
            return false;
        try
        {
            ::IInspectable* raw = DetachInspectable(
                slot.root.as<winrt::Windows::Foundation::IInspectable>());
            if (!raw)
                return false;
            InstanceHandle h = 0;
            const HRESULT hr = m_diagnostics->GetHandleFromIInspectable(raw, &h);
            raw->Release();
            if (FAILED(hr) || h == 0)
                return false;
            slot.presenterHandle = h;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool NcMenuFlyoutPanelRegistry::ClearPropByIndex(
        InstanceHandle element,
        unsigned propIndex) noexcept
    {
        if (!m_vts3 || element == 0 || propIndex == UINT_MAX)
            return false;
        // Windhawk restore: ClearValue когда local override нужно снять к theme.
        return SUCCEEDED(m_vts3->ClearProperty(element, propIndex));
    }

    bool NcMenuFlyoutPanelRegistry::CaptureCornerVts3(PanelSlot& slot) noexcept
    {
        if (!m_diagnostics || !m_vts3)
            return false;
        if (!EnsureChromeHandle(slot))
            return false;
        if (slot.cornerPropIndex != UINT_MAX && slot.originalCornerVts3)
            return true;

        unsigned sourceCount = 0;
        unsigned propertyCount = 0;
        PropertyChainSource* sources = nullptr;
        PropertyChainValue* properties = nullptr;
        const HRESULT chainHr = m_vts3->GetPropertyValuesChain(
            slot.chromeHandle,
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
                slot.chromeHandle, propertyIndex, &propertyHandle);
        if (SUCCEEDED(propertyHr) && propertyHandle != 0)
        {
            propertyHr = m_diagnostics->GetIInspectableFromHandle(
                propertyHandle, &inspectable);
        }

        ReleasePropertyChain(sources, sourceCount, properties, propertyCount);

        if (SUCCEEDED(propertyHr) && inspectable)
        {
            slot.cornerPropIndex = propertyIndex;
            slot.originalCornerVts3.attach(inspectable);
            return true;
        }
        if (inspectable)
            inspectable->Release();
        return false;
    }

    bool NcMenuFlyoutPanelRegistry::CaptureBackgroundVts3(PanelSlot& slot) noexcept
    {
        if (!m_diagnostics || !m_vts3)
            return false;
        if (!EnsureChromeHandle(slot))
            return false;
        if (slot.borderBackgroundPropIndex != UINT_MAX && slot.originalBorderBackgroundVts3)
            return true;

        unsigned sourceCount = 0;
        unsigned propertyCount = 0;
        PropertyChainSource* sources = nullptr;
        PropertyChainValue* properties = nullptr;
        const HRESULT chainHr = m_vts3->GetPropertyValuesChain(
            slot.chromeHandle,
            &sourceCount,
            &sources,
            &propertyCount,
            &properties);

        unsigned propertyIndex = slot.borderBackgroundPropIndex;
        if (propertyIndex == UINT_MAX && SUCCEEDED(chainHr) && properties)
        {
            for (unsigned p = 0; p < propertyCount; ++p)
            {
                if (properties[p].PropertyName
                    && wcscmp(properties[p].PropertyName, L"Background") == 0)
                {
                    propertyIndex = properties[p].Index;
                    break;
                }
            }
        }
        if (propertyIndex != UINT_MAX)
            slot.borderBackgroundPropIndex = propertyIndex;

        InstanceHandle propertyHandle = 0;
        ::IInspectable* inspectable = nullptr;
        HRESULT propertyHr = (propertyIndex != UINT_MAX) ? S_OK : E_FAIL;
        if (SUCCEEDED(propertyHr))
            propertyHr = m_vts3->GetProperty(
                slot.chromeHandle, propertyIndex, &propertyHandle);
        if (SUCCEEDED(propertyHr) && propertyHandle != 0)
        {
            propertyHr = m_diagnostics->GetIInspectableFromHandle(
                propertyHandle, &inspectable);
        }

        ReleasePropertyChain(sources, sourceCount, properties, propertyCount);

        if (SUCCEEDED(propertyHr) && inspectable)
        {
            slot.originalBorderBackgroundVts3.attach(inspectable);
            return true;
        }
        if (inspectable)
            inspectable->Release();
        return false;
    }

    bool NcMenuFlyoutPanelRegistry::RestoreBackgroundVts3(PanelSlot& slot) noexcept
    {
        if (!m_diagnostics || !m_vts3)
            return false;
        if (slot.borderBackgroundPropIndex == UINT_MAX || !slot.originalBorderBackgroundVts3)
            return false;

        // Unregister: старый chromeHandle часто мёртв — как у CornerRadius.
        slot.chromeHandle = 0;
        if (!EnsureChromeHandle(slot))
            return false;

        InstanceHandle valueHandle = 0;
        const HRESULT hrHandle = m_diagnostics->GetHandleFromIInspectable(
            slot.originalBorderBackgroundVts3.get(), &valueHandle);
        HRESULT hrSet = E_FAIL;
        if (SUCCEEDED(hrHandle) && valueHandle != 0)
        {
            hrSet = m_vts3->SetProperty(
                slot.chromeHandle, valueHandle, slot.borderBackgroundPropIndex);
        }
        return SUCCEEDED(hrSet);
    }

    bool NcMenuFlyoutPanelRegistry::SetCornerVts3(
        PanelSlot& slot,
        wux::CornerRadius const& cr) noexcept
    {
        if (!m_diagnostics || !m_vts3)
            return false;
        if (!EnsureChromeHandle(slot))
            return false;
        if (slot.cornerPropIndex == UINT_MAX && !CaptureCornerVts3(slot))
            return false;
        if (slot.cornerPropIndex == UINT_MAX)
            return false;

        ::IInspectable* crInsp = DetachInspectable(winrt::box_value(cr));
        if (!crInsp)
            return false;
        InstanceHandle valueHandle = 0;
        const HRESULT hrHandle =
            m_diagnostics->GetHandleFromIInspectable(crInsp, &valueHandle);
        const HRESULT hrSet =
            (SUCCEEDED(hrHandle) && valueHandle != 0)
            ? m_vts3->SetProperty(slot.chromeHandle, valueHandle, slot.cornerPropIndex)
            : E_FAIL;
        crInsp->Release();
        return SUCCEEDED(hrSet);
    }

    bool NcMenuFlyoutPanelRegistry::CapturePresenterCornerVts3(PanelSlot& slot) noexcept
    {
        if (!m_diagnostics || !m_vts3)
            return false;
        if (!EnsurePresenterHandle(slot))
            return false;
        if (slot.presenterCornerPropIndex != UINT_MAX && slot.originalPresenterCornerVts3)
            return true;

        unsigned sourceCount = 0;
        unsigned propertyCount = 0;
        PropertyChainSource* sources = nullptr;
        PropertyChainValue* properties = nullptr;
        const HRESULT chainHr = m_vts3->GetPropertyValuesChain(
            slot.presenterHandle,
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
                slot.presenterHandle, propertyIndex, &propertyHandle);
        if (SUCCEEDED(propertyHr) && propertyHandle != 0)
        {
            propertyHr = m_diagnostics->GetIInspectableFromHandle(
                propertyHandle, &inspectable);
        }

        ReleasePropertyChain(sources, sourceCount, properties, propertyCount);

        if (SUCCEEDED(propertyHr) && inspectable)
        {
            slot.presenterCornerPropIndex = propertyIndex;
            slot.originalPresenterCornerVts3.attach(inspectable);
            slot.presenterCornerCaptured = true;
            return true;
        }
        if (inspectable)
            inspectable->Release();
        return false;
    }

    bool NcMenuFlyoutPanelRegistry::SetPresenterCornerVts3(
        PanelSlot& slot,
        wux::CornerRadius const& cr) noexcept
    {
        if (!m_diagnostics || !m_vts3)
            return false;
        if (!EnsurePresenterHandle(slot))
            return false;
        if (slot.presenterCornerPropIndex == UINT_MAX && !CapturePresenterCornerVts3(slot))
            return false;
        if (slot.presenterCornerPropIndex == UINT_MAX)
            return false;

        ::IInspectable* crInsp = DetachInspectable(winrt::box_value(cr));
        if (!crInsp)
            return false;
        InstanceHandle valueHandle = 0;
        const HRESULT hrHandle =
            m_diagnostics->GetHandleFromIInspectable(crInsp, &valueHandle);
        const HRESULT hrSet =
            (SUCCEEDED(hrHandle) && valueHandle != 0)
            ? m_vts3->SetProperty(
                slot.presenterHandle, valueHandle, slot.presenterCornerPropIndex)
            : E_FAIL;
        crInsp->Release();
        if (SUCCEEDED(hrSet))
            slot.presenterCornerPatched = true;
        return SUCCEEDED(hrSet);
    }

    bool NcMenuFlyoutPanelRegistry::RestorePresenterCornerVts3(PanelSlot& slot) noexcept
    {
        if (!m_diagnostics || !m_vts3)
            return false;
        if (slot.presenterHandle == 0 || slot.presenterCornerPropIndex == UINT_MAX)
            return false;

        if (slot.originalPresenterCornerVts3)
        {
            InstanceHandle valueHandle = 0;
            const HRESULT hrHandle = m_diagnostics->GetHandleFromIInspectable(
                slot.originalPresenterCornerVts3.get(), &valueHandle);
            if (SUCCEEDED(hrHandle) && valueHandle != 0
                && SUCCEEDED(m_vts3->SetProperty(
                    slot.presenterHandle, valueHandle, slot.presenterCornerPropIndex)))
            {
                return true;
            }
        }
        // Windhawk: ClearValue → theme default, если original SetProperty не встаёт.
        return ClearPropByIndex(slot.presenterHandle, slot.presenterCornerPropIndex);
    }

    bool NcMenuFlyoutPanelRegistry::RestoreCornerVts3(PanelSlot& slot) noexcept
    {
        if (!m_diagnostics || !m_vts3)
            return false;
        if (slot.chromeHandle == 0 || slot.cornerPropIndex == UINT_MAX)
            return false;

        if (slot.originalCornerVts3)
        {
            InstanceHandle valueHandle = 0;
            const HRESULT hrHandle = m_diagnostics->GetHandleFromIInspectable(
                slot.originalCornerVts3.get(), &valueHandle);
            if (SUCCEEDED(hrHandle) && valueHandle != 0
                && SUCCEEDED(m_vts3->SetProperty(
                    slot.chromeHandle, valueHandle, slot.cornerPropIndex)))
            {
                return true;
            }
        }
        // Донор Windhawk: ClearValue снимает local override → theme.
        return ClearPropByIndex(slot.chromeHandle, slot.cornerPropIndex);
    }

    bool NcMenuFlyoutPanelRegistry::ResolveVisuals(PanelSlot& slot) noexcept
    {
        if (!slot.root)
            return false;

        try
        {
            const auto found = FindChromeBorder(slot.root, 0);
            if (!found)
                return false;

            const bool borderChanged =
                !slot.chromeBorder
                || (winrt::get_abi(slot.chromeBorder) != winrt::get_abi(found));
            slot.chromeBorder = found;

            if (const auto ctrl = slot.root.try_as<wux::Controls::Control>())
            {
                if (!slot.clearBackgroundOnRestore
                    && !slot.originalPresenterBackground
                    && !slot.backgroundPatched)
                    slot.originalPresenterBackground = ctrl.Background();
            }

            if (borderChanged)
            {
                // Уже патчили фон — не перехватывать Background (иначе original = наш acrylic).
                slot.chromeCaptured = false;
                slot.appliedOk = false;
                slot.needsRetry = true;
                slot.ResetCornerCapture();
                if (!slot.clearBackgroundOnRestore
                    && !slot.chromePatched
                    && !slot.backgroundPatched)
                {
                    slot.originalBorderBackground = nullptr;
                    slot.borderBackgroundPropIndex = UINT_MAX;
                    slot.originalBorderBackgroundVts3 = nullptr;
                    try
                    {
                        slot.originalBorderBackground = slot.chromeBorder.Background();
                    }
                    catch (...)
                    {
                    }
                }
            }
            else if (
                !slot.clearBackgroundOnRestore
                && !slot.originalBorderBackground
                && !slot.chromePatched
                && !slot.backgroundPatched)
            {
                try
                {
                    slot.originalBorderBackground = slot.chromeBorder.Background();
                }
                catch (...)
                {
                }
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

    void NcMenuFlyoutPanelRegistry::CaptureChrome(PanelSlot& slot) noexcept
    {
        if (!slot.chromeBorder || slot.chromeCaptured)
            return;
        try
        {
            slot.originalBorderBrush = slot.chromeBorder.BorderBrush();
            slot.originalBorderThickness = slot.chromeBorder.BorderThickness();
            if (AvoidWinrtBorderCornerRadius())
            {
                // WinRT CornerRadius() GET в SMEH валит процесс — только VTS3.
                (void)CaptureCornerVts3(slot);
                (void)CapturePresenterCornerVts3(slot);
                // Background() часто null — acrylic только через VTS3 (как ShellSurface).
                if (!slot.clearBackgroundOnRestore
                    && !slot.chromePatched
                    && !slot.originalBorderBackgroundVts3)
                    (void)CaptureBackgroundVts3(slot);
                slot.originalCornerRadius = {};
            }
            else
            {
                slot.originalCornerRadius = slot.chromeBorder.CornerRadius();
            }
            try
            {
                slot.originalShadow = slot.chromeBorder.Shadow();
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

    void NcMenuFlyoutPanelRegistry::EnsureSizeHook(std::shared_ptr<PanelSlot> slot) noexcept
    {
        if (!slot || !slot->chromeBorder || slot->sizeHooked)
            return;
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;
        if (BorderLayoutReady(slot->chromeBorder) && slot->appliedOk && !slot->needsRetry)
            return;

        try
        {
            std::weak_ptr<PanelSlot> weakSlot = slot;
            std::weak_ptr<RegistryState> weakState = m_state;
            NcMenuFlyoutPanelRegistry* self = this;
            // Нельзя обнулять sizeRevoker внутри SizeChanged — CRT fail-fast.
            slot->sizeRevoker = slot->chromeBorder.SizeChanged(
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
                        if (!s || !s->chromeBorder)
                            return;
                        if (s->sizeApplyBusy || s->sizeApplyDone)
                            return;
                        if (!BorderLayoutReady(s->chromeBorder))
                            return;
                        s->sizeApplyBusy = true;
                        self->ApplyPanelVisualsSync(*s);
                        // Не оставлять needsRetry=true: иначе RetryUnresolved крутит SetCorner вечно.
                        // SMEH: sizeApplyDone только после capture+patch Background.
                        if (s->appliedOk && BorderLayoutReady(s->chromeBorder)
                            && s->backgroundPatched)
                        {
                            s->needsRetry = false;
                            s->sizeApplyDone = true;
                        }
                        else if (s->appliedOk && !s->backgroundPatched)
                        {
                            s->needsRetry = true;
                        }
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

    void NcMenuFlyoutPanelRegistry::RestoreChrome(PanelSlot& slot) noexcept
    {
        if (!slot.chromeBorder || !slot.chromeCaptured || !slot.chromePatched)
        {
            slot.chromePatched = false;
            return;
        }
        try
        {
            if (AvoidWinrtBorderCornerRadius())
            {
                // SoftClear BorderBrush/Thickness/CR → прямые углы без бордюра
                // (local values, как acrylic Background). Вернуть captured originals.
                try
                {
                    slot.chromeBorder.BorderBrush(slot.originalBorderBrush);
                    slot.chromeBorder.BorderThickness(slot.originalBorderThickness);
                }
                catch (...)
                {
                }
                // CR: живой handle + SetProperty(original); ClearProp только fallback.
                slot.chromeHandle = 0;
                if (EnsureChromeHandle(slot))
                    (void)RestoreCornerVts3(slot);
            }
            else
            {
                slot.chromeBorder.BorderBrush(slot.originalBorderBrush);
                slot.chromeBorder.BorderThickness(slot.originalBorderThickness);
                slot.chromeBorder.CornerRadius(slot.originalCornerRadius);
            }
            try
            {
                slot.chromeBorder.Shadow(slot.originalShadow);
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

    void NcMenuFlyoutPanelRegistry::RestorePresenterCorner(PanelSlot& slot) noexcept
    {
        if (!slot.presenterCornerPatched)
            return;
        if (AvoidWinrtBorderCornerRadius())
        {
            // SetProperty(original) после refresh; ClearProp — только внутри fallback.
            slot.presenterHandle = 0;
            if (EnsurePresenterHandle(slot))
                (void)RestorePresenterCornerVts3(slot);
            slot.presenterCornerPatched = false;
            return;
        }
        if (const auto ctrl = slot.root.try_as<wux::Controls::Control>())
        {
            try
            {
                ctrl.ClearValue(wux::Controls::Control::CornerRadiusProperty());
                if (slot.presenterCornerCaptured)
                    ctrl.CornerRadius(slot.originalPresenterCornerRadius);
            }
            catch (...)
            {
            }
        }
        slot.presenterCornerPatched = false;
    }

    void NcMenuFlyoutPanelRegistry::RestorePanelVisuals(
        PanelSlot& slot,
        bool restoreBackground,
        bool& hadOrig) noexcept
    {
        hadOrig = false;
        // SizeChanged до restore chrome — иначе хук может переписать углы.
        slot.sizeRevoker = {};
        slot.sizeHooked = false;

        const bool smehSoft = AvoidWinrtBorderCornerRadius();

        RestoreChrome(slot);
        RestorePresenterCorner(slot);

        // Presenter Background тоже только при backgroundPatched: иначе ClearValue
        // на непатченном/угол-only слоте → прозрачные меню NC/QS после Stop.
        if (restoreBackground && slot.backgroundPatched)
        {
            const bool preferNeutral =
                slot.clearBackgroundOnRestore
                || m_themePreferNeutralRestore.load(std::memory_order_acquire);
            bool restoredBg = false;

            // ClearValue всегда; Neutral только после theme epoch
            // (Neutral до theme липнет со старым tint).
            if (slot.chromeBorder)
            {
                try
                {
                    // SMEH: при hide (не Stop/DWM) — SetProperty(captured acrylic),
                    // не ClearValue → прозрачность. ClearValue оставить для Stop/DWM/theme Neutral.
                    const bool isHide = !preferNeutral && smehSoft && slot.originalBorderBackgroundVts3;
                    if (isHide)
                    {
                        restoredBg = RestoreBackgroundVts3(slot);
                        hadOrig = restoredBg;
                    }
                    else
                    {
                        slot.chromeBorder.ClearValue(
                            wux::Controls::Border::BackgroundProperty());
                        restoredBg = true;
                        if (preferNeutral)
                        {
                            if (const auto neut = CreateNeutralAcrylicBackdropBrush())
                                slot.chromeBorder.Background(neut);
                        }
                    }
                }
                catch (...)
                {
                }
            }
            if (const auto ctrl = slot.root.try_as<wux::Controls::Control>())
            {
                bool presenterHad = false;
                try
                {
                    ctrl.ClearValue(wux::Controls::Control::BackgroundProperty());
                    if (preferNeutral)
                    {
                        if (const auto neut = CreateNeutralAcrylicBackdropBrush())
                        {
                            ctrl.Background(neut);
                            presenterHad = true;
                        }
                    }
                }
                catch (...)
                {
                }
                hadOrig = hadOrig || presenterHad;
                restoredBg = restoredBg || presenterHad;
            }
            (void)restoredBg;
            slot.backgroundPatched = false;
        }
    }

    void NcMenuFlyoutPanelRegistry::RestoreMarshalThunk(void* param) noexcept
    {
        auto* j = static_cast<MarshalRestoreJob*>(param);
        if (!j || !j->self || !j->slot)
            return;
        try
        {
            j->self->RestorePanelVisuals(*j->slot, j->restoreBackground, j->hadOrig);
            j->slot->ClearWinrtRefs();
        }
        catch (const winrt::hresult_error& ex)
        {
            j->hr = ex.code();
        }
        catch (...)
        {
            j->hr = E_FAIL;
        }
    }

    HRESULT NcMenuFlyoutPanelRegistry::RestoreSlotOnUiThread(
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
            if (!slot->root && !slot->chromeBorder)
                return S_FALSE;

            wux::FrameworkElement fe = slot->root ? slot->root
                : slot->chromeBorder.as<wux::FrameworkElement>();
            if (!fe)
                return S_FALSE;

            const auto dispatcher = fe.Dispatcher();
            if (dispatcher && dispatcher.HasThreadAccess())
            {
                RestorePanelVisuals(*slot, restoreBackground, hadOrig);
                slot->ClearWinrtRefs();
                return S_OK;
            }

            // PopupHost / Start|NC CoreWindow — надёжнее CoreDispatcher на Stop.
            if (const HWND hwnd = FindMenuFlyoutRestoreHwnd())
            {
                MarshalRestoreJob job{};
                job.self = this;
                job.slot = slot;
                job.restoreBackground = restoreBackground;
                const bool marshaled = RunFromWindowThread(
                    hwnd,
                    &NcMenuFlyoutPanelRegistry::RestoreMarshalThunk,
                    &job);
                if (marshaled)
                {
                    usedDispatcher = true;
                    hadOrig = job.hadOrig;
                    DebugLog(
                        DBG_IMPORTANT,
                        L"[UICompat][MenuFlyout] Restore via RunFromWindowThread hwnd=%p hr=0x%08X",
                        reinterpret_cast<void*>(hwnd),
                        static_cast<unsigned>(job.hr));
                    return job.hr;
                }
            }

            if (dispatcher)
            {
                usedDispatcher = true;
                const HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                if (!done)
                    return HRESULT_FROM_WIN32(GetLastError());

                struct Job
                {
                    MarshalRestoreJob restore{};
                    std::shared_ptr<RegistryState> state;
                    HANDLE done{};
                    bool ownsEvent{ true };

                    ~Job() noexcept
                    {
                        if (ownsEvent && done)
                            CloseHandle(done);
                    }
                };

                auto job = std::make_shared<Job>();
                job->restore.self = this;
                job->restore.slot = std::move(slot);
                job->restore.restoreBackground = restoreBackground;
                job->state = m_state;
                job->done = done;

                m_state->pendingAsync.fetch_add(1, std::memory_order_acq_rel);
                try
                {
                    (void)dispatcher.RunAsync(
                        winrt::Windows::UI::Core::CoreDispatcherPriority::High,
                        winrt::Windows::UI::Core::DispatchedHandler(
                            [job]()
                            {
                                RestoreMarshalThunk(&job->restore);
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
                    hadOrig = job->restore.hadOrig;
                    const HRESULT hr = job->restore.hr;
                    job->ownsEvent = false;
                    CloseHandle(done);
                    return hr;
                }

                DebugLog(
                    DBG_WARNING,
                    L"[UICompat][MenuFlyout] Restore wait timeout (%u ms)",
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

    void NcMenuFlyoutPanelRegistry::ApplyPanelVisualsSync(PanelSlot& slot) noexcept
    {
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;
        if (!slot.root)
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
        if (!slot.chromeBorder)
        {
            slot.needsRetry = true;
            return;
        }

        CaptureChrome(slot);

        if (!settings.paintPathXaml)
        {
            // Restore только если уже патчили — иначе ClearValue → прозрачность.
            if (slot.backgroundPatched || slot.chromePatched || slot.presenterCornerPatched)
            {
                bool hadOrig = false;
                RestorePanelVisuals(slot, true, hadOrig);
            }
            slot.needsRetry = false;
            slot.appliedOk = true;
            return;
        }

        // Blur/compositor — только на Border (GetElementVisual на Presenter ломает клики).
        // Меню: полный наш CreateBackdropBrush (Tray\Xaml EffectType), без donor-remap.
        const DWORD effect = settings.xamlEffectType;
        wux::Media::Brush brush = CreateBackdropBrush(
            slot.chromeBorder,
            effect,
            settings.tintArgb);

        if (!brush)
        {
            slot.needsRetry = true;
            return;
        }

        try
        {
            const bool avoidWinrtCr = AvoidWinrtBorderCornerRadius();

            // SMEH: Background local появляется с задержкой. После theme —
            // не захватывать stale как original; патчить BG и на Stop/Unregister → Neutral.
            const bool themeNeutral =
                slot.clearBackgroundOnRestore
                || m_themePreferNeutralRestore.load(std::memory_order_acquire);
            if (avoidWinrtCr && !slot.backgroundPatched && !themeNeutral)
            {
                if (!slot.originalBorderBackgroundVts3)
                    (void)CaptureBackgroundVts3(slot);
                if (!slot.originalBorderBackground)
                {
                    try
                    {
                        slot.originalBorderBackground = slot.chromeBorder.Background();
                    }
                    catch (...)
                    {
                    }
                }
            }

            // Всегда apply кисть на chrome Border (как TrayMenuFlyout), не ждать haveBgOrig.
            // Original capture для Stop, но не гейтит apply (fix reshow прозрачности).
            if (!slot.backgroundPatched)
            {
                if (themeNeutral)
                {
                    slot.originalBorderBackground = nullptr;
                    slot.originalBorderBackgroundVts3 = nullptr;
                    slot.originalPresenterBackground = nullptr;
                    slot.clearBackgroundOnRestore = true;
                }
                slot.chromeBorder.Background(brush);
                slot.backgroundPatched = true;

                if (const auto ctrl = slot.root.try_as<wux::Controls::Control>())
                {
                    winrt::Windows::UI::Color clear{};
                    clear.A = 0;
                    wux::Media::SolidColorBrush solid;
                    solid.Color(clear);
                    ctrl.Background(solid);
                }
            }

            // Уже ставили углы — только ждали BG: не повторять SetCorner VTS3.
            if (slot.chromePatched && slot.presenterCornerPatched)
            {
                if (avoidWinrtCr && !slot.backgroundPatched)
                    slot.needsRetry = true;
                else if (slot.backgroundPatched && BorderLayoutReady(slot.chromeBorder))
                    slot.needsRetry = false;
                slot.appliedOk = true;
                return;
            }

            try
            {
                slot.chromeBorder.Shadow(nullptr);
            }
            catch (...)
            {
            }

            const bool wantCorners =
                settings.cornersPresent && settings.cornerType != 0;
            const double trayH = Shared::TrayXamlPaint::QueryTaskbarHeightDip();
            const double popupH = QueryVisibleHeightDip(slot.chromeBorder);
            double appliedR = 0.0;
            const bool layoutReady = BorderLayoutReady(slot.chromeBorder);

            // Углы/бордюр или фон — для DWM/Stop restore.
            slot.chromePatched = true;

            if (wantCorners)
            {
                appliedR = Shared::TrayXamlPaint::XamlPopupCornerRadiusDip(
                    settings.cornerType, trayH, popupH);
                const wux::CornerRadius cr{ appliedR, appliedR, appliedR, appliedR };

                if (avoidWinrtCr)
                {
                    // Border + Presenter CR через VTS3 (TrayMenuFlyout / Windhawk clip).
                    if (!SetCornerVts3(slot, cr))
                        slot.needsRetry = true;
                    if (!SetPresenterCornerVts3(slot, cr))
                        slot.needsRetry = true;
                }
                else
                {
                    slot.chromeBorder.CornerRadius(cr);

                    if (const auto ctrl = slot.root.try_as<wux::Controls::Control>())
                    {
                        try
                        {
                            if (!slot.presenterCornerCaptured)
                            {
                                slot.originalPresenterCornerRadius = ctrl.CornerRadius();
                                slot.presenterCornerCaptured = true;
                            }
                            ctrl.CornerRadius(cr);
                            slot.presenterCornerPatched = true;
                        }
                        catch (...)
                        {
                        }
                    }
                }
            }
            else
            {
                // Без углов — откатить CR; бордюр ниже (tint независимо).
                try
                {
                    if (slot.chromeCaptured)
                    {
                        if (avoidWinrtCr)
                            (void)RestoreCornerVts3(slot);
                        else
                            slot.chromeBorder.CornerRadius(slot.originalCornerRadius);
                    }
                }
                catch (...)
                {
                }
                RestorePresenterCorner(slot);
            }

            if (WantTintBorder(settings))
            {
                wux::Media::SolidColorBrush borderBrush;
                borderBrush.Color(ColorFromArgb(settings.borderArgb));
                slot.chromeBorder.BorderBrush(borderBrush);
                const wux::Thickness th =
                    ThicknessAnyPositive(slot.originalBorderThickness)
                    ? slot.originalBorderThickness
                    : wux::Thickness{ 1.0, 1.0, 1.0, 1.0 };
                slot.chromeBorder.BorderThickness(th);
            }
            else if (settings.patchBorder || settings.noBorder || wantCorners)
            {
                slot.chromeBorder.BorderBrush(nullptr);
                slot.chromeBorder.BorderThickness(wux::Thickness{ 0.0, 0.0, 0.0, 0.0 });
            }
            else if (slot.chromeCaptured)
            {
                try
                {
                    slot.chromeBorder.BorderBrush(slot.originalBorderBrush);
                    slot.chromeBorder.BorderThickness(slot.originalBorderThickness);
                }
                catch (...)
                {
                }
            }

            const bool firstApply = !slot.appliedOk;
            slot.appliedOk = true;
            // SMEH: держать retry пока Background не захвачен/не патчен.
            // Иначе layoutReady сбрасывал needsRetry → apply без original → прозрачность.
            if (avoidWinrtCr && !slot.backgroundPatched)
                slot.needsRetry = true;
            else if (!slot.needsRetry)
                slot.needsRetry = wantCorners && !layoutReady;
            else if (layoutReady)
                slot.needsRetry = false;

            if (firstApply && slot.firstSeenTick != 0 && slot.appliedOk)
            {
                const unsigned delayMs = static_cast<unsigned>(
                    GetTickCount64() - slot.firstSeenTick);
                DebugLog(
                    DBG_IMPORTANT,
                    L"[UICompat][MenuFlyout] applied delayMs=%u paintXaml=%u effect=%u "
                    L"corner=%u trayH=%.1f popupH=%.1f r=%.1f layout=%u start=%u",
                    delayMs,
                    settings.paintPathXaml ? 1u : 0u,
                    settings.xamlEffectType,
                    settings.cornerType,
                    trayH,
                    popupH,
                    appliedR,
                    layoutReady ? 1u : 0u,
                    avoidWinrtCr ? 1u : 0u);
            }
        }
        catch (const winrt::hresult_error& ex)
        {
            slot.needsRetry = true;
            DebugLog(
                DBG_WARNING,
                L"[UICompat][MenuFlyout] ApplyPanelVisualsSync hr=0x%08X",
                static_cast<unsigned>(ex.code()));
        }
        catch (...)
        {
            slot.needsRetry = true;
        }
    }

    void NcMenuFlyoutPanelRegistry::ApplyPanelGuarded(std::shared_ptr<PanelSlot> slot) noexcept
    {
        if (!slot || !slot->root)
            return;
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;

        try
        {
            const auto dispatcher = slot->root.Dispatcher();
            if (dispatcher && !dispatcher.HasThreadAccess())
            {
                std::weak_ptr<PanelSlot> weakSlot = slot;
                std::weak_ptr<RegistryState> weakState = m_state;
                NcMenuFlyoutPanelRegistry* self = this;
                m_state->pendingAsync.fetch_add(1, std::memory_order_acq_rel);
                try
                {
                    (void)dispatcher.RunAsync(
                        winrt::Windows::UI::Core::CoreDispatcherPriority::High,
                        winrt::Windows::UI::Core::DispatchedHandler(
                            [weakSlot, weakState, self]()
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
                                if (!self || !s || !s->root)
                                    return;
                                self->ApplyPanelVisualsSync(*s);
                                self->EnsureSizeHook(s);
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

    void NcMenuFlyoutPanelRegistry::RegisterPanel(
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
            {
                std::lock_guard lock(m_mx);
                auto& ref = m_panels[handle];
                const bool isReuse = static_cast<bool>(ref);
                if (!ref)
                    ref = std::make_shared<PanelSlot>();
                slot = ref;
                slot->root = root;
                if (slot->firstSeenTick == 0)
                    slot->firstSeenTick = GetTickCount64();
                if (m_themePreferNeutralRestore.load(std::memory_order_acquire))
                    slot->clearBackgroundOnRestore = true;

                // Reuse слота (повторный показ меню без Remove): reset патч-флагов,
                // чтобы apply заново выставил кисть (fix прозрачность со 2-го показа).
                if (isReuse && (slot->sizeApplyDone || slot->backgroundPatched))
                {
                    slot->sizeApplyDone = false;
                    slot->backgroundPatched = false;
                    slot->chromePatched = false;
                    slot->presenterCornerPatched = false;
                    slot->appliedOk = false;
                    slot->needsRetry = true;
                }
            }

            ApplyPanelVisualsSync(*slot);
            EnsureSizeHook(slot);
            if (slot->needsRetry)
                m_unresolved.store(1, std::memory_order_release);

            DebugLog(
                DBG_IMPORTANT,
                L"[UICompat][MenuFlyout] Panel registered handle=%llu paintXaml=%u "
                L"effect=%u corner=%u patchBorder=%u resolved=%u retry=%u start=%u",
                static_cast<unsigned long long>(handle),
                m_settings.paintPathXaml ? 1u : 0u,
                m_settings.xamlEffectType,
                m_settings.cornerType,
                m_settings.patchBorder ? 1u : 0u,
                slot->visualsResolved ? 1u : 0u,
                slot->needsRetry ? 1u : 0u,
                AvoidWinrtBorderCornerRadius() ? 1u : 0u);
        }
        catch (const winrt::hresult_error& ex)
        {
            DebugLog(
                DBG_WARNING,
                L"[UICompat][MenuFlyout] RegisterPanel hr=0x%08X msg=%s",
                static_cast<unsigned>(ex.code()),
                ex.message().c_str());
        }
        catch (...)
        {
            DebugLog(DBG_WARNING, L"[UICompat][MenuFlyout] RegisterPanel unknown exception");
        }
    }

    void NcMenuFlyoutPanelRegistry::RetryUnresolvedPanels() noexcept
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
        const ULONGLONG now = GetTickCount64();
        for (auto& s : snapshot)
        {
            if (s->appliedOk && s->sizeApplyDone && !s->needsRetry)
                continue;

            // Углы: ждём SizeChanged (не долбить VTS3 на чужой VTC Add).
            if (s->appliedOk && s->sizeHooked && s->needsRetry && s->backgroundPatched
                && !s->sizeApplyDone)
            {
                ++still;
                continue;
            }

            // SMEH BG capture: throttle — иначе каждый VTC Add → VTS3 storm и
            // задержка ShellSurface apply на том же diagnostics.
            if (s->appliedOk && s->needsRetry && !s->backgroundPatched)
            {
                if (s->lastBgRetryTick != 0 && (now - s->lastBgRetryTick) < 50)
                {
                    ++still;
                    continue;
                }
                s->lastBgRetryTick = now;
            }

            ApplyPanelVisualsSync(*s);
            EnsureSizeHook(s);
            if (s->needsRetry || !s->appliedOk)
                ++still;
        }
        m_unresolved.store(still, std::memory_order_release);
    }

    void NcMenuFlyoutPanelRegistry::Unregister(InstanceHandle handle) noexcept
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
            // RestorePanelVisuals сам гасит фон только при backgroundPatched.
            bool hadOrig = false;
            bool usedDisp = false;
            (void)RestoreSlotOnUiThread(std::move(slot), true, hadOrig, usedDisp);
        }
        catch (...)
        {
        }
    }

    void NcMenuFlyoutPanelRegistry::RestoreAll() noexcept
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
        unsigned patchedCount = 0;
        for (auto& slot : snapshot)
        {
            const bool patched =
                slot && (slot->chromePatched || slot->presenterCornerPatched);
            if (patched)
                ++patchedCount;
            const int hasBorderPre = (slot && slot->chromeBorder) ? 1 : 0;
            const int hasVts3OrigPre = (slot && slot->originalCornerVts3) ? 1 : 0;
            const HWND hwnd = FindMenuFlyoutRestoreHwnd();
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
            (void)hasBorderPre;
            (void)hasVts3OrigPre;
            (void)hwnd;
        }

        const int pending = m_state->pendingAsync.load(std::memory_order_acquire);
        PinModuleIfPendingAsync(pending);

        DebugLog(
            DBG_IMPORTANT,
            L"[UICompat][MenuFlyout] RestoreAll total=%u ok=%u fail=%u noOrig=%u "
            L"disp=%u pendingAsync=%d",
            total,
            ok,
            fail,
            noOrig,
            viaDispatcher,
            pending);
    }

    void NcMenuFlyoutPanelRegistry::ReapplyPatchedBrushes() noexcept
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

    void NcMenuFlyoutPanelRegistry::RecaptureAfterThemeChange(Registry::Settings settings) noexcept
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
                if (slot->root)
                {
                    if (const auto presenter =
                            slot->root.try_as<wux::Controls::MenuFlyoutPresenter>())
                    {
                        presenter.ClearValue(wux::Controls::Control::BackgroundProperty());
                        presenter.ClearValue(wux::Controls::Control::CornerRadiusProperty());
                    }
                }
                if (slot->chromeBorder)
                {
                    slot->chromeBorder.ClearValue(wux::Controls::Border::BackgroundProperty());
                    slot->chromeBorder.ClearValue(wux::Controls::Border::BorderBrushProperty());
                    slot->chromeBorder.ClearValue(wux::Controls::Border::BorderThicknessProperty());
                    slot->chromeBorder.ClearValue(wux::Controls::Border::CornerRadiusProperty());
                }
            }
            catch (...)
            {
            }
            slot->originalPresenterBackground = nullptr;
            slot->originalBorderBackground = nullptr;
            slot->originalBorderBackgroundVts3 = nullptr;
            slot->originalBorderBrush = nullptr;
            slot->originalPresenterCornerVts3 = nullptr;
            slot->originalCornerVts3 = nullptr;
            slot->chromeCaptured = false;
            slot->chromePatched = false;
            slot->backgroundPatched = false;
            slot->presenterCornerCaptured = false;
            slot->presenterCornerPatched = false;
            slot->appliedOk = false;
            slot->needsRetry = true;
            slot->visualsResolved = false;
        }
        for (auto& s : snapshot)
            ApplyPanelGuarded(s);
        DebugLog(DBG_IMPORTANT, L"[UICompat][NcMenuFlyout] RecaptureAfterThemeChange");
    }

    void NcMenuFlyoutPanelRegistry::UpdateSettings(Registry::Settings settings) noexcept
    {
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;
        {
            std::lock_guard lock(m_mx);
            m_settings = settings;
        }
        ReapplyPatchedBrushes();
    }

    void NcMenuFlyoutPanelRegistry::RefreshBrushesAfterThemeChange(
        Registry::Settings settings) noexcept
    {
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;
        {
            std::lock_guard lock(m_mx);
            m_settings = settings;
        }

        // Даже при slots=0: меню NC/QS откроются после theme без flag.
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
        // Force new brush; Stop → Neutral (не SetValue stale theme).
        for (auto& slot : snapshot)
        {
            if (!slot)
                continue;
            slot->clearBackgroundOnRestore = true;
            slot->originalBorderBackground = nullptr;
            slot->originalBorderBackgroundVts3 = nullptr;
            slot->originalPresenterBackground = nullptr;
            slot->backgroundPatched = false;
            slot->appliedOk = false;
            slot->needsRetry = true;
        }
        for (auto& s : snapshot)
            ApplyPanelGuarded(s);
        DebugLog(DBG_IMPORTANT, L"[UICompat][NcMenuFlyout] RefreshBrushesAfterThemeChange");
    }
}
