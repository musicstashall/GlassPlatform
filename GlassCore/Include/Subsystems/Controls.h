#pragma once
#include <windows.h>

namespace GlassCore::Controls
{
    void Prepare();
    void Startup();
    void Shutdown();
    void Update();

    void OnControlCreated(HWND hWnd);
    void OnControlDestroyed(HWND hWnd);
}
