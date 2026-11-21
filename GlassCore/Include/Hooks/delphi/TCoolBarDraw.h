#pragma once
#include <windows.h>

// Собственная сессия и IAT-хуки Delphi TCoolBar (классы TCoolBar / CoolBar).
// Рендеринг текста не зависит от GetWindowTheme(hwnd): поддерживаются
// DrawTextW / DrawTextExW / DrawThemeText. Generic rebar не используется.
namespace GlassCore::CoolBarControl
{
    void Prepare();
    void Startup();
    void Shutdown();

    // suppliedDc — аргумент WM_PRINTCLIENT или WM_PAINT (может быть NULL).
    // End вызывается только после успешного Begin, включая вложенные paint.
    bool OnPaintBegin(HWND hWnd, HDC suppliedDc);
    void OnPaintEnd();
}
