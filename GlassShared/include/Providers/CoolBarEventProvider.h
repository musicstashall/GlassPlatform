#pragma once
#include <windows.h>

// Отдельный путь отрисовки Delphi TCoolBar (окно класса CoolBar):
// у контрола нет темы, поэтому rebar-ветка (DrawThemeBackground/DrawThemeText)
// для него не подходит. Провайдер ведёт свою paint-сессию в GlassCore.
namespace WndCompat::CoolBar
{
    struct CoolBarEventProvider
    {
        // WM_PAINT: включить/выключить хуки фона и текста полосы.
        void (*OnPaintBegin)(HWND hWnd) = nullptr;
        void (*OnPaintEnd)() = nullptr;
    };

    void SetCoolBarEventProvider(const CoolBarEventProvider& provider);
    const CoolBarEventProvider* GetCoolBarEventProvider();
}
