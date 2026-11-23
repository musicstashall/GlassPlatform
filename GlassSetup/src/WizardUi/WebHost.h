#pragma once

#include <windows.h>
#include <memory>
#include <string>
#include <string_view>

namespace WizardUi {

// Все вызовы и уведомления выполняются в STA-потоке окна мастера.
class WebHost final {
public:
    struct Events {
        void* context;
        void (*onReady)(void*);
        void (*onFailure)(void*, HRESULT);
        void (*onMessage)(void*, const std::wstring&);
    };

    WebHost() noexcept = default;
    ~WebHost() noexcept;
    WebHost(const WebHost&) = delete;
    WebHost& operator=(const WebHost&) = delete;

    void Start(HWND parent, HWND host, HINSTANCE resources, bool russian,
        const wchar_t* profile, bool suppressBrowserKeys, Events events);
    void Close() noexcept;
    void SetBounds(RECT bounds) noexcept;
    void SetVisible(bool visible) noexcept;
    void NotifyParentMoved() noexcept;
    HRESULT PostJson(const wchar_t* json) const noexcept;

private:
    struct Session;
    std::shared_ptr<Session> session_;
};

// Пакеты формирует конкретный мастер; доставка и дедупликация общие.
class WebState final {
public:
    bool NeedsInit() const noexcept { return !initSent_; }
    void Reset() noexcept { initSent_ = false; lastState_.clear(); }
    bool SendInit(const WebHost& host, const std::wstring& json) noexcept;
    void SendState(const WebHost& host, std::wstring json) noexcept;

private:
    bool initSent_ = false;
    std::wstring lastState_;
};

} // namespace WizardUi
