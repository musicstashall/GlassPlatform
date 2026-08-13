#include "UiPch.h"
#include "xaml/SearchWebViewPanelRegistry.hpp"

#include <RootBackdropPolicy.h>
#include <TrayXamlPaint.h>
#include <ThemeHelper.h>
#include <DebugLog.h>

#include <sstream>
#include <string>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.Controls.h>

namespace UICompat
{
    namespace
    {
        // Windhawk windows-11-start-menu-styler.wh.cpp
        constexpr winrt::guid kIID_WebView2Standalone_IWebView2{
            0x59C47E46,
            0xCC96,
            0x525F,
            { 0xA1, 0x7C, 0x2C, 0x21, 0x3F, 0x98, 0x84, 0x47 } };

        constexpr winrt::guid kIID_ICoreWebView2NavigationCompletedEventArgs{
            0x4865E238,
            0x036A,
            0x5664,
            { 0x95, 0xA3, 0x44, 0x7E, 0xC4, 0x4C, 0xF4, 0x98 } };

        // Порядок методов = донор (vtable). Не укорачивать.
        struct WebView2Standalone_IWebView2 : ::IInspectable
        {
            virtual int32_t __stdcall get_CoreWebView2(void**) noexcept = 0;
            virtual int32_t __stdcall get_CoreWebView2Controller(void**) noexcept = 0;
            virtual int32_t __stdcall EnsureCoreWebView2Async(void**) noexcept = 0;
            virtual int32_t __stdcall ExecuteScriptAsync(void*, void**) noexcept = 0;
            virtual int32_t __stdcall get_Source(void**) noexcept = 0;
            virtual int32_t __stdcall put_Source(void*) noexcept = 0;
            virtual int32_t __stdcall get_CanGoForward(bool*) noexcept = 0;
            virtual int32_t __stdcall put_CanGoForward(bool) noexcept = 0;
            virtual int32_t __stdcall get_CanGoBack(bool*) noexcept = 0;
            virtual int32_t __stdcall put_CanGoBack(bool) noexcept = 0;
            virtual int32_t __stdcall Reload() noexcept = 0;
            virtual int32_t __stdcall GoForward() noexcept = 0;
            virtual int32_t __stdcall GoBack() noexcept = 0;
            virtual int32_t __stdcall NavigateToString(void*) noexcept = 0;
            virtual int32_t __stdcall Close() noexcept = 0;
            virtual int32_t __stdcall add_NavigationCompleted(
                void*, winrt::event_token*) noexcept = 0;
            virtual int32_t __stdcall remove_NavigationCompleted(
                winrt::event_token) noexcept = 0;
            virtual int32_t __stdcall add_WebMessageReceived(
                void*, winrt::event_token*) noexcept = 0;
            virtual int32_t __stdcall remove_WebMessageReceived(
                winrt::event_token) noexcept = 0;
            virtual int32_t __stdcall add_NavigationStarting(
                void*, winrt::event_token*) noexcept = 0;
            virtual int32_t __stdcall remove_NavigationStarting(
                winrt::event_token) noexcept = 0;
            virtual int32_t __stdcall add_CoreProcessFailed(
                void*, winrt::event_token*) noexcept = 0;
            virtual int32_t __stdcall remove_CoreProcessFailed(
                winrt::event_token) noexcept = 0;
            virtual int32_t __stdcall add_CoreWebView2Initialized(
                void*, winrt::event_token*) noexcept = 0;
            virtual int32_t __stdcall remove_CoreWebView2Initialized(
                winrt::event_token) noexcept = 0;
        };

        struct ICoreWebView2NavigationCompletedEventArgs : ::IInspectable
        {
            virtual int32_t __stdcall get_IsSuccess(bool*) noexcept = 0;
            virtual int32_t __stdcall get_WebErrorStatus(int32_t*) noexcept = 0;
            virtual int32_t __stdcall get_NavigationId(uint64_t*) noexcept = 0;
        };

        [[nodiscard]] std::wstring EscapeJsTemplateString(std::wstring_view str) noexcept
        {
            std::wstring buffer;
            buffer.reserve(str.size());
            for (const auto c : str)
            {
                switch (c)
                {
                case L'\\':
                    buffer.append(L"\\\\");
                    break;
                case L'`':
                    buffer.append(L"\\`");
                    break;
                default:
                    buffer.push_back(c);
                    break;
                }
            }
            return buffer;
        }

        [[nodiscard]] std::wstring CssColorFromArgb(DWORD argb) noexcept
        {
            const unsigned a = (argb >> 24) & 0xFFu;
            const unsigned r = (argb >> 16) & 0xFFu;
            const unsigned g = (argb >> 8) & 0xFFu;
            const unsigned b = argb & 0xFFu;
            wchar_t buf[64]{};
            swprintf_s(buf, L"rgba(%u,%u,%u,%.3f)", r, g, b, a / 255.0);
            return buf;
        }

        // WebView-меню Search: фон/frosted системные. Бордюр штатно часто через
        // box-shadow + ::before — наш border поверх давал «двойную» рамку (H-B8).
        // Как донор: box-shadow:none + явный border; radius и на ul::before
        // (без display:none / без background).
        [[nodiscard]] std::wstring BuildWebContentCss(
            const Registry::Settings& settings) noexcept
        {
            const DWORD effect = settings.xamlEffectType;
            // 0 Disable / 5 System — не трогаем меню (как XamlBackdropFactory).
            if (effect == 0 || effect == 5)
                return {};

            const bool wantCorners =
                settings.cornersPresent && settings.cornerType != 0;
            const bool wantBorder =
                settings.patchBorder && !settings.noBorder && settings.borderArgb != 0;
            // Углы / наш border / noBorder — снять системный box-shadow+border.
            if (!wantCorners && !wantBorder && !settings.noBorder)
                return {};

            const double trayH = Shared::TrayXamlPaint::QueryTaskbarHeightDip();
            const double radius = wantCorners
                ? Shared::TrayXamlPaint::XamlPopupCornerRadiusDip(
                      settings.cornerType, trayH, 0.0)
                : 0.0;
            const std::wstring borderCol =
                wantBorder ? CssColorFromArgb(settings.borderArgb) : L"";

            const wchar_t* menuSel =
                L"#root.darkTheme:not(.fileExplorer) .contextMenu,"
                L"#root:not(.fileExplorer) .contextMenu,"
                L".darkTheme #menuContainer,"
                L".lightTheme #menuContainer,"
                L".contextMenu,#menuContainer";

            std::wostringstream css;
            css << menuSel << L"{"
                // Снять системный контур (донор: box-shadow:none на #menuContainer).
                << L"box-shadow:none !important;"
                << L"outline:none !important;"
                << L"-webkit-box-shadow:none !important;";
            if (wantCorners)
                css << L"border-radius:" << radius << L"px !important;";
            if (wantBorder)
                css << L"border:1px solid " << borderCol << L" !important;";
            else
                css << L"border:none !important;"
                    << L"border-width:0 !important;"
                    << L"border-color:transparent !important;";
            css << L"}";

            // Frosted-слой: тот же радиус, без своей рамки; фон не трогаем.
            css << L"ul.contextMenu::before{"
                << L"box-shadow:none !important;"
                << L"outline:none !important;"
                << L"border:none !important;";
            if (wantCorners)
                css << L"border-radius:" << radius << L"px !important;";
            css << L"}";

            return css.str();
        }

        [[nodiscard]] std::wstring CreateWebViewJsCodeForClear() noexcept
        {
            // Снять хвосты отклонённого polyfill (H-B5).
            return LR"(
        (() => {
        try {
          if (window.__glassUiMenuObs) {
            window.__glassUiMenuObs.disconnect();
            window.__glassUiMenuObs = null;
          }
        } catch (e) {}
        try {
          if (window.__glassUiMenuRaf) {
            cancelAnimationFrame(window.__glassUiMenuRaf);
            window.__glassUiMenuRaf = 0;
          }
        } catch (e) {}
        try {
          document.querySelectorAll('.glass-uicompat-blur-clip,.glass-uicompat-blur').forEach(n => n.remove());
        } catch (e) {}
        const styleElementId = "glass-uicompat-search-style";
        const style = document.getElementById(styleElementId);
        if (style && style.parentNode) {
            style.parentNode.removeChild(style);
        }
        })();
)";
        }

        // Только inject <style> (донорский путь). Без MutationObserver / clone-blur.
        [[nodiscard]] std::wstring CreateWebViewJsCodeForApply(
            const Registry::Settings& settings) noexcept
        {
            const std::wstring css = BuildWebContentCss(settings);
            if (css.empty())
                return CreateWebViewJsCodeForClear();

            std::wstring jsCode =
                LR"(
        (() => {
        try {
          if (window.__glassUiMenuObs) {
            window.__glassUiMenuObs.disconnect();
            window.__glassUiMenuObs = null;
          }
        } catch (e) {}
        try {
          document.querySelectorAll('.glass-uicompat-blur-clip,.glass-uicompat-blur').forEach(n => n.remove());
        } catch (e) {}
        const styleElementId = "glass-uicompat-search-style";
        const styleContent = `
)";
            jsCode += EscapeJsTemplateString(css);
            jsCode +=
                LR"(
`;
        let style = document.getElementById(styleElementId);
        if (!style) {
            style = document.createElement("style");
            style.id = styleElementId;
            (document.head || document.documentElement).appendChild(style);
        }
        style.textContent = styleContent;
        })();
)";
            return jsCode;
        }

        [[nodiscard]] bool IsAllowedWebViewUri(std::wstring_view canonicalUri) noexcept
        {
            return canonicalUri == L"https://www.bing.com/WS/Init"
                || canonicalUri
                    == L"ms-appx-web://microsoftwindows.client.cbs/Cortana.UI/cache/"
                       L"svlocal/desktop/2.html"
                || canonicalUri
                    == L"https://www.bing.com/AS/API/WindowsCortanaPane/V2/Init"
                || canonicalUri
                    == L"ms-appx-web://microsoft.windows.search/cache/local/desktop/"
                       L"2.html"
                || canonicalUri
                    == L"https://searchapp.bundleassets.example/desktop/2.html";
        }

        bool ApplyWebViewStyleCustomizations(
            winrt::Windows::UI::Xaml::Controls::WebView const& webViewElement,
            const Registry::Settings& settings) noexcept
        {
            try
            {
                const auto source = webViewElement.Source();
                if (!source)
                    return false;
                const auto canonicalUri = source.AbsoluteCanonicalUri();
                if (!IsAllowedWebViewUri(canonicalUri))
                {
                    return false;
                }
                const auto jsCode = CreateWebViewJsCodeForApply(settings);
                webViewElement.InvokeScriptAsync(
                    L"eval",
                    winrt::single_threaded_vector<winrt::hstring>(
                        { winrt::hstring(
                            jsCode.c_str(),
                            static_cast<winrt::hstring::size_type>(jsCode.size())) }));
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        bool ApplyWebView2StyleCustomizations(
            WebView2Standalone_IWebView2* webViewElement,
            const Registry::Settings& settings) noexcept
        {
            if (!webViewElement)
                return false;
            try
            {
                void* sourcePtr = nullptr;
                if (FAILED(webViewElement->get_Source(&sourcePtr)) || !sourcePtr)
                    return false;
                const auto source = winrt::Windows::Foundation::Uri{
                    sourcePtr, winrt::take_ownership_from_abi };
                if (!source)
                    return false;
                const auto canonicalUri = source.AbsoluteCanonicalUri();
                if (!IsAllowedWebViewUri(canonicalUri))
                {
                    return false;
                }

                const auto jsCode = CreateWebViewJsCodeForApply(settings);
                void* operationPtr = nullptr;
                auto jsCodeHstring = winrt::hstring(
                    jsCode.c_str(),
                    static_cast<winrt::hstring::size_type>(jsCode.size()));
                if (FAILED(webViewElement->ExecuteScriptAsync(
                        *reinterpret_cast<void**>(&jsCodeHstring), &operationPtr)))
                {
                    return false;
                }
                if (operationPtr)
                {
                    winrt::Windows::Foundation::IAsyncOperation<winrt::hstring> operation{
                        operationPtr, winrt::take_ownership_from_abi };
                    (void)operation;
                }
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        bool ClearWebViewStyleCustomizations(
            winrt::Windows::UI::Xaml::Controls::WebView const& webViewElement) noexcept
        {
            try
            {
                const auto jsCode = CreateWebViewJsCodeForClear();
                webViewElement.InvokeScriptAsync(
                    L"eval",
                    winrt::single_threaded_vector<winrt::hstring>(
                        { winrt::hstring(jsCode) }));
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        bool ClearWebView2StyleCustomizations(
            WebView2Standalone_IWebView2* webViewElement) noexcept
        {
            if (!webViewElement)
                return false;
            try
            {
                const auto jsCode = CreateWebViewJsCodeForClear();
                void* operationPtr = nullptr;
                auto jsCodeHstring = winrt::hstring(jsCode);
                if (FAILED(webViewElement->ExecuteScriptAsync(
                        *reinterpret_cast<void**>(&jsCodeHstring), &operationPtr)))
                {
                    return false;
                }
                if (operationPtr)
                {
                    winrt::Windows::Foundation::IAsyncOperation<winrt::hstring> operation{
                        operationPtr, winrt::take_ownership_from_abi };
                    (void)operation;
                }
                return true;
            }
            catch (...)
            {
                return false;
            }
        }
    }

    SearchWebViewPanelRegistry::SearchWebViewPanelRegistry(
        Registry::Settings settings) noexcept
        : m_settings(settings)
    {
    }

    unsigned SearchWebViewPanelRegistry::PanelCount() const noexcept
    {
        std::lock_guard lock(m_mx);
        return static_cast<unsigned>(m_panels.size());
    }

    void SearchWebViewPanelRegistry::ApplyCss(PanelSlot& slot) noexcept
    {
        if (m_shutdown->load(std::memory_order_acquire) || !slot.element)
            return;

        const Registry::Settings settings = [this]() noexcept
        {
            std::lock_guard lock(m_mx);
            return m_settings;
        }();

        if (!settings.paintPathXaml)
        {
            ClearCss(slot);
            return;
        }

        bool ok = false;
        if (slot.isWebView2)
        {
            winrt::com_ptr<WebView2Standalone_IWebView2> wv;
            if (SUCCEEDED(
                    reinterpret_cast<::IUnknown*>(winrt::get_abi(slot.element))
                        ->QueryInterface(
                            kIID_WebView2Standalone_IWebView2, wv.put_void())))
            {
                ok = ApplyWebView2StyleCustomizations(wv.get(), settings);
            }
        }
        else
        {
            try
            {
                if (const auto legacy =
                        slot.element.try_as<winrt::Windows::UI::Xaml::Controls::WebView>())
                {
                    ok = ApplyWebViewStyleCustomizations(legacy, settings);
                }
            }
            catch (...)
            {
                ok = false;
            }
        }

        slot.cssApplied = ok;
    }

    void SearchWebViewPanelRegistry::ClearCss(PanelSlot& slot) noexcept
    {
        if (!slot.element || !slot.cssApplied)
        {
            slot.cssApplied = false;
            return;
        }
        if (slot.isWebView2)
        {
            winrt::com_ptr<WebView2Standalone_IWebView2> wv;
            if (SUCCEEDED(
                    reinterpret_cast<::IUnknown*>(winrt::get_abi(slot.element))
                        ->QueryInterface(
                            kIID_WebView2Standalone_IWebView2, wv.put_void())))
            {
                (void)ClearWebView2StyleCustomizations(wv.get());
            }
        }
        else
        {
            try
            {
                if (const auto legacy =
                        slot.element.try_as<winrt::Windows::UI::Xaml::Controls::WebView>())
                {
                    (void)ClearWebViewStyleCustomizations(legacy);
                }
            }
            catch (...)
            {
            }
        }
        slot.cssApplied = false;
    }

    void SearchWebViewPanelRegistry::ScheduleFinishRegister(
        std::shared_ptr<PanelSlot> slot) noexcept
    {
        if (!slot || !slot->element)
            return;
        if (slot->finishScheduled)
            return;
        slot->finishScheduled = true;

        try
        {
            const auto dispatcher = slot->element.Dispatcher();
            SearchWebViewPanelRegistry* self = this;
            std::weak_ptr<PanelSlot> weakSlot = slot;
            auto work = [self, weakSlot]()
            {
                if (auto s = weakSlot.lock())
                    self->FinishRegisterWebView(s);
            };
            if (!dispatcher)
            {
                work();
                return;
            }
            // Всегда Post: даже HasThreadAccess на Advise → AV WebView2Standalone.
            (void)dispatcher.RunAsync(
                winrt::Windows::UI::Core::CoreDispatcherPriority::Low,
                winrt::Windows::UI::Core::DispatchedHandler(work));
        }
        catch (...)
        {
            slot->finishScheduled = false;
        }
    }

    void SearchWebViewPanelRegistry::FinishRegisterWebView(
        std::shared_ptr<PanelSlot> slot) noexcept
    {
        if (!slot || !slot->element)
            return;
        if (m_shutdown->load(std::memory_order_acquire))
            return;

        try
        {
            if (!slot->isWebView2)
            {
                ApplyCss(*slot);
                if (!slot->navHooked)
                {
                    auto shut = m_shutdown;
                    std::weak_ptr<PanelSlot> weakSlot = slot;
                    SearchWebViewPanelRegistry* self = this;
                    const auto legacy =
                        slot->element.as<winrt::Windows::UI::Xaml::Controls::WebView>();
                    slot->navToken = legacy.NavigationCompleted(
                        [shut, weakSlot, self](
                            winrt::Windows::UI::Xaml::Controls::WebView const& sender,
                            winrt::Windows::UI::Xaml::Controls::
                                WebViewNavigationCompletedEventArgs const& args)
                        {
                            if (shut->load(std::memory_order_acquire))
                                return;
                            if (!args.IsSuccess())
                                return;
                            if (auto s = weakSlot.lock())
                            {
                                s->element = sender;
                                self->ApplyCss(*s);
                            }
                        });
                    slot->navHooked = true;
                }
                return;
            }

            winrt::com_ptr<WebView2Standalone_IWebView2> webViewElement;
            if (FAILED(
                    reinterpret_cast<::IUnknown*>(winrt::get_abi(slot->element))
                        ->QueryInterface(
                            kIID_WebView2Standalone_IWebView2,
                            webViewElement.put_void()))
                || !webViewElement)
            {
                return;
            }

            ApplyCss(*slot);

            if (!slot->navHooked)
            {
                auto shut = m_shutdown;
                std::weak_ptr<PanelSlot> weakSlot = slot;
                SearchWebViewPanelRegistry* self = this;
                winrt::Windows::Foundation::TypedEventHandler<
                    winrt::Windows::Foundation::IInspectable,
                    winrt::Windows::Foundation::IInspectable>
                    eventHandler =
                        [shut, weakSlot, self](
                            winrt::Windows::Foundation::IInspectable const& sender,
                            winrt::Windows::Foundation::IInspectable const& args)
                {
                    if (shut->load(std::memory_order_acquire))
                        return;
                    try
                    {
                        winrt::com_ptr<ICoreWebView2NavigationCompletedEventArgs>
                            webViewArgs;
                        if (FAILED(
                                reinterpret_cast<::IUnknown*>(winrt::get_abi(args))
                                    ->QueryInterface(
                                        kIID_ICoreWebView2NavigationCompletedEventArgs,
                                        webViewArgs.put_void()))
                            || !webViewArgs)
                        {
                            return;
                        }
                        bool success = false;
                        if (FAILED(webViewArgs->get_IsSuccess(&success)) || !success)
                            return;

                        if (auto s = weakSlot.lock())
                        {
                            if (const auto fe = sender.try_as<
                                    winrt::Windows::UI::Xaml::FrameworkElement>())
                            {
                                s->element = fe;
                            }
                            self->ApplyCss(*s);
                        }
                    }
                    catch (...)
                    {
                    }
                };

                winrt::event_token tok{};
                if (SUCCEEDED(webViewElement->add_NavigationCompleted(
                        *reinterpret_cast<void**>(&eventHandler), &tok)))
                {
                    slot->navToken = tok;
                    slot->navHooked = true;
                }
            }
        }
        catch (const winrt::hresult_error& ex)
        {
            DebugLog(
                DBG_WARNING,
                L"[UICompat][SearchWebView] FinishRegister hr=0x%08X msg=%s",
                static_cast<unsigned>(ex.code()),
                ex.message().c_str());
        }
        catch (...)
        {
        }
    }

    void SearchWebViewPanelRegistry::TryRegisterIfWebView(
        InstanceHandle handle,
        winrt::Windows::UI::Xaml::FrameworkElement element) noexcept
    {
        if (!element || handle == 0)
            return;
        if (!Shared::RootBackdropPolicy::IsSearchHostProcess())
            return;
        if (m_shutdown->load(std::memory_order_acquire))
            return;

        try
        {
            // Донор: только get_class_name, не VTC Type / HostedWebView*.
            const auto className = winrt::get_class_name(element);

            const bool isLegacy = className == L"Windows.UI.Xaml.Controls.WebView";
            const bool isWv2 = className == L"WebView2Standalone.Controls.WebView2";
            if (!isLegacy && !isWv2)
                return;

            std::shared_ptr<PanelSlot> slot;
            {
                std::lock_guard lock(m_mx);
                auto& ref = m_panels[handle];
                if (ref && ref->element)
                    return; // уже зарегистрирован (донор)
                if (!ref)
                    ref = std::make_shared<PanelSlot>();
                slot = ref;
                slot->element = element;
                slot->handle = handle;
                slot->isWebView2 = isWv2;
            }

            // QI/ExecuteScript/add_NavigationCompleted — после выхода из Advise.
            ScheduleFinishRegister(slot);
        }
        catch (const winrt::hresult_error& ex)
        {
            DebugLog(
                DBG_WARNING,
                L"[UICompat][SearchWebView] Register hr=0x%08X msg=%s",
                static_cast<unsigned>(ex.code()),
                ex.message().c_str());
        }
        catch (...)
        {
        }
    }

    void SearchWebViewPanelRegistry::Unregister(InstanceHandle handle) noexcept
    {
        std::shared_ptr<PanelSlot> slot;
        {
            std::lock_guard lock(m_mx);
            const auto it = m_panels.find(handle);
            if (it == m_panels.end())
                return;
            slot = std::move(it->second);
            m_panels.erase(it);
        }
        if (!slot || !slot->element)
            return;

        // Донор ClearWebViewCustomizations: снять CSS + NavigationCompleted.
        try
        {
            if (!slot->isWebView2)
            {
                if (const auto legacy =
                        slot->element.try_as<winrt::Windows::UI::Xaml::Controls::WebView>())
                {
                    ClearCss(*slot);
                    if (slot->navHooked)
                        legacy.NavigationCompleted(slot->navToken);
                }
            }
            else
            {
                winrt::com_ptr<WebView2Standalone_IWebView2> wv;
                if (SUCCEEDED(
                        reinterpret_cast<::IUnknown*>(winrt::get_abi(slot->element))
                            ->QueryInterface(
                                kIID_WebView2Standalone_IWebView2, wv.put_void()))
                    && wv)
                {
                    ClearCss(*slot);
                    if (slot->navHooked)
                        (void)wv->remove_NavigationCompleted(slot->navToken);
                }
            }
        }
        catch (...)
        {
        }
        slot->navHooked = false;
        slot->element = nullptr;
    }

    void SearchWebViewPanelRegistry::RestoreAll() noexcept
    {
        m_shutdown->store(true, std::memory_order_release);
        std::map<InstanceHandle, std::shared_ptr<PanelSlot>> copy;
        {
            std::lock_guard lock(m_mx);
            copy.swap(m_panels);
        }
        unsigned cleared = 0;
        for (auto& [h, slot] : copy)
        {
            (void)h;
            if (!slot)
                continue;
            if (slot->cssApplied)
                ++cleared;
            // Unregister-логика без повторного erase.
            try
            {
                if (slot->element)
                {
                    if (!slot->isWebView2)
                    {
                        if (const auto legacy = slot->element.try_as<
                                winrt::Windows::UI::Xaml::Controls::WebView>())
                        {
                            ClearCss(*slot);
                            if (slot->navHooked)
                                legacy.NavigationCompleted(slot->navToken);
                        }
                    }
                    else
                    {
                        winrt::com_ptr<WebView2Standalone_IWebView2> wv;
                        if (SUCCEEDED(
                                reinterpret_cast<::IUnknown*>(
                                    winrt::get_abi(slot->element))
                                    ->QueryInterface(
                                        kIID_WebView2Standalone_IWebView2,
                                        wv.put_void()))
                            && wv)
                        {
                            ClearCss(*slot);
                            if (slot->navHooked)
                                (void)wv->remove_NavigationCompleted(slot->navToken);
                        }
                    }
                }
            }
            catch (...)
            {
            }
            slot->navHooked = false;
            slot->element = nullptr;
        }
    }

    void SearchWebViewPanelRegistry::ReapplyPatchedBrushes() noexcept
    {
        if (m_shutdown->load(std::memory_order_acquire))
            return;
        std::vector<std::shared_ptr<PanelSlot>> slots;
        {
            std::lock_guard lock(m_mx);
            slots.reserve(m_panels.size());
            for (auto& [h, s] : m_panels)
            {
                (void)h;
                if (s)
                    slots.push_back(s);
            }
        }
        for (auto& s : slots)
            ApplyCss(*s);
    }

    void SearchWebViewPanelRegistry::UpdateSettings(Registry::Settings settings) noexcept
    {
        {
            std::lock_guard lock(m_mx);
            m_settings = settings;
        }
        ReapplyPatchedBrushes();
    }
}
