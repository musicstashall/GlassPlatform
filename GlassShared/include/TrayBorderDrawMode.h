#pragma once

#include <Windows.h>
#include <Helpers/RealUserReg.h>

// Tray\BorderDrawMode — полный путь отрисовки панели задач (не только бордюр):
// 0 = DWM/SWCA (фон SWCA на Shell_*TrayWnd), 1 = XAML (фон через TaskbarCompat).
// См. docs/guides/TaskbarBorders.md

namespace Shared::TrayBorderDrawMode
{
    inline constexpr const wchar_t* kSubKey = L"Software\\StanislavKa\\BlackWindowManager\\Tray";
    inline constexpr const wchar_t* kValueName = L"BorderDrawMode";

    enum class Mode : DWORD
    {
        Dwm  = 0,
        Xaml = 1,
    };

    // outSource: 0=hkcu, 1=HKEY_USERS, 2=fail/default — см. RealUserReg.
    [[nodiscard]] inline Mode ReadFromRegistry(int* outSource = nullptr) noexcept
    {
        DWORD mode = 0;
        const LSTATUS ls = Shared::RealUserReg::GetDword(kSubKey, kValueName, mode, outSource);
        if (ls != ERROR_SUCCESS || mode > static_cast<DWORD>(Mode::Xaml))
            return Mode::Dwm;
        return static_cast<Mode>(mode);
    }
}
