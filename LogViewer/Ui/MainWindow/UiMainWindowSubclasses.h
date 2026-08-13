#pragma once

#include <windows.h>

LRESULT CALLBACK LogViewerMainWndEditSubclassProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR uIdSubclass, DWORD_PTR dwRefData);

LRESULT CALLBACK LogViewerMainWndTabCtrlSubclassProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR uIdSubclass, DWORD_PTR dwRefData);

[[nodiscard]] bool LogViewerSearchNavigateBackwardFromModifiers() noexcept;
