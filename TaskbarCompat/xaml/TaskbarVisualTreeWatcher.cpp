#include "TaskbarPch.h"
#include "xaml/TaskbarVisualTreeWatcher.hpp"
#include "xaml/TaskbarBackgroundRegistry.hpp"
#include "xaml/InputSwitchPanelRegistry.hpp"
#include "xaml/TrayOverflowPanelRegistry.hpp"
#include "xaml/TrayHoverFlyoutPanelRegistry.hpp"
#include "xaml/TrayTooltipPanelRegistry.hpp"
#include "xaml/TrayMenuFlyoutPanelRegistry.hpp"
#include "xaml/TaskbarTapSite.hpp"

#include <unknwn.h>

#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>

#include <DebugLog.h>

#include <cstdint>

namespace wux = winrt::Windows::UI::Xaml;

namespace
{
    // Для лога при SEH (не ловится catch (...)).
    thread_local const wchar_t* g_vtcStage = L"(none)";


    // AV на освобождённом IXamlDiagnostics не ловится C++ catch — только SEH.
    [[nodiscard]] HRESULT SafeGetIInspectableFromHandle(
        IXamlDiagnostics* diag,
        InstanceHandle handle,
        IInspectable** out,
        bool* sehHit) noexcept
    {
        if (sehHit)
            *sehHit = false;
        if (!diag || !out || handle == 0)
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

    [[nodiscard]] HRESULT SafeGetHandleFromIInspectable(
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
}

namespace TaskbarCompat
{
    TaskbarVisualTreeWatcher::TaskbarVisualTreeWatcher(
        IXamlDiagnostics* diagnostics,
        Registry::Settings settings) noexcept
        : m_registry(std::make_unique<TaskbarBackgroundRegistry>(settings))
        , m_inputSwitchRegistry(std::make_unique<InputSwitchPanelRegistry>(
            Registry::ReadSurface(Registry::SurfaceKind::Popup)))
        , m_overflowRegistry(std::make_unique<TrayOverflowPanelRegistry>(
            Registry::ReadSurface(Registry::SurfaceKind::Popup)))
        , m_hoverFlyoutRegistry(std::make_unique<TrayHoverFlyoutPanelRegistry>(
            Registry::ReadSurface(Registry::SurfaceKind::Preview)))
        , m_tooltipRegistry(std::make_unique<TrayTooltipPanelRegistry>(
            Registry::ReadSurface(Registry::SurfaceKind::Tooltip)))
        , m_menuFlyoutRegistry(std::make_unique<TrayMenuFlyoutPanelRegistry>(
            Registry::ReadSurface(Registry::SurfaceKind::Menu)))
    {
        m_xamlDiagnostics = diagnostics;
        if (m_xamlDiagnostics)
            m_xamlDiagnostics->AddRef();

        DebugLog(DBG_IMPORTANT, L"[TaskbarCompat][VTC] watcher created");
    }

    TaskbarVisualTreeWatcher::~TaskbarVisualTreeWatcher()
    {
        DebugLog(DBG_IMPORTANT, L"[TaskbarCompat][VTC] watcher destroyed");

        if (m_xamlDiagnostics)
            m_xamlDiagnostics->Release();
    }

    HRESULT STDMETHODCALLTYPE TaskbarVisualTreeWatcher::QueryInterface(
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

    ULONG STDMETHODCALLTYPE TaskbarVisualTreeWatcher::AddRef() noexcept
    {
        return m_ref.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE TaskbarVisualTreeWatcher::Release() noexcept
    {
        const ULONG c = m_ref.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (c == 0)
            delete this;
        return c;
    }

    template<typename T>
    T TaskbarVisualTreeWatcher::FromHandle(InstanceHandle handle) noexcept
    {
        try
        {
            if (!m_diagUsable.load(std::memory_order_acquire) || !m_xamlDiagnostics || handle == 0)
                return nullptr;

            IInspectable* insp = nullptr;
            bool sehHit = false;
            const HRESULT hr =
                SafeGetIInspectableFromHandle(m_xamlDiagnostics, handle, &insp, &sehHit);
            if (sehHit)
            {
                m_diagUsable.store(false, std::memory_order_release);
                DebugLog(
                    DBG_CRITICAL,
                    L"[TaskbarCompat][VTC] FromHandle SEH — diag disabled (stage=%ls)",
                    g_vtcStage ? g_vtcStage : L"?");
                return nullptr;
            }
            if (FAILED(hr) || !insp)
                return nullptr;

            T result = nullptr;
            winrt::copy_from_abi(result, insp);
            insp->Release();
            return result;
        }
        catch (...)
        {
            return nullptr;
        }
    }

    wux::FrameworkElement TaskbarVisualTreeWatcher::FindParent(
        std::wstring_view name,
        wux::FrameworkElement element) noexcept
    {
        try
        {
            const auto parent = wux::Media::VisualTreeHelper::GetParent(element)
                .try_as<wux::FrameworkElement>();
            if (!parent)
                return nullptr;

            if (parent.Name() == name)
                return parent;

            return FindParent(name, parent);
        }
        catch (...)
        {
            return nullptr;
        }
    }

    void TaskbarVisualTreeWatcher::EnqueuePendingRectanglePatch(
        InstanceHandle parentHandle,
        InstanceHandle elementHandle,
        bool isBackgroundFill) noexcept
    {
        std::lock_guard lock(m_pendingMx);
        for (const PendingRectanglePatch& p : m_pendingRectangles)
        {
            if (p.element == elementHandle && p.isBackgroundFill == isBackgroundFill)
                return;
        }
        constexpr size_t kMaxPending = 32;
        if (m_pendingRectangles.size() >= kMaxPending)
            m_pendingRectangles.erase(m_pendingRectangles.begin());
        m_pendingRectangles.push_back(PendingRectanglePatch{
            parentHandle, elementHandle, isBackgroundFill });
    }

    void TaskbarVisualTreeWatcher::RetryPendingRectanglePatches() noexcept
    {
        std::vector<PendingRectanglePatch> pending;
        {
            std::lock_guard lock(m_pendingMx);
            pending.swap(m_pendingRectangles);
        }
        for (const PendingRectanglePatch& p : pending)
        {
            if (!ApplyRectangleAddFromHandles(p.parent, p.element, p.isBackgroundFill))
                EnqueuePendingRectanglePatch(p.parent, p.element, p.isBackgroundFill);
        }
    }

    bool TaskbarVisualTreeWatcher::ApplyRectangleAddFromHandles(
        InstanceHandle parentHandle,
        InstanceHandle elementHandle,
        bool isBackgroundFill) noexcept
    {
        try
        {
            g_vtcStage = L"Rectangle:FromHandle_parent";
            const auto parentElement = FromHandle<wux::FrameworkElement>(parentHandle);
            if (!parentElement)
            {
                DebugLog(
                    DBG_NORMAL,
                    L"[TaskbarCompat][VTC] ApplyRectangle: parentElement null (parent=%llu elem=%llu) — pending",
                    static_cast<unsigned long long>(parentHandle),
                    static_cast<unsigned long long>(elementHandle));
                EnqueuePendingRectanglePatch(parentHandle, elementHandle, isBackgroundFill);
                return false;
            }

            g_vtcStage = L"Rectangle:FindParent_TaskbarFrame";
            const auto frame = FindParent(L"TaskbarFrame", parentElement);
            if (!frame)
            {
                DebugLog(
                    DBG_NORMAL,
                    L"[TaskbarCompat][VTC] ApplyRectangle: TaskbarFrame parent not found (parent=%llu elem=%llu) — pending",
                    static_cast<unsigned long long>(parentHandle),
                    static_cast<unsigned long long>(elementHandle));
                EnqueuePendingRectanglePatch(parentHandle, elementHandle, isBackgroundFill);
                return false;
            }

            InstanceHandle frameHandle = 0;
            g_vtcStage = L"Rectangle:GetHandleFromIInspectable";
            if (!m_diagUsable.load(std::memory_order_acquire) || !m_xamlDiagnostics)
                return false;
            bool sehHit = false;
            const HRESULT hrFrame = SafeGetHandleFromIInspectable(
                m_xamlDiagnostics,
                reinterpret_cast<::IInspectable*>(winrt::get_abi(frame)),
                &frameHandle,
                &sehHit);
            if (sehHit)
            {
                m_diagUsable.store(false, std::memory_order_release);
                DebugLog(
                    DBG_CRITICAL,
                    L"[TaskbarCompat][VTC] GetHandleFromIInspectable SEH — diag disabled");
                return false;
            }
            if (FAILED(hrFrame))
            {
                DebugLog(
                    DBG_WARNING,
                    L"[TaskbarCompat][VTC] ApplyRectangle: GetHandleFromIInspectable hr=0x%08X",
                    static_cast<unsigned>(hrFrame));
                return false;
            }

            g_vtcStage = L"Rectangle:FromHandle_shape";
            const auto shape = FromHandle<wux::Shapes::Rectangle>(elementHandle);
            if (!shape)
            {
                DebugLog(
                    DBG_WARNING,
                    L"[TaskbarCompat][VTC] ApplyRectangle: shape null (elem=%llu)",
                    static_cast<unsigned long long>(elementHandle));
                return false;
            }

            if (isBackgroundFill)
                m_registry->RegisterBackgroundFill(frameHandle, shape);
            else
                m_registry->RegisterBackgroundStroke(frameHandle, shape);
            return true;
        }
        catch (const winrt::hresult_error& ex)
        {
            DebugLog(
                DBG_ERROR,
                L"[TaskbarCompat][VTC] ApplyRectangle exception: hr=0x%08X msg=%s",
                static_cast<unsigned>(ex.code()),
                ex.message().c_str());
        }
        catch (...)
        {
            DebugLog(DBG_ERROR, L"[TaskbarCompat][VTC] ApplyRectangle unknown exception");
        }
        return false;
    }

    HRESULT STDMETHODCALLTYPE TaskbarVisualTreeWatcher::OnVisualTreeChange(
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
            const DWORD code = GetExceptionCode();
            const wchar_t* stage = g_vtcStage ? g_vtcStage : L"(null)";
            m_diagUsable.store(false, std::memory_order_release);

            // Синхронный путь: pipe-клиент может не успеть сбросить буфер до завершения процесса.
            wchar_t synBuf[256]{};
            swprintf_s(
                synBuf,
                L"[TaskbarCompat][VTC] SEH code=0x%08X stage=%ls\n",
                static_cast<unsigned>(code),
                stage);
            OutputDebugStringW(synBuf);

            DebugLog(
                DBG_CRITICAL,
                L"[TaskbarCompat][VTC] SEH in OnVisualTreeChange code=0x%08X stage=%ls",
                static_cast<unsigned>(code),
                stage);
            return E_FAIL;
        }
    }

    HRESULT TaskbarVisualTreeWatcher::OnVisualTreeChangeImpl(
        ParentChildRelation relation,
        VisualElement element,
        VisualMutationType mutationType) noexcept
    {
        try
        {
            g_vtcStage = L"dispatch";
            if (mutationType == Add)
            {
                const std::wstring_view type{ element.Type, SysStringLen(element.Type) };

                // 1) DesktopWindowXamlSource — игнорируем (HWND острова не нужен; см. ниже).

                // 2) Taskbar.TaskbarFrame — игнорируем.
                //
                // Раньше здесь сопоставлялся DWXS → HWND острова через FromHandle / Content() /
                // IDesktopWindowXamlSourceNative::get_WindowHandle. На Win11 24H2 эта связка
                // детерминированно роняла explorer.exe сразу после Add-мутации TaskbarFrame
                // (handle ещё не стабилен / Content race). HWND острова в TaskbarCompat не
                // используется (SWCA / эффекты задаёт GlassCore на Shell_TrayWnd). Регистрация
                // фрейма выполняется upsert-ом из ветки Rectangle ниже.
                if (type == L"Taskbar.TaskbarFrame")
                {
                    g_vtcStage = L"TaskbarFrame:skip";
                    DebugLog(
                        DBG_NORMAL,
                        L"[TaskbarCompat][VTC] TaskbarFrame found (skip): handle=%llu parent=%llu",
                        static_cast<unsigned long long>(element.Handle),
                        static_cast<unsigned long long>(relation.Parent));
                    RetryPendingRectanglePatches();
                    return S_OK;
                }

                // 4) InputSwitcher (Shell_InputSwitchTopLevelWindow) — Windhawk parity.
                // FrameworkElement: copy_from_abi без QI; Control-методы не нужны.
                if (type == L"WindowsInternal.ComposableShell.Experiences.TextInput.Common.InputSwitcher"
                    || type.ends_with(L".InputSwitcher"))
                {
                    g_vtcStage = L"InputSwitch:Control";
                    const auto root = FromHandle<wux::FrameworkElement>(element.Handle);
                    if (root && m_inputSwitchRegistry)
                        m_inputSwitchRegistry->RegisterPanel(element.Handle, root);
                    return S_OK;
                }

                // 5) Tray overflow (TopLevelWindowForOverflowXamlIsland) — Windhawk parity:
                //    SystemTray.NotificationAreaOverflow > Grid#OverflowRootGrid
                //    > Border#OverflowFlyoutBackgroundBorder.
                if (type == L"SystemTray.NotificationAreaOverflow"
                    || type.ends_with(L".NotificationAreaOverflow"))
                {
                    g_vtcStage = L"Overflow:Anchor";
                    const auto root = FromHandle<wux::FrameworkElement>(element.Handle);
                    if (root && m_overflowRegistry)
                        m_overflowRegistry->RegisterPanel(element.Handle, root);
                    return S_OK;
                }
                if (type == L"Windows.UI.Xaml.Controls.Border")
                {
                    const std::wstring_view name{ element.Name, SysStringLen(element.Name) };
                    if (name == L"OverflowFlyoutBackgroundBorder")
                    {
                        g_vtcStage = L"Overflow:Border";
                        const auto border = FromHandle<wux::Controls::Border>(element.Handle);
                        if (border && m_overflowRegistry)
                            m_overflowRegistry->RegisterBackgroundBorder(element.Handle, border);
                        return S_OK;
                    }
                    // Windhawk chrome: Grid#HoverFlyoutGrid > Border#HoverFlyoutBackground.
                    if (name == L"HoverFlyoutBackground")
                    {
                        g_vtcStage = L"HoverFlyout:Border";
                        const auto border = FromHandle<wux::Controls::Border>(element.Handle);
                        if (border && m_hoverFlyoutRegistry)
                            m_hoverFlyoutRegistry->RegisterBackgroundBorder(element.Handle, border);
                        return S_OK;
                    }
                }
                if (type == L"Windows.UI.Xaml.Controls.Grid"
                    || type.ends_with(L".Grid"))
                {
                    const std::wstring_view name{ element.Name, SysStringLen(element.Name) };
                    if (name == L"HoverFlyoutGrid")
                    {
                        g_vtcStage = L"HoverFlyout:Grid";
                        const auto grid = FromHandle<wux::Controls::Grid>(element.Handle);
                        if (grid && m_hoverFlyoutRegistry)
                            m_hoverFlyoutRegistry->RegisterChromeGrid(element.Handle, grid);
                        return S_OK;
                    }
                }

                // HoverFlyout якорь / прогрев Matter Fill DP с tray TaskbarBackground.
                if (type == L"Taskbar.TaskbarBackground" || type.ends_with(L".TaskbarBackground"))
                {
                    const std::wstring_view name{ element.Name, SysStringLen(element.Name) };
                    if (name == L"HoverFlyoutBackgroundControl")
                    {
                        g_vtcStage = L"HoverFlyout:Anchor";
                        const auto root = FromHandle<wux::FrameworkElement>(element.Handle);
                        if (root && m_hoverFlyoutRegistry)
                            m_hoverFlyoutRegistry->RegisterPanel(element.Handle, root);
                        return S_OK;
                    }
                }

                // 6) Taskbar ToolTip (Xaml_WindowedPopupClass host) — Tray\Xaml\* chrome.
                if (type == L"Windows.UI.Xaml.Controls.ToolTip"
                    || type.ends_with(L".ToolTip"))
                {
                    g_vtcStage = L"Tooltip:Control";
                    const auto root = FromHandle<wux::FrameworkElement>(element.Handle);
                    if (root && m_tooltipRegistry)
                        m_tooltipRegistry->RegisterPanel(element.Handle, root);
                    return S_OK;
                }

                // 7) Taskbar / Start context menu — MenuFlyoutPresenter > Border.
                //    GetElementVisual на Presenter ломает hit-test (Windhawk #2147).
                if (type == L"Windows.UI.Xaml.Controls.MenuFlyoutPresenter"
                    || type.ends_with(L".MenuFlyoutPresenter"))
                {
                    g_vtcStage = L"MenuFlyout:Presenter";
                    const auto root = FromHandle<wux::FrameworkElement>(element.Handle);
                    if (root && m_menuFlyoutRegistry)
                        m_menuFlyoutRegistry->RegisterPanel(element.Handle, root);
                    return S_OK;
                }

                // 3) Rectangle name=BackgroundFill / BackgroundStroke.
                if (type == L"Windows.UI.Xaml.Shapes.Rectangle")
                {
                    g_vtcStage = L"Rectangle:parse";
                    const std::wstring_view name{ element.Name, SysStringLen(element.Name) };
                    const bool isBackgroundFill   = (name == L"BackgroundFill");
                    const bool isBackgroundStroke = (name == L"BackgroundStroke");

                    if (!(isBackgroundFill || isBackgroundStroke))
                    {
                        // Событийный ретрай InputSwitch/Tooltip: поддерево строится десятками Add.
                        // Overflow: НЕ ретраим на каждом Add — SizeChanged / Border Add.
                        if (m_inputSwitchRegistry
                            && m_inputSwitchRegistry->PanelCount() > 0)
                        {
                            g_vtcStage = L"InputSwitch:Retry";
                            m_inputSwitchRegistry->RetryUnresolvedPanels();
                        }
                        if (m_tooltipRegistry && m_tooltipRegistry->PanelCount() > 0)
                        {
                            g_vtcStage = L"Tooltip:Retry";
                            m_tooltipRegistry->RetryUnresolvedPanels();
                        }
                        if (m_menuFlyoutRegistry && m_menuFlyoutRegistry->PanelCount() > 0)
                        {
                            g_vtcStage = L"MenuFlyout:Retry";
                            m_menuFlyoutRegistry->RetryUnresolvedPanels();
                        }
                        return S_OK;
                    }

                    DebugLog(
                        DBG_NORMAL,
                        L"[TaskbarCompat][VTC] Rectangle candidate: name=%ls handle=%llu parent=%llu",
                        name.data(),
                        static_cast<unsigned long long>(element.Handle),
                        static_cast<unsigned long long>(relation.Parent));

                    // HoverFlyout Fill — только TryAttachFill от якоря (GetParent здесь → SEH).

                    if (TaskbarTapSite_PostVtcRectangleAdd(
                            this,
                            static_cast<std::uint64_t>(relation.Parent),
                            static_cast<std::uint64_t>(element.Handle),
                            isBackgroundFill))
                    {
                        return S_OK;
                    }

                    (void)ApplyRectangleAddFromHandles(relation.Parent, element.Handle, isBackgroundFill);
                    // После патча трея — ретрай InputSwitch/Tooltip (layout мог вырасти).
                    if (m_inputSwitchRegistry && m_inputSwitchRegistry->PanelCount() > 0)
                    {
                        g_vtcStage = L"InputSwitch:Retry";
                        m_inputSwitchRegistry->RetryUnresolvedPanels();
                    }
                    if (m_tooltipRegistry && m_tooltipRegistry->PanelCount() > 0)
                    {
                        g_vtcStage = L"Tooltip:Retry";
                        m_tooltipRegistry->RetryUnresolvedPanels();
                    }
                    if (m_menuFlyoutRegistry && m_menuFlyoutRegistry->PanelCount() > 0)
                    {
                        g_vtcStage = L"MenuFlyout:Retry";
                        m_menuFlyoutRegistry->RetryUnresolvedPanels();
                    }
                    return S_OK;
                }

                // Событийный ретрай InputSwitch/Tooltip/MenuFlyout/HoverFlyout на любом Add.
                // Overflow — только SizeChanged / собственный Register (не каждый VTC Add).
                if (m_inputSwitchRegistry && m_inputSwitchRegistry->PanelCount() > 0)
                {
                    g_vtcStage = L"InputSwitch:Retry";
                    m_inputSwitchRegistry->RetryUnresolvedPanels();
                }
                if (m_tooltipRegistry && m_tooltipRegistry->PanelCount() > 0)
                {
                    g_vtcStage = L"Tooltip:Retry";
                    m_tooltipRegistry->RetryUnresolvedPanels();
                }
                if (m_menuFlyoutRegistry && m_menuFlyoutRegistry->PanelCount() > 0)
                {
                    g_vtcStage = L"MenuFlyout:Retry";
                    m_menuFlyoutRegistry->RetryUnresolvedPanels();
                }
                // HoverFlyout: без Retry на каждом Add (флуд deferred + SEH на walk).

                return S_OK;
            }

            if (mutationType == Remove)
            {
                g_vtcStage = L"Remove";
                m_registry->UnregisterFrame(element.Handle);
                if (m_inputSwitchRegistry)
                    m_inputSwitchRegistry->Unregister(element.Handle);
                if (m_overflowRegistry)
                    m_overflowRegistry->Unregister(element.Handle);
                if (m_hoverFlyoutRegistry)
                    m_hoverFlyoutRegistry->Unregister(element.Handle);
                if (m_tooltipRegistry)
                    m_tooltipRegistry->Unregister(element.Handle);
                if (m_menuFlyoutRegistry)
                    m_menuFlyoutRegistry->Unregister(element.Handle);
                return S_OK;
            }

            return S_OK;
        }
        catch (const winrt::hresult_error& ex)
        {
            DebugLog(
                DBG_ERROR,
                L"[TaskbarCompat][VTC] OnVisualTreeChange exception: hr=0x%08X msg=%s",
                static_cast<unsigned>(ex.code()),
                ex.message().c_str());
            return ex.code();
        }
        catch (...)
        {
            DebugLog(DBG_ERROR, L"[TaskbarCompat][VTC] OnVisualTreeChange unknown exception");
            return E_FAIL;
        }
    }

    HRESULT STDMETHODCALLTYPE TaskbarVisualTreeWatcher::OnElementStateChanged(
        InstanceHandle /*element*/,
        VisualElementState /*elementState*/,
        LPCWSTR /*context*/) noexcept
    {
        return S_OK;
    }

    void TaskbarVisualTreeWatcher::RestoreAllFills() noexcept
    {
        // IXDE/diagnostics на shutdown могут быть уже невалидны — не трогаем FromHandle.
        m_diagUsable.store(false, std::memory_order_release);
        if (m_registry)
            m_registry->RestoreAll();
        if (m_inputSwitchRegistry)
            m_inputSwitchRegistry->RestoreAll();
        if (m_overflowRegistry)
            m_overflowRegistry->RestoreAll();
        if (m_hoverFlyoutRegistry)
            m_hoverFlyoutRegistry->RestoreAll();
        if (m_tooltipRegistry)
            m_tooltipRegistry->RestoreAll();
        if (m_menuFlyoutRegistry)
            m_menuFlyoutRegistry->RestoreAll();
    }

    void TaskbarVisualTreeWatcher::ReapplyPatchedBrushes() noexcept
    {
        if (m_registry)
            m_registry->ReapplyPatchedBrushes();
        if (m_inputSwitchRegistry)
            m_inputSwitchRegistry->ReapplyPatchedBrushes();
        if (m_overflowRegistry)
            m_overflowRegistry->ReapplyPatchedBrushes();
        if (m_hoverFlyoutRegistry)
            m_hoverFlyoutRegistry->ReapplyPatchedBrushes();
        if (m_tooltipRegistry)
            m_tooltipRegistry->ReapplyPatchedBrushes();
        if (m_menuFlyoutRegistry)
            m_menuFlyoutRegistry->ReapplyPatchedBrushes();
        RetryPendingRectanglePatches();
    }

    void TaskbarVisualTreeWatcher::RecaptureAfterThemeChange(Registry::Settings barSettings) noexcept
    {
        if (m_registry)
            m_registry->RecaptureAfterThemeChange(barSettings);
        if (m_inputSwitchRegistry)
            m_inputSwitchRegistry->RecaptureAfterThemeChange(
                Registry::ReadSurface(Registry::SurfaceKind::Popup));
        if (m_overflowRegistry)
            m_overflowRegistry->RecaptureAfterThemeChange(
                Registry::ReadSurface(Registry::SurfaceKind::Popup));
        if (m_hoverFlyoutRegistry)
            m_hoverFlyoutRegistry->RecaptureAfterThemeChange(
                Registry::ReadSurface(Registry::SurfaceKind::Preview));
        if (m_tooltipRegistry)
            m_tooltipRegistry->RecaptureAfterThemeChange(
                Registry::ReadSurface(Registry::SurfaceKind::Tooltip));
        if (m_menuFlyoutRegistry)
            m_menuFlyoutRegistry->RecaptureAfterThemeChange(
                Registry::ReadSurface(Registry::SurfaceKind::Menu));
        RetryPendingRectanglePatches();
    }

    void TaskbarVisualTreeWatcher::UpdateSettings(Registry::Settings settings) noexcept
    {
        // settings = Bar (Tray\Xaml). Смежные — per-surface.
        if (m_registry)
            m_registry->UpdateSettings(settings);
        if (m_inputSwitchRegistry)
            m_inputSwitchRegistry->UpdateSettings(
                Registry::ReadSurface(Registry::SurfaceKind::Popup));
        if (m_overflowRegistry)
            m_overflowRegistry->UpdateSettings(
                Registry::ReadSurface(Registry::SurfaceKind::Popup));
        if (m_hoverFlyoutRegistry)
            m_hoverFlyoutRegistry->UpdateSettings(
                Registry::ReadSurface(Registry::SurfaceKind::Preview));
        if (m_tooltipRegistry)
            m_tooltipRegistry->UpdateSettings(
                Registry::ReadSurface(Registry::SurfaceKind::Tooltip));
        if (m_menuFlyoutRegistry)
            m_menuFlyoutRegistry->UpdateSettings(
                Registry::ReadSurface(Registry::SurfaceKind::Menu));
    }
}
