#pragma once
#include <windows.h>

namespace GlassCore::ComboBoxControl
{
    void Prepare();
    void Startup();
    void Shutdown();

    // Глобальный Detours на DrawThemeBackground — forms.ni вызывает DTB напрямую (без IAT comctl32).
}
