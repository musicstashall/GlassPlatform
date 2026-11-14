#pragma once

#include <windows.h>
#include <iterator>
#include <string>
#include "../res/resource.h"

// Every URL is mapped to an embedded resource. Unknown requests are rejected;
// WebView2 never has to contact a network host for the wizard UI.
namespace WizardAssets {
inline constexpr wchar_t kDocumentUrl[] = L"https://glassplatform.invalid/index.html";
struct Asset { UINT id; const wchar_t* contentType; bool document; };

inline Asset Resolve(const std::wstring& url) noexcept {
    if (url == kDocumentUrl || url == std::wstring(kDocumentUrl) + L"?lang=ru-RU" ||
        url == std::wstring(kDocumentUrl) + L"?lang=en-US")
        return { IDR_HTML, L"text/html; charset=utf-8", true };
    static constexpr wchar_t base[] = L"https://glassplatform.invalid/assets/";
    if (url.rfind(base, 0) != 0) return {};
    const std::wstring file = url.substr(std::size(base) - 1);
    if (file == L"banner.png") return { IDR_WELCOME, L"image/png", false };
    if (file == L"handwritten.ttf") return { IDR_FONT_HANDWRITTEN, L"font/ttf", false };
    if (file == L"slide_start.jpg") return { IDR_SLIDE_START, L"image/jpeg", false };
    if (file == L"slide_taskbar.jpg") return { IDR_SLIDE_TASKBAR, L"image/jpeg", false };
    if (file == L"slide_notification.png") return { IDR_SLIDE_NOTIFICATION, L"image/png", false };
    if (file == L"slide_context_menu.jpg") return { IDR_SLIDE_CONTEXT_MENU, L"image/jpeg", false };
    if (file == L"slide_explorer.jpg") return { IDR_SLIDE_EXPLORER, L"image/jpeg", false };
    return {};
}
}
