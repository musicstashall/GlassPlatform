#pragma once
#include <windows.h>
#include <cwchar>

namespace GlassShared
{
    // Custom-painted VCL groups, not native Button and not panel backgrounds.
    [[nodiscard]] inline bool IsDelphiButtonGroupsClassName(const wchar_t* name) noexcept
    {
        return name && (_wcsicmp(name, L"TButtonGroup") == 0
            || _wcsicmp(name, L"TCategoryButtons") == 0);
    }

    [[nodiscard]] inline bool IsDelphiButtonGroupsWindow(HWND hwnd) noexcept
    {
        wchar_t name[64]{};
        return hwnd && GetClassNameW(hwnd, name, static_cast<int>(_countof(name))) > 0
            && IsDelphiButtonGroupsClassName(name);
    }
}
