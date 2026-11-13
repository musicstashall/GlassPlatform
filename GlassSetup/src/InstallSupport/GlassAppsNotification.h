#pragma once
#include <Windows.h>

namespace GlassAppsNotification {
// Notify consumers after a committed MSI/ARP change. Never drive, terminate or
// reopen the user's Settings window. The timeout bounds unresponsive recipients;
// use a synchronous API because WM_SETTINGCHANGE carries a string pointer.
inline void Changed() noexcept {
    DWORD_PTR result=0;
    (void)SendMessageTimeoutW(HWND_BROADCAST,WM_SETTINGCHANGE,0,
        reinterpret_cast<LPARAM>(L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall"),
        SMTO_ABORTIFHUNG,100,&result);
}
}
