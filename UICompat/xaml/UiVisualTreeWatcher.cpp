#include "UiPch.h"
#include "xaml/UiVisualTreeWatcher.hpp"
#include "xaml/NotificationCenterPanelRegistry.hpp"
#include "xaml/NcMenuFlyoutPanelRegistry.hpp"
#include "xaml/JumpListPanelRegistry.hpp"
#include "xaml/ShellSurfacePanelRegistry.hpp"
#include "xaml/SearchWebViewPanelRegistry.hpp"
#include "xaml/XamlDiagInit.hpp"

#include <DebugLog.h>
#include <RootBackdropPolicy.h>
#include <atomic>
#include <cstdio>
#include <string_view>
#include <vector>

#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/base.h>

namespace wux = winrt::Windows::UI::Xaml;

namespace
{
    thread_local const wchar_t* g_vtcStage = L"?";

    [[nodiscard]] wux::DependencyObject TryGetUiLayerRoot(
        IXamlDiagnostics* diag) noexcept
    {
        if (!diag)
            return nullptr;
        try
        {
            winrt::com_ptr<::IInspectable> layer;
            const HRESULT hr = diag->GetUiLayer(layer.put());
            if (FAILED(hr) || !layer)
                return nullptr;
            if (auto dep = layer.try_as<wux::DependencyObject>())
                return dep;
        }
        catch (...)
        {
        }
        return nullptr;
    }

    HRESULT SafeGetIInspectableFromHandle(
        IXamlDiagnostics* diag,
        InstanceHandle handle,
        IInspectable** out,
        bool* sehHit) noexcept
    {
        if (sehHit)
            *sehHit = false;
        if (!diag || !out)
            return E_POINTER;
        *out = nullptr;
        __try
        {
            return diag->GetIInspectableFromHandle(handle, out);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (sehHit)
                *sehHit = true;
            return E_FAIL;
        }
    }

    HRESULT SafeGetHandleFromIInspectable(
        IXamlDiagnostics* diag,
        IInspectable* insp,
        InstanceHandle* outHandle,
        bool* sehHit) noexcept
    {
        if (sehHit)
            *sehHit = false;
        if (!diag || !insp || !outHandle)
            return E_POINTER;
        *outHandle = 0;
        __try
        {
            return diag->GetHandleFromIInspectable(insp, outHandle);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (sehHit)
                *sehHit = true;
            return E_FAIL;
        }
    }

    [[nodiscard]] bool IsShellSurfaceHost() noexcept
    {
        return Shared::RootBackdropPolicy::IsStartMenuHostProcess()
            || Shared::RootBackdropPolicy::IsSearchHostProcess();
    }

    [[nodiscard]] bool IsUiThreadIxdeHost() noexcept
    {
        return IsShellSurfaceHost()
            || Shared::RootBackdropPolicy::IsShellNotificationHostProcess();
    }

    [[nodiscard]] bool IsShellSurfaceTargetName(std::wstring_view name) noexcept
    {
        return name == L"AcrylicBorder"
            || name == L"AcrylicOverlay"
            || name == L"AppBorder"
            || name == L"LayerBorder"
            || name == L"dropshadow"
            || name == L"DropShadow"
            || name == L"RootGridDropShadow"
            || name == L"StartDropShadow";
    }

    // Windhawk NC Styler / TranslucentShell — toast chrome в SEH.
    [[nodiscard]] bool IsToastBorderName(std::wstring_view name) noexcept
    {
        return name == L"ToastBackgroundBorder"
            || name == L"ToastBackgroundBorder2";
    }

    [[nodiscard]] bool IsToastViewClearName(std::wstring_view name) noexcept
    {
        return name == L"FlexibleNormalToastView";
    }

    // Штатный acrylic-overlay поверх ToastBackgroundBorder* (лог H28) — иначе Solid не виден.
    [[nodiscard]] bool IsToastOverlayClearName(std::wstring_view name) noexcept
    {
        return name == L"ToastAcrylicHightlightBorder" // typo в ActionCenter
            || name == L"ToastAcrylicHighlightBorder";
    }

    // Только NC/QS — для adopt g_shellSurfaceThreadId. Toast НЕ включать:
    // adopt toast tid крадёт NC tid (лог H32 FlexibleNormalToastView 9528→51616).
    [[nodiscard]] bool IsNcAdoptName(std::wstring_view name) noexcept
    {
        return name == L"NotificationCenterGrid"
            || name == L"CalendarCenterGrid"
            || name == L"ControlCenterRegion"
            || name == L"MediaTransportControlsRegion"
            || name == L"MediaTransportControlsRoot";
    }

    [[nodiscard]] bool IsToastProcessName(std::wstring_view name) noexcept
    {
        return IsToastBorderName(name)
            || IsToastViewClearName(name)
            || IsToastOverlayClearName(name)
            || name == L"ToastPeekRegion";
    }

    // Windhawk NC Styler / TranslucentShell — Jump List chrome в SEH/ShellHost.
    [[nodiscard]] bool IsJumpListChromeName(std::wstring_view name) noexcept
    {
        return name == L"JumpListRestyledAcrylic";
    }

    [[nodiscard]] bool IsJumpListCompanionName(std::wstring_view name) noexcept
    {
        return name == L"JumpListAcrylic";
    }

    [[nodiscard]] bool IsJumpListHostProcess() noexcept
    {
        return Shared::RootBackdropPolicy::IsShellNotificationHostProcess();
    }

    [[nodiscard]] bool IsJumpRelatedName(std::wstring_view name) noexcept
    {
        if (name.empty())
            return false;
        if (name.find(L"Jump") != std::wstring_view::npos)
            return true;
        if (name.find(L"jump") != std::wstring_view::npos)
            return true;
        return false;
    }

    [[nodiscard]] bool IsJumpRelatedType(std::wstring_view type) noexcept
    {
        return type.find(L"JumpView") != std::wstring_view::npos
            || type.find(L"JumpList") != std::wstring_view::npos;
    }

    void RegisterJumpListHandleNoexcept(
        UICompat::JumpListPanelRegistry* reg,
        InstanceHandle handle,
        UICompat::JumpListPanelRegistry::PanelKind kind) noexcept
    {
        if (!reg || handle == 0)
            return;
        try
        {
            reg->RegisterPanel(handle, kind);
        }
        catch (...)
        {
        }
    }

    // Только POD — ловит AV вокруг VTS3 Jump List apply.
    [[nodiscard]] bool RegisterJumpListHandleSeh(
        UICompat::JumpListPanelRegistry* reg,
        InstanceHandle handle,
        UICompat::JumpListPanelRegistry::PanelKind kind,
        DWORD* outSeh) noexcept
    {
        if (outSeh)
            *outSeh = 0;
        if (!reg || handle == 0)
            return false;
        __try
        {
            RegisterJumpListHandleNoexcept(reg, handle, kind);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (outSeh)
                *outSeh = GetExceptionCode();
            return false;
        }
    }
}

namespace UICompat
{
    UiVisualTreeWatcher::UiVisualTreeWatcher(
        IXamlDiagnostics* diagnostics,
        Registry::Settings settings) noexcept
        : m_ncRegistry(std::make_unique<NotificationCenterPanelRegistry>(
            Registry::ReadSurface(Registry::SurfaceKind::Popup)))
        , m_shellRegistry(
            std::make_unique<ShellSurfacePanelRegistry>(
                diagnostics,
                Registry::ReadSurface(Registry::SurfaceKind::Popup)))
        , m_menuFlyoutRegistry(
            std::make_unique<NcMenuFlyoutPanelRegistry>(
                diagnostics,
                Registry::ReadSurface(Registry::SurfaceKind::Menu)))
        , m_jumpListRegistry(
            std::make_unique<JumpListPanelRegistry>(
                diagnostics,
                Registry::ReadSurface(Registry::SurfaceKind::Menu)))
        , m_searchWebViewRegistry(
            std::make_unique<SearchWebViewPanelRegistry>(
                Registry::ReadSurface(Registry::SurfaceKind::Menu)))
    {
        (void)settings; // pending Popup — surface-specific читаем выше
        m_xamlDiagnostics = diagnostics;
        if (m_xamlDiagnostics)
            m_xamlDiagnostics->AddRef();
        DebugLog(DBG_IMPORTANT, L"[UICompat][VTC] watcher created");
    }

    UiVisualTreeWatcher::~UiVisualTreeWatcher()
    {
        DebugLog(DBG_IMPORTANT, L"[UICompat][VTC] watcher destroyed");
        if (m_xamlDiagnostics)
            m_xamlDiagnostics->Release();
    }

    HRESULT STDMETHODCALLTYPE UiVisualTreeWatcher::QueryInterface(
        REFIID riid, void** ppvObject) noexcept
    {
        if (!ppvObject)
            return E_POINTER;
        *ppvObject = nullptr;
        if (riid == IID_IUnknown
            || riid == IID_IVisualTreeServiceCallback
            || riid == IID_IVisualTreeServiceCallback2)
        {
            *ppvObject = static_cast<IVisualTreeServiceCallback2*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE UiVisualTreeWatcher::AddRef() noexcept
    {
        return m_ref.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE UiVisualTreeWatcher::Release() noexcept
    {
        const ULONG n = m_ref.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (n == 0)
            delete this;
        return n;
    }

    HRESULT STDMETHODCALLTYPE UiVisualTreeWatcher::OnVisualTreeChange(
        ParentChildRelation relation,
        VisualElement element,
        VisualMutationType mutationType) noexcept
    {
        __try
        {
            return OnVisualTreeChangeImpl(relation, element, mutationType);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            DebugLog(
                DBG_CRITICAL,
                L"[UICompat][VTC] SEH in OnVisualTreeChange code=0x%08X stage=%ls",
                static_cast<unsigned>(GetExceptionCode()),
                g_vtcStage ? g_vtcStage : L"?");
            return E_FAIL;
        }
    }

    HRESULT UiVisualTreeWatcher::OnVisualTreeChangeImpl(
        ParentChildRelation /*relation*/,
        VisualElement element,
        VisualMutationType mutationType) noexcept
    {
        try
        {
            g_vtcStage = L"dispatch";

            // Windhawk: Start/Search/NC — UI-поток окна (иначе AV в WUX / H49).
            // targetTid == 0 (до init / после Stop) — отказ.
            // SEH: IXDE CAS на tid первого CoreWindow, а Add NC-панелей — на tid
            // окна с деревом → TryAdoptShellSurfaceThreadIdForNcVtc.
            if (IsUiThreadIxdeHost())
            {
                const DWORD targetTid = GetShellSurfaceThreadId();
                const DWORD curTid    = GetCurrentThreadId();
                const std::wstring_view nameSkip{
                    element.Name, SysStringLen(element.Name) };
                const std::wstring_view typeSkip{
                    element.Type, SysStringLen(element.Type) };
                const bool jumpListTarget =
                    mutationType == Add
                    && IsJumpListHostProcess()
                    && (IsJumpListChromeName(nameSkip)
                        || IsJumpListCompanionName(nameSkip)
                        || IsJumpRelatedName(nameSkip)
                        || IsJumpRelatedType(typeSkip));
                // targetTid==0 до NC CAS — не глотать Jump List Add (иначе меню
                // без кастома до первого NC / после рестарта SEH).
                if (targetTid == 0 && !jumpListTarget)
                {
                    return S_OK;
                }
                if (targetTid != 0 && curTid != targetTid)
                {
                    const bool toastTarget = IsToastProcessName(nameSkip)
                        && Shared::RootBackdropPolicy::IsShellExperienceHostProcess();
                    // MenuFlyout / Jump List могут быть на другом STA —
                    // обработать без adopt (не красть NC / Start tid).
                    const bool menuFlyoutHost =
                        Shared::RootBackdropPolicy::IsShellNotificationHostProcess()
                        || Shared::RootBackdropPolicy::IsStartMenuHostProcess()
                        || Shared::RootBackdropPolicy::IsSearchHostProcess();
                    const bool menuFlyoutTarget =
                        mutationType == Add
                        && menuFlyoutHost
                        && (typeSkip == L"Windows.UI.Xaml.Controls.MenuFlyoutPresenter"
                            || typeSkip.ends_with(L".MenuFlyoutPresenter"));
                    if (!toastTarget && !menuFlyoutTarget && !jumpListTarget)
                    {
                        const bool ncTarget =
                            mutationType == Add && IsNcAdoptName(nameSkip);
                        if (!ncTarget
                            || !TryAdoptShellSurfaceThreadIdForNcVtc(curTid))
                        {
                            return S_OK;
                        }
                    }
                }
            }

            if (mutationType == Add)
            {
                const std::wstring_view type{ element.Type, SysStringLen(element.Type) };
                const std::wstring_view name{ element.Name, SysStringLen(element.Name) };

                if (TryRegisterNcPanel(element.Handle, type, name))
                    return S_OK;
                if (TryRegisterToastTarget(element.Handle, type, name))
                    return S_OK;
                if (TryRegisterMenuFlyout(element.Handle, type))
                    return S_OK;
                if (TryRegisterJumpList(element.Handle, type, name))
                    return S_OK;
                if (TryRegisterShellSurfaceBorder(element.Handle, type, name))
                    return S_OK;
                if (TryRegisterSearchWebView(element.Handle, type, name))
                    return S_OK;
                // Border шаблона MenuFlyout часто появляется после Presenter Add.
                if (m_menuFlyoutRegistry && m_menuFlyoutRegistry->PanelCount() > 0
                    && m_menuFlyoutRegistry->UnresolvedCount() > 0)
                {
                    m_menuFlyoutRegistry->RetryUnresolvedPanels();
                }
                if (m_jumpListRegistry && m_jumpListRegistry->PanelCount() > 0
                    && m_jumpListRegistry->UnresolvedCount() > 0)
                {
                    m_jumpListRegistry->RetryUnresolvedPanels();
                }
                return S_OK;
            }

            if (mutationType == Remove)
            {
                g_vtcStage = L"Remove";
                if (m_ncRegistry)
                    m_ncRegistry->Unregister(element.Handle);
                if (m_menuFlyoutRegistry)
                    m_menuFlyoutRegistry->Unregister(element.Handle);
                if (m_jumpListRegistry)
                    m_jumpListRegistry->Unregister(element.Handle);
                if (m_searchWebViewRegistry)
                    m_searchWebViewRegistry->Unregister(element.Handle);
                if (m_shellRegistry)
                    m_shellRegistry->Unregister(element.Handle);
                return S_OK;
            }
            return S_OK;
        }
        catch (const winrt::hresult_error& ex)
        {
            DebugLog(
                DBG_ERROR,
                L"[UICompat][VTC] OnVisualTreeChange hr=0x%08X msg=%s",
                static_cast<unsigned>(ex.code()),
                ex.message().c_str());
            return ex.code();
        }
        catch (...)
        {
            return E_FAIL;
        }
    }

    HRESULT STDMETHODCALLTYPE UiVisualTreeWatcher::OnElementStateChanged(
        InstanceHandle, VisualElementState, LPCWSTR) noexcept
    {
        return S_OK;
    }

    void UiVisualTreeWatcher::RestoreAllFills() noexcept
    {
        m_diagUsable.store(false, std::memory_order_release);
        if (m_ncRegistry)
            m_ncRegistry->RestoreAll();
        if (m_menuFlyoutRegistry)
            m_menuFlyoutRegistry->RestoreAll();
        if (m_jumpListRegistry)
            m_jumpListRegistry->RestoreAll();
        if (m_searchWebViewRegistry)
            m_searchWebViewRegistry->RestoreAll();
        if (m_shellRegistry)
            m_shellRegistry->RestoreAll();
    }

    void UiVisualTreeWatcher::ReapplyPatchedBrushes() noexcept
    {
        if (m_ncRegistry)
            m_ncRegistry->ReapplyPatchedBrushes();
        if (m_menuFlyoutRegistry)
            m_menuFlyoutRegistry->ReapplyPatchedBrushes();
        if (m_jumpListRegistry)
            m_jumpListRegistry->ReapplyPatchedBrushes();
        if (m_searchWebViewRegistry)
            m_searchWebViewRegistry->ReapplyPatchedBrushes();
        if (m_shellRegistry)
            m_shellRegistry->ReapplyPatchedBrushes();
    }

    void UiVisualTreeWatcher::RecaptureAfterThemeChange() noexcept
    {
        const auto popup = Registry::ReadSurface(Registry::SurfaceKind::Popup);
        const auto menu = Registry::ReadSurface(Registry::SurfaceKind::Menu);
        if (m_ncRegistry)
            m_ncRegistry->RecaptureAfterThemeChange(popup);
        if (m_shellRegistry)
            m_shellRegistry->RecaptureAfterThemeChange(popup);
        if (m_menuFlyoutRegistry)
            m_menuFlyoutRegistry->RecaptureAfterThemeChange(menu);
        if (m_jumpListRegistry)
            m_jumpListRegistry->RecaptureAfterThemeChange(menu);
        // SearchWebView: CSS/WebView — без XAML original*; Reapply достаточно.
        if (m_searchWebViewRegistry)
            m_searchWebViewRegistry->UpdateSettings(menu);
    }

    void UiVisualTreeWatcher::RefreshBrushesAfterThemeChange() noexcept
    {
        const auto popup = Registry::ReadSurface(Registry::SurfaceKind::Popup);
        const auto menu = Registry::ReadSurface(Registry::SurfaceKind::Menu);
        if (m_ncRegistry)
            m_ncRegistry->RefreshBrushesAfterThemeChange(popup);
        if (m_shellRegistry)
            m_shellRegistry->RefreshBrushesAfterThemeChange(popup);
        if (m_menuFlyoutRegistry)
            m_menuFlyoutRegistry->RefreshBrushesAfterThemeChange(menu);
        if (m_jumpListRegistry)
            m_jumpListRegistry->RefreshBrushesAfterThemeChange(menu);
        if (m_searchWebViewRegistry)
            m_searchWebViewRegistry->UpdateSettings(menu);
    }

    void UiVisualTreeWatcher::UpdateSettings(Registry::Settings settings) noexcept
    {
        (void)settings;
        const auto popup = Registry::ReadSurface(Registry::SurfaceKind::Popup);
        const auto menu = Registry::ReadSurface(Registry::SurfaceKind::Menu);
        if (m_ncRegistry)
            m_ncRegistry->UpdateSettings(popup);
        if (m_shellRegistry)
            m_shellRegistry->UpdateSettings(popup);
        if (m_menuFlyoutRegistry)
            m_menuFlyoutRegistry->UpdateSettings(menu);
        if (m_jumpListRegistry)
            m_jumpListRegistry->UpdateSettings(menu);
        if (m_searchWebViewRegistry)
            m_searchWebViewRegistry->UpdateSettings(menu);
    }

    unsigned UiVisualTreeWatcher::NcPanelCount() const noexcept
    {
        return m_ncRegistry ? m_ncRegistry->PanelCount() : 0u;
    }

    unsigned UiVisualTreeWatcher::ShellSurfacePanelCount() const noexcept
    {
        return m_shellRegistry ? m_shellRegistry->PanelCount() : 0u;
    }

    unsigned UiVisualTreeWatcher::MenuFlyoutPanelCount() const noexcept
    {
        return m_menuFlyoutRegistry ? m_menuFlyoutRegistry->PanelCount() : 0u;
    }

    bool UiVisualTreeWatcher::MenuThemePreferNeutral() const noexcept
    {
        return m_menuFlyoutRegistry
            && m_menuFlyoutRegistry->ThemePreferNeutralRestore();
    }

    void UiVisualTreeWatcher::FlushPendingShellPrepares() noexcept
    {
        if (m_shellRegistry)
            m_shellRegistry->FlushPendingPrepares();
    }

    bool UiVisualTreeWatcher::TryRegisterNcPanel(
        InstanceHandle handle,
        std::wstring_view type,
        std::wstring_view name) noexcept
    {
        if (!m_ncRegistry || handle == 0)
            return false;

        const bool isNcName =
            name == L"NotificationCenterGrid"
            || name == L"CalendarCenterGrid"
            || name == L"ControlCenterRegion"
            || name == L"MediaTransportControlsRegion"
            || name == L"MediaTransportControlsRoot";
        if (!isNcName)
            return false;
        if (!(type == L"Windows.UI.Xaml.Controls.Grid" || type == L"Grid"))
            return false;

        g_vtcStage = L"NC:Grid";
        const auto grid = FromHandle<wux::Controls::Grid>(handle);
        if (!grid)
            return false;

        m_ncRegistry->RegisterPanel(handle, grid);
        return true;
    }

    bool UiVisualTreeWatcher::TryRegisterToastTarget(
        InstanceHandle handle,
        std::wstring_view type,
        std::wstring_view name) noexcept
    {
        if (!m_ncRegistry || handle == 0)
            return false;

        // Toast: донор (TranslucentShell) — WindhawkBlur/Acrylic на ToastBackgroundBorder*.
        // Solid-tint без blur пользователю не нужен; наш XamlBlurBrush/Acrylic на toast
        // ранее валил SEH (H19/H22) и оставлял слоты → UAF Release при Stop (no_uicompat).
        // Пока не патчим toast (съедаем Add, без registry).
        if (IsToastBorderName(name) || IsToastViewClearName(name)
            || IsToastOverlayClearName(name))
        {
            (void)type;
            (void)handle;
            return true;
        }

        return false;
    }

    bool UiVisualTreeWatcher::TryRegisterMenuFlyout(
        InstanceHandle handle,
        std::wstring_view type) noexcept
    {
        if (!m_menuFlyoutRegistry || handle == 0)
            return false;
        if (!Shared::RootBackdropPolicy::IsShellNotificationHostProcess()
            && !Shared::RootBackdropPolicy::IsStartMenuHostProcess()
            && !Shared::RootBackdropPolicy::IsSearchHostProcess())
            return false;
        if (!(type == L"Windows.UI.Xaml.Controls.MenuFlyoutPresenter"
                || type.ends_with(L".MenuFlyoutPresenter")))
            return false;

        g_vtcStage = L"MenuFlyout:Presenter";
        const auto root = FromHandle<wux::FrameworkElement>(handle);
        if (!root)
        {
            return false;
        }

        m_menuFlyoutRegistry->RegisterPanel(handle, root);
        return true;
    }

    bool UiVisualTreeWatcher::TryRegisterJumpList(
        InstanceHandle handle,
        std::wstring_view type,
        std::wstring_view name) noexcept
    {
        if (!m_jumpListRegistry || handle == 0)
            return false;
        if (!IsJumpListHostProcess())
            return false;
        if (!(type == L"Windows.UI.Xaml.Controls.Border" || type == L"Border"))
            return false;

        using Kind = JumpListPanelRegistry::PanelKind;
        Kind kind = Kind::Chrome;
        if (IsJumpListChromeName(name))
            kind = Kind::Chrome;
        else if (IsJumpListCompanionName(name))
            kind = Kind::Companion;
        else
            return false;

        g_vtcStage = kind == Kind::Companion
            ? L"JumpList:Companion"
            : L"JumpList:Border";

        // VTS3-only: без FromHandle/WinRT Background (AV 0xC0000005 на RestyledAcrylic).
        (void)RegisterJumpListHandleSeh(
            m_jumpListRegistry.get(), handle, kind, nullptr);

        // Даже при SEH — съесть Add (не crash-loop).
        return true;
    }

    void UiVisualTreeWatcher::DiscoverSearchWebViewSubtree(
        wux::DependencyObject const& root) noexcept
    {
        if (!m_searchWebViewRegistry || !root || !m_xamlDiagnostics)
            return;
        if (!m_diagUsable.load(std::memory_order_acquire))
            return;

        try
        {
            std::vector<wux::DependencyObject> stack;
            stack.push_back(root);
            unsigned visited = 0;
            unsigned found = 0;
            while (!stack.empty() && visited < 48)
            {
                const auto node = stack.back();
                stack.pop_back();
                ++visited;

                if (const auto fe = node.try_as<wux::FrameworkElement>())
                {
                    const auto className = winrt::get_class_name(fe);
                    if (className == L"Windows.UI.Xaml.Controls.WebView"
                        || className == L"WebView2Standalone.Controls.WebView2")
                    {
                        InstanceHandle childHandle = 0;
                        bool sehHit = false;
                        const auto insp =
                            fe.as<winrt::Windows::Foundation::IInspectable>();
                        const HRESULT hr = SafeGetHandleFromIInspectable(
                            m_xamlDiagnostics,
                            reinterpret_cast<::IInspectable*>(winrt::get_abi(insp)),
                            &childHandle,
                            &sehHit);
                        if (sehHit)
                        {
                            m_diagUsable.store(false, std::memory_order_release);
                            return;
                        }
                        if (SUCCEEDED(hr) && childHandle != 0)
                        {
                            const unsigned before =
                                m_searchWebViewRegistry->PanelCount();
                            m_searchWebViewRegistry->TryRegisterIfWebView(
                                childHandle, fe);
                            if (m_searchWebViewRegistry->PanelCount() > before)
                                ++found;
                        }
                    }
                }

                const int n = wux::Media::VisualTreeHelper::GetChildrenCount(node);
                for (int i = 0; i < n; ++i)
                    stack.push_back(wux::Media::VisualTreeHelper::GetChild(node, i));
            }
        }
        catch (...)
        {
        }
    }

    void UiVisualTreeWatcher::ScheduleDiscoverSearchWebViewSubtree(
        wux::FrameworkElement const& host) noexcept
    {
        if (!host || !m_searchWebViewRegistry)
            return;
        try
        {
            // Не VisualTreeHelper/QI WebView2 на стеке Advise — AV в
            // WebView2Standalone.dll (лог: css_ok → crash).
            auto* self = this;
            winrt::weak_ref<wux::FrameworkElement> weakHost{ host };
            const auto dispatcher = host.Dispatcher();
            if (!dispatcher)
            {
                DiscoverSearchWebViewSubtree(host);
                return;
            }
            (void)dispatcher.RunAsync(
                winrt::Windows::UI::Core::CoreDispatcherPriority::Low,
                winrt::Windows::UI::Core::DispatchedHandler(
                    [self, weakHost]()
                    {
                        try
                        {
                            if (!self->m_searchWebViewRegistry)
                                return;
                            if (const auto fe = weakHost.get())
                                self->DiscoverSearchWebViewSubtree(fe);
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

    bool UiVisualTreeWatcher::TryRegisterSearchWebView(
        InstanceHandle handle,
        std::wstring_view type,
        std::wstring_view name) noexcept
    {
        if (!m_searchWebViewRegistry || handle == 0)
            return false;
        if (!Shared::RootBackdropPolicy::IsSearchHostProcess())
            return false;

        // Кандидат по VTC Type/Name (дешёвый фильтр). Решение — только
        // get_class_name внутри TryRegisterIfWebView (путь донора).
        const bool candidate =
            type == L"Windows.UI.Xaml.Controls.WebView"
            || type == L"WebView"
            || type == L"WebView2Standalone.Controls.WebView2"
            || type.find(L"WebView") != std::wstring_view::npos
            || name.find(L"WebView") != std::wstring_view::npos;
        if (!candidate)
            return false;

        g_vtcStage = L"SearchWebView";
        const auto fe = FromHandle<wux::FrameworkElement>(handle);
        if (!fe)
            return false;

        const unsigned before = m_searchWebViewRegistry->PanelCount();
        m_searchWebViewRegistry->TryRegisterIfWebView(handle, fe);

        const bool hostSubtree =
            type.find(L"HostedWebView") != std::wstring_view::npos
            || name.find(L"HostedWebView") != std::wstring_view::npos
            || name == L"WebViewGrid";
        if (hostSubtree)
            ScheduleDiscoverSearchWebViewSubtree(fe);

        return m_searchWebViewRegistry->PanelCount() > before || hostSubtree;
    }

    bool UiVisualTreeWatcher::TryRegisterShellSurfaceBorder(
        InstanceHandle handle,
        std::wstring_view type,
        std::wstring_view name) noexcept
    {
        if (!m_shellRegistry || handle == 0)
            return false;
        if (!IsShellSurfaceHost())
            return false;
        if (!IsShellSurfaceTargetName(name))
            return false;
        if (!(type == L"Windows.UI.Xaml.Controls.Border" || type == L"Border"))
            return false;

        g_vtcStage = L"Shell:Border";
        const auto border = FromHandle<wux::Controls::Border>(handle);
        if (!border)
            return false;

        m_shellRegistry->RegisterBorder(handle, border);
        return true;
    }

    template<typename T>
    T UiVisualTreeWatcher::FromHandle(InstanceHandle handle) noexcept
    {
        if (!m_xamlDiagnostics || handle == 0 || !m_diagUsable.load(std::memory_order_acquire))
            return nullptr;
        try
        {
            IInspectable* insp = nullptr;
            bool sehHit = false;
            const HRESULT hr = SafeGetIInspectableFromHandle(
                m_xamlDiagnostics, handle, &insp, &sehHit);
            if (sehHit)
            {
                m_diagUsable.store(false, std::memory_order_release);
                return nullptr;
            }
            if (FAILED(hr) || !insp)
                return nullptr;
            T result{ nullptr };
            winrt::copy_from_abi(result, insp);
            insp->Release();
            return result;
        }
        catch (...)
        {
            return nullptr;
        }
    }

    void UiVisualTreeWatcher::CatchUpWalk(
        wux::DependencyObject const& node,
        int depth,
        unsigned& registeredNc,
        unsigned& registeredShell) noexcept
    {
        if (!node || depth > 48 || !m_xamlDiagnostics)
            return;
        if (!m_diagUsable.load(std::memory_order_acquire))
            return;

        try
        {
            if (const auto fe = node.try_as<wux::FrameworkElement>())
            {
                const auto name = fe.Name();

                if (m_ncRegistry)
                {
                    const std::wstring_view nameView{ name.c_str(), name.size() };
                    const auto insp =
                        fe.as<winrt::Windows::Foundation::IInspectable>();
                    InstanceHandle handle = 0;
                    bool sehHit = false;
                    const HRESULT hr = SafeGetHandleFromIInspectable(
                        m_xamlDiagnostics,
                        reinterpret_cast<::IInspectable*>(winrt::get_abi(insp)),
                        &handle,
                        &sehHit);
                    if (sehHit)
                    {
                        m_diagUsable.store(false, std::memory_order_release);
                        return;
                    }
                    if (SUCCEEDED(hr) && handle != 0)
                    {
                        const unsigned before = m_ncRegistry->PanelCount();
                        // Toast только через VTC TryRegisterToastTarget — CatchUp на toast
                        // HWND давал panels=15 + async storm и крал marshal tid (лог H34).
                        if (nameView == L"NotificationCenterGrid"
                            || nameView == L"CalendarCenterGrid"
                            || nameView == L"ControlCenterRegion"
                            || nameView == L"MediaTransportControlsRegion"
                            || nameView == L"MediaTransportControlsRoot")
                        {
                            if (const auto grid = fe.try_as<wux::Controls::Grid>())
                                m_ncRegistry->RegisterPanel(handle, grid);
                        }
                        if (m_ncRegistry->PanelCount() > before)
                            ++registeredNc;
                    }
                }

                if (m_shellRegistry && IsShellSurfaceHost()
                    && IsShellSurfaceTargetName(
                        std::wstring_view{ name.c_str(), name.size() }))
                {
                    if (const auto border = fe.try_as<wux::Controls::Border>())
                    {
                        const auto insp =
                            fe.as<winrt::Windows::Foundation::IInspectable>();
                        InstanceHandle handle = 0;
                        bool sehHit = false;
                        const HRESULT hr = SafeGetHandleFromIInspectable(
                            m_xamlDiagnostics,
                            reinterpret_cast<::IInspectable*>(winrt::get_abi(insp)),
                            &handle,
                            &sehHit);
                        if (sehHit)
                        {
                            m_diagUsable.store(false, std::memory_order_release);
                            return;
                        }
                        if (SUCCEEDED(hr) && handle != 0)
                        {
                            const unsigned before = m_shellRegistry->PanelCount();
                            m_shellRegistry->RegisterBorder(handle, border);
                            if (m_shellRegistry->PanelCount() > before)
                                ++registeredShell;
                        }
                    }
                }
            }

            const int n = wux::Media::VisualTreeHelper::GetChildrenCount(node);
            for (int i = 0; i < n; ++i)
            {
                CatchUpWalk(
                    wux::Media::VisualTreeHelper::GetChild(node, i),
                    depth + 1,
                    registeredNc,
                    registeredShell);
                if (!m_diagUsable.load(std::memory_order_acquire))
                    return;
            }
        }
        catch (...)
        {
        }
    }

    unsigned UiVisualTreeWatcher::CatchUpExistingNcPanels() noexcept
    {
        // Window::Current() потоко-аффинный — вне UI-потока IXDE всегда nullptr.
        // SEH: toast мог занять shellSurfaceTid — при CatchUp на UI NC adopt tid.
        if (Shared::RootBackdropPolicy::IsShellNotificationHostProcess())
        {
            const DWORD targetTid = GetShellSurfaceThreadId();
            const DWORD curTid = GetCurrentThreadId();
            if (targetTid == 0)
            {
                return 0;
            }
            if (curTid != targetTid
                && !TryAdoptShellSurfaceThreadIdForNcVtc(curTid))
            {
                return 0;
            }
        }

        unsigned registeredNc = 0;
        unsigned registeredShell = 0;
        const char* skipReason = nullptr;
        const char* rootSource = "";
        try
        {
            const auto window = wux::Window::Current();
            wux::DependencyObject root{ nullptr };
            if (window)
            {
                if (const auto content = window.Content())
                {
                    root = content;
                    rootSource = "content";
                }
            }
            else
            {
                skipReason = "no_window";
            }

            // Content null (dormant/cloaked QS) — diagnostics UI layer как корень обхода.
            if (!root)
            {
                root = TryGetUiLayerRoot(m_xamlDiagnostics);
                if (root)
                    rootSource = "ui_layer";
                else if (!skipReason)
                    skipReason = "no_content";
            }

            if (root)
            {
                CatchUpWalk(root, 0, registeredNc, registeredShell);
                if (registeredNc > 0)
                    skipReason = nullptr;
            }
        }
        catch (...)
        {
            registeredNc = 0;
            skipReason = "exception";
        }

        DebugLog(
            DBG_IMPORTANT,
            L"[UICompat][VTC] CatchUpExistingNcPanels registered=%u total=%u src=%S",
            registeredNc,
            NcPanelCount(),
            rootSource[0] ? rootSource : (skipReason ? skipReason : ""));
        return registeredNc;
    }

    unsigned UiVisualTreeWatcher::CatchUpExistingShellSurfacePanels() noexcept
    {
        if (!IsShellSurfaceHost())
            return 0;

        // Обход дерева допустим только с UI-потока CoreWindow.
        const DWORD targetTid = GetShellSurfaceThreadId();
        const bool onUiThread  = (targetTid != 0 && GetCurrentThreadId() == targetTid);

        if (!onUiThread)
            return 0;

        unsigned registeredNc = 0;
        unsigned registeredShell = 0;
        try
        {
            const auto window = wux::Window::Current();
            if (!window)
                return 0;
            const auto content = window.Content();
            if (!content)
                return 0;
            CatchUpWalk(content, 0, registeredNc, registeredShell);
        }
        catch (...)
        {
            registeredShell = 0;
        }

        DebugLog(
            DBG_IMPORTANT,
            L"[UICompat][VTC] CatchUpExistingShellSurface registered=%u total=%u",
            registeredShell,
            ShellSurfacePanelCount());
        return registeredShell;
    }
}
