#pragma once
#include <windows.h>

// Отдельный путь Delphi TCoolBar (точный класс окна CoolBar).
// WM_PAINT и WM_PRINTCLIENT сохраняют native paint; GlassCore заменяет
// только фон и текст внутри собственной сессии, не generic rebar.
namespace WndCompat::CoolBar
{
    struct CoolBarEventProvider
    {
        // suppliedDc — HDC из wParam (NULL при обычном WM_PAINT).
        // End допустим только после Begin, вернувшего true.
        bool (*OnPaintBegin)(HWND hWnd, HDC suppliedDc) = nullptr;
        void (*OnPaintEnd)() = nullptr;
    };

    void SetCoolBarEventProvider(const CoolBarEventProvider& provider);
    const CoolBarEventProvider* GetCoolBarEventProvider();
}
