#pragma once
#include <Windows.h>

namespace LogViewer
{
    /// Единый SWCA/DWM-backdrop для окон LogViewer: параметры из `Root` (как у прочих root-окон), альфа из GlassOpacity.
    void ApplyWindowBackdrop(HWND hwnd) noexcept;

    /// Снять SWCA/DWM-оформление (после ApplyWindowBackdrop), если GlassCore::OnComboLBoxDetach недоступен.
    void ClearWindowBackdrop(HWND hwnd) noexcept;
}
