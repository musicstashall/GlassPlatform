#pragma once
#include <Windows.h>
#include <dwmapi.h>
#include <string_view>

namespace Shared::SwcaBackdrop
{
    /// Реестр BWM: тип эффекта (engine 0…8); для part=Root — Root\Active|Inactive\EffectType.
    /// skipNcRendering: если true, пропускает TriggerWindowNCRendering (для отложенного вызова после cloaking).
    void SetWindowBackdrop(HWND hwnd, BOOL dropShadow, DWORD tintAbgr, int effectTypeReg, bool skipNcRendering = false, bool flyoutBackdrop = false) noexcept;

    void SetImmersiveDarkMode(HWND hwnd, BOOL dark) noexcept;

    /// Читает `part` (например `Root`), подмешивает альфу из GlassOpacity, применяет SWCA + рамку Win11 + переход DWM.
    void ApplyChromeFromBwmPart(HWND hwnd, bool darkMode, std::wstring_view part) noexcept;
}
