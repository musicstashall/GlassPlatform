#pragma once
#include "framework.h"

class UiMainWindow
{
public:
    void Create(HINSTANCE hInst);
    HWND GetHwnd() const { return m_hwnd; }

    void SetPlatformRunning(bool running);

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    HWND m_hwnd = nullptr;
};
