#pragma once
#include <windows.h>

namespace GlassCore::Menu
{
    void Prepare();
    void Startup();
    void Shutdown();
    void Update();

    void OnPopupCreated(HWND hWnd);
    void OnPopupDestroyed(HWND hWnd);
}
