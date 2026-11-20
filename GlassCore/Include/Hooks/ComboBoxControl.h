#pragma once
#include <windows.h>

namespace GlassCore::ComboBoxControl
{
    void Prepare();
    void Startup();
    void Shutdown();

    // True while a native class-"ComboBox" window is painted by the host
    // application (EXE IAT session). Used by Root GDI hooks to scope their
    // fill/rectangle handling to that exact window only.
    [[nodiscard]] bool IsNativeAppPaintSession(HWND hWnd) noexcept;

    // Глобальный Detours на DrawThemeBackground — forms.ni вызывает DTB напрямую (без IAT comctl32).
}
