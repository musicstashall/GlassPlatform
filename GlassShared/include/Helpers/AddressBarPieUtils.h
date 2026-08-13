#pragma once

#include <windows.h>

namespace Shared::AddressBarPie
{
    // Иерархия «пирога» адресной строки (комдиалог / старый проводник):
    //   msctls_progress32 → ComboBoxEx32 → ComboBox (и дочерние HWND) / дропдаун ComboLBox.
    // Для этого поддерева Glass не вешает сессию отрисовки комбо — только триггер ComboLBox (AnimateWindow и т.д.).
    [[nodiscard]] inline bool IsInProgressPieComboSubtree(HWND hwnd) noexcept
    {
        if (!hwnd || !IsWindow(hwnd))
            return false;

        HWND cur = hwnd;
        wchar_t cls[32]{};
        if (GetClassNameW(cur, cls, static_cast<int>(sizeof(cls) / sizeof(wchar_t))) > 0
            && _wcsicmp(cls, L"ComboLBox") == 0)
        {
            const HWND owner = GetWindow(cur, GW_OWNER);
            if (!owner || !IsWindow(owner))
                return false;
            cur = owner;
        }

        for (int depth = 0; depth < 24 && cur && IsWindow(cur); ++depth)
        {
            if (GetClassNameW(cur, cls, static_cast<int>(sizeof(cls) / sizeof(wchar_t))) > 0
                && _wcsicmp(cls, L"ComboBoxEx32") == 0)
            {
                const HWND par = GetParent(cur);
                if (!par || !IsWindow(par))
                    return false;
                wchar_t pcls[32]{};
                if (GetClassNameW(par, pcls, static_cast<int>(sizeof(pcls) / sizeof(wchar_t))) <= 0)
                    return false;
                if (_wcsicmp(pcls, L"msctls_progress32") == 0)
                    return true;
            }
            cur = GetParent(cur);
        }
        return false;
    }
} // namespace Shared::AddressBarPie
