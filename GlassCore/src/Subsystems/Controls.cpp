#include "CorePch.h"
#include "Subsystems\Controls.h"

namespace GlassCore::Controls
{
    void Prepare()
    {
        DebugLog(L"[GlassCore][Controls] Prepare");
    }

    void Startup()
    {
        DebugLog(L"[GlassCore][Controls] Startup");
    }

    void Shutdown()
    {
        DebugLog(L"[GlassCore][Controls] Shutdown");
    }

    void Update()
    {
    }

    void OnControlCreated(HWND hWnd)
    {
        DebugLog(L"[GlassCore][Controls] OnControlCreated: 0x%p", hWnd);
    }

    void OnControlDestroyed(HWND hWnd)
    {
        DebugLog(L"[GlassCore][Controls] OnControlDestroyed: 0x%p", hWnd);
    }
}
