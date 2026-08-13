#pragma once
#include <windows.h>

namespace GlassCore::ListBoxControl
{
    void Prepare();
    void Startup();
    void Shutdown();

    // Активна сессия WinForms .LISTBOX (PaintDispatcher host + BeginPaint / GetDC).
    // Включает gdiplus PatBlt/GetStockObject для фона строк.
    [[nodiscard]] bool IsInsideWinFormsListBoxPaint() noexcept;
}
