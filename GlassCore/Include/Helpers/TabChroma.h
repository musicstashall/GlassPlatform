#pragma once
#include <windows.h>

namespace GlassCore::TabChroma
{
    // Кэш цветов хромакея вкладок (только в GlassCore.dll; GlassShared — без состояния).
    // Ключи: TIS_NORMAL(1), TIS_DISABLED(4), TIS_FOCUSED(5). serial==0 — ещё не было успешного Refresh.

    [[nodiscard]] bool TryReadChromaForTisState(
        int tis1Based, COLORREF& outRgb, UINT32* optSerial) noexcept;

    [[nodiscard]] bool RefreshFromTheme(HWND themeAnchorOrNull) noexcept;
}
