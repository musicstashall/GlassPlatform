#pragma once
#include <windows.h>

namespace GlassCore::TooltipHooks
{
    void Prepare();
    void Startup();
    void Shutdown();

    // IAT comctl32 v6 (g_comctl32New): DrawTextW, DrawThemeBackground, DrawThemeTextEx — только на время WM_PAINT.
    void EnableComctlPaintHooks(bool enable);

    // GetThemeMargins: без ref-count (как BWM EnableMarginHooks).
    void EnableMarginHooks(bool enable);

    /// Переарм OHK_DRAWTEXTEX на comctl32 после смены GlobalTextGlow.
    void SyncTextGlowHooksFromRegistry() noexcept;

    [[nodiscard]] bool IsComctl32Ready() noexcept;
}
