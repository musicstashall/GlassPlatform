#include "WebHost.h"

#include "../WizardAssets.h"
#include <objbase.h>
#include <objidl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wrl.h>
#include <WebView2.h>
#include <utility>

namespace WizardUi {
namespace {
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

struct TaskString {
    TaskString() noexcept = default;
    TaskString(const TaskString&) = delete;
    TaskString& operator=(const TaskString&) = delete;
    ~TaskString() noexcept { CoTaskMemFree(value); }
    wchar_t** put() noexcept { return &value; }
    const wchar_t* get() const noexcept { return value; }
    explicit operator bool() const noexcept { return value != nullptr; }
private:
    wchar_t* value = nullptr;
};
}

struct WebHost::Session final : std::enable_shared_from_this<Session> {
    HWND host = nullptr;
    HINSTANCE resources = nullptr;
    Events events{};
    std::wstring document;
    bool suppressBrowserKeys = false;
    bool active = true;
    bool visible = false;
    RECT bounds{};
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> web;
    enum Event { Resource, Navigation, Popup, Permission, Message, Completed, Count };
    EventRegistrationToken tokens[Count]{};
    bool subscribed[Count]{};

    void Close() noexcept {
        active = false;
        if (web) {
            if (subscribed[Resource]) web->remove_WebResourceRequested(tokens[Resource]);
            if (subscribed[Navigation]) web->remove_NavigationStarting(tokens[Navigation]);
            if (subscribed[Popup]) web->remove_NewWindowRequested(tokens[Popup]);
            if (subscribed[Permission]) web->remove_PermissionRequested(tokens[Permission]);
            if (subscribed[Message]) web->remove_WebMessageReceived(tokens[Message]);
            if (subscribed[Completed]) web->remove_NavigationCompleted(tokens[Completed]);
        }
        if (controller) controller->Close();
        web.Reset();
        controller.Reset();
        environment.Reset();
    }

    void Fail(HRESULT hr) {
        if (!active) return;
        Close();
        events.onFailure(events.context, FAILED(hr) ? hr : E_FAIL);
    }

    HRESULT Serve(ICoreWebView2WebResourceRequestedEventArgs* args) {
        ComPtr<ICoreWebView2WebResourceRequest> request;
        HRESULT hr = args->get_Request(&request);
        if (FAILED(hr)) return hr;
        TaskString uri;
        if (FAILED(hr = request->get_Uri(uri.put()))) return hr;
        const auto asset = WizardAssets::Resolve(uri ? uri.get() : L"");
        const bool allowed = asset.id != 0;
        ComPtr<IStream> stream;
        if (allowed) {
            const HRSRC res = FindResourceExW(resources, RT_RCDATA, MAKEINTRESOURCEW(asset.id),
                MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL));
            if (!res) return E_FAIL;
            const auto* bytes = static_cast<const BYTE*>(LockResource(LoadResource(resources, res)));
            const DWORD size = SizeofResource(resources, res);
            if (!bytes || !size) return E_FAIL;
            stream.Attach(SHCreateMemStream(bytes, size));
            if (!stream) return E_OUTOFMEMORY;
        }
        std::wstring headers = L"Content-Type: ";
        headers += allowed ? asset.contentType : L"text/plain; charset=utf-8";
        headers += L"\r\nCache-Control: no-store\r\n";
        if (asset.document) headers +=
            L"Content-Security-Policy: default-src 'none'; img-src https://glassplatform.invalid data:; "
            L"font-src https://glassplatform.invalid; style-src 'unsafe-inline'; script-src 'unsafe-inline'; "
            L"connect-src 'none'; frame-src 'none'; base-uri 'none'; form-action 'none'\r\n";
        ComPtr<ICoreWebView2WebResourceResponse> response;
        hr = environment->CreateWebResourceResponse(stream.Get(), allowed ? 200 : 403,
            allowed ? L"OK" : L"Forbidden", headers.c_str(), &response);
        return FAILED(hr) ? hr : args->put_Response(response.Get());
    }

    void Configure() {
        controller->put_IsVisible(visible ? TRUE : FALSE);
        controller->put_Bounds(bounds);
        ComPtr<ICoreWebView2Controller2> colored;
        if (SUCCEEDED(controller.As(&colored))) colored->put_DefaultBackgroundColor({255,16,18,37});
        ComPtr<ICoreWebView2Settings> settings;
        if (SUCCEEDED(web->get_Settings(&settings)) && settings) {
            settings->put_AreDefaultContextMenusEnabled(FALSE);
            settings->put_AreDevToolsEnabled(FALSE);
            settings->put_IsStatusBarEnabled(FALSE);
            settings->put_IsZoomControlEnabled(FALSE);
            settings->put_AreDefaultScriptDialogsEnabled(FALSE);
            ComPtr<ICoreWebView2Settings3> extended;
            if (suppressBrowserKeys && SUCCEEDED(settings.As(&extended)))
                extended->put_AreBrowserAcceleratorKeysEnabled(FALSE);
        }
        HRESULT hr = web->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
        if (FAILED(hr)) { Fail(hr); return; }
        const auto weak = weak_from_this();
        hr = web->add_WebResourceRequested(Callback<ICoreWebView2WebResourceRequestedEventHandler>(
            [weak](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                const auto self = weak.lock();
                return self && self->active ? self->Serve(args) : E_ABORT;
            }).Get(), &tokens[Resource]);
        if (FAILED(hr)) { Fail(hr); return; }
        subscribed[Resource] = true;
        hr = web->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
            [weak](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                const auto self = weak.lock();
                TaskString uri;
                if (!self || !self->active || FAILED(args->get_Uri(uri.put())) ||
                    !uri || self->document != uri.get()) return args->put_Cancel(TRUE);
                return S_OK;
            }).Get(), &tokens[Navigation]);
        if (FAILED(hr)) { Fail(hr); return; }
        subscribed[Navigation] = true;
        hr = web->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                return args->put_Handled(TRUE);
            }).Get(), &tokens[Popup]);
        if (FAILED(hr)) { Fail(hr); return; }
        subscribed[Popup] = true;
        hr = web->add_PermissionRequested(Callback<ICoreWebView2PermissionRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
                return args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
            }).Get(), &tokens[Permission]);
        if (FAILED(hr)) { Fail(hr); return; }
        subscribed[Permission] = true;
        hr = web->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [weak](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                const auto self = weak.lock();
                if (!self || !self->active) return S_OK;
                TaskString source, raw;
                if (FAILED(args->get_Source(source.put())) || !source || self->document != source.get() ||
                    FAILED(args->TryGetWebMessageAsString(raw.put())) || !raw) return S_OK;
                const std::wstring_view message(raw.get());
                if (message.size() <= 64) self->events.onMessage(self->events.context, std::wstring(message));
                return S_OK;
            }).Get(), &tokens[Message]);
        if (FAILED(hr)) { Fail(hr); return; }
        subscribed[Message] = true;
        hr = web->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [weak](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                const auto self = weak.lock();
                if (!self || !self->active) return S_OK;
                BOOL success = FALSE;
                if (FAILED(args->get_IsSuccess(&success)) || !success) self->Fail(E_FAIL);
                else self->events.onReady(self->events.context);
                return S_OK;
            }).Get(), &tokens[Completed]);
        if (FAILED(hr)) { Fail(hr); return; }
        subscribed[Completed] = true;
        hr = web->Navigate(document.c_str());
        if (FAILED(hr)) Fail(hr);
    }
};

WebHost::~WebHost() noexcept { Close(); }

void WebHost::Start(HWND parent, HWND host, HINSTANCE resources, bool russian,
    const wchar_t* profile, bool suppressBrowserKeys, Events events) {
    Close();
    auto self = std::make_shared<Session>();
    session_ = self;
    self->host = host;
    self->resources = resources;
    self->events = events;
    self->document = WizardAssets::DocumentUrl(russian);
    self->suppressBrowserKeys = suppressBrowserKeys;
    if (!GetClientRect(host, &self->bounds)) { self->Fail(E_HANDLE); return; }

    TaskString local;
    const HRESULT path = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, local.put());
    if (FAILED(path) || !local) { self->Fail(path); return; }
    const std::wstring profilePath = std::wstring(local.get()) + L"\\GlassPlatform\\" + profile;
    const int result = SHCreateDirectoryExW(parent, profilePath.c_str(), nullptr);
    if (result != ERROR_SUCCESS && result != ERROR_ALREADY_EXISTS && result != ERROR_FILE_EXISTS) {
        self->Fail(HRESULT_FROM_WIN32(result));
        return;
    }
    const std::weak_ptr<Session> weak = self;
    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, profilePath.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [weak](HRESULT status, ICoreWebView2Environment* environment) -> HRESULT {
                const auto session = weak.lock();
                if (!session || !session->active) return S_OK;
                if (FAILED(status) || !environment) { session->Fail(status); return S_OK; }
                session->environment = environment;
                const HRESULT created = environment->CreateCoreWebView2Controller(session->host,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [weak](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                            const auto session = weak.lock();
                            // Закрытие окна не отменяет отложенное создание контроллера.
                            if (!session || !session->active) {
                                if (controller) controller->Close();
                                return S_OK;
                            }
                            session->controller = controller;
                            if (FAILED(result) || !controller) { session->Fail(result); return S_OK; }
                            const HRESULT hr = controller->get_CoreWebView2(session->web.ReleaseAndGetAddressOf());
                            if (FAILED(hr) || !session->web) { session->Fail(hr); return S_OK; }
                            session->Configure();
                            return S_OK;
                        }).Get());
                if (FAILED(created)) session->Fail(created);
                return S_OK;
            }).Get());
    if (FAILED(hr)) self->Fail(hr);
}

void WebHost::Close() noexcept {
    if (auto session = std::move(session_)) session->Close();
}

void WebHost::SetBounds(RECT bounds) noexcept {
    if (!session_ || EqualRect(&bounds, &session_->bounds)) return;
    session_->bounds = bounds;
    if (session_->controller) session_->controller->put_Bounds(bounds);
}

void WebHost::SetVisible(bool visible) noexcept {
    if (!session_) return;
    session_->visible = visible;
    if (session_->controller) session_->controller->put_IsVisible(visible ? TRUE : FALSE);
}

void WebHost::NotifyParentMoved() noexcept {
    if (session_ && session_->controller) session_->controller->NotifyParentWindowPositionChanged();
}

HRESULT WebHost::PostJson(const wchar_t* json) const noexcept {
    return session_ && session_->active && session_->web ? session_->web->PostWebMessageAsJson(json) : E_HANDLE;
}

bool WebState::SendInit(const WebHost& host, const std::wstring& json) noexcept {
    initSent_ = SUCCEEDED(host.PostJson(json.c_str()));
    return initSent_;
}

void WebState::SendState(const WebHost& host, std::wstring json) noexcept {
    if (json != lastState_ && SUCCEEDED(host.PostJson(json.c_str()))) lastState_ = std::move(json);
}

} // namespace WizardUi
