#pragma once
#include <windows.h>
#include <cwchar>

namespace GlassShared
{
    // Bome's custom-painted memo and its line-number gutter, not native Edit.
    [[nodiscard]] inline bool IsBomeMemoClassName(const wchar_t* name) noexcept
    {
        return name && (_wcsicmp(name, L"TBomeMemo") == 0
            || _wcsicmp(name, L"TBMGutter") == 0);
    }

    [[nodiscard]] inline bool IsBomeMemoWindow(HWND hwnd) noexcept
    {
        wchar_t name[64]{};
        return hwnd && GetClassNameW(hwnd, name, static_cast<int>(_countof(name))) > 0
            && IsBomeMemoClassName(name);
    }
}
