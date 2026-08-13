#pragma once
#include <windows.h>

namespace GlassCore::StaticControl
{
    // Lifecycle
    void Prepare();
    void Startup();
    void Shutdown();

    // Сессия BeginPaint/EndPaint для Static (в т.ч. WinForms .STATIC)
    [[nodiscard]] bool IsInsideStaticPaint() noexcept;
    [[nodiscard]] bool IsInsideStaticPaint(HWND hWnd) noexcept;
}
