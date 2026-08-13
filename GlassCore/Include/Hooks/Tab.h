#pragma once
#include <windows.h>

namespace GlassCore::TabControl
{
    // Lifecycle
    void Prepare();
    void Startup();
    void Shutdown();

    // Root (WndCompat) → сэмпл TabChroma в модуле GlassCore (см. RootEventProvider::OnRootWmThemeChangedForTabChroma).
    void OnRootWmThemeChangedForTabChroma(HWND themeAnchorOrNull) noexcept;
}
