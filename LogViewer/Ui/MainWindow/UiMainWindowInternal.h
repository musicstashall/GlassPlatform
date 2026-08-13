#pragma once

#include <windows.h>

class UiMainWindowImpl;

[[nodiscard]] UiMainWindowImpl* GetImpl(HWND hwnd);
[[nodiscard]] HFONT CreateMonoFont(int ptSize = 9);
[[nodiscard]] HFONT CreateUiFont();

/// Регистрация дочернего окна-контейнера тулбара фильтров (LogViewer.FilterTbHost).
[[nodiscard]] bool RegisterLogViewerFilterTbHostClass(HINSTANCE hInst) noexcept;
