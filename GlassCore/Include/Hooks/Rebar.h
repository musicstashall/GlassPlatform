#pragma once
#include <windows.h>

namespace GlassCore::RebarControl
{
    void Prepare();
    void Startup();
    void Shutdown();

    // Прямое управление хуками из WndCompat (вызывается из RebarSubclassProc).
    // Включает/выключает OHK_DRAWTHEMEBG на comctl32.
    void OnPaintBegin(HWND hWnd);
    void OnPaintEnd();
}
