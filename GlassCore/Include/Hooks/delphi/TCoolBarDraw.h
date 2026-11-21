#pragma once
#include <windows.h>

// Отдельный путь отрисовки Delphi TCoolBar (окно класса CoolBar).
//
// Rebar-ветка для этого контрола не подходит: у окна нет темы, полоса
// заливается пустым ExtTextOutW с ETO_OPAQUE, текст рисуется DrawTextExW
// штатным GDI. Поэтому у CoolBar свой набор хуков и своя сессия отрисовки.

namespace GlassCore::CoolBarControl
{
    void Prepare();
    void Startup();
    void Shutdown();

    // Прямое управление хуками из WndCompat (CoolBarSubclassProc).
    void OnPaintBegin(HWND hWnd);
    void OnPaintEnd();
}
