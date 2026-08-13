#include "SharedPch.h"

#include "MessageRouter.h"
#include "UiMainWindow.h"

UiMainWindow* g_mainWindow = nullptr; // определение глобального указателя

void MessageRouter::Send(LogViewerCommand cmd, void* payload)
{
    if (!g_mainWindow)
        return;

    HWND hwnd = g_mainWindow->GetHwnd();
    if (!hwnd)
        return;

    PostMessageW(hwnd, WM_APP_COMMAND, (WPARAM)cmd, (LPARAM)payload);
}
