#include "WndPch.h"
#include "WindowClassUtils.h"

namespace WndCompat
{
    std::wstring NormalizeWindowClass(HWND hWnd)
    {
        WCHAR raw[MAX_PATH]{};
        if (!GetClassNameW(hWnd, raw, MAX_PATH))
            return L"";

        std::wstring cls = raw;

        // --- WinForms ---
        // WindowsForms10.<RealClass>.<version>.<app>.<random>
        if (cls.rfind(L"WindowsForms10.", 0) == 0)
        {
            // Найти вторую точку после "WindowsForms10."
            size_t prefixLen = 15; // strlen("WindowsForms10.")
            size_t pos = cls.find(L'.', prefixLen);
            if (pos != std::wstring::npos)
            {
                // Вернуть WinForms-класс с точкой: ".Button", ".Edit", ".SysListView32"
                return L"." + cls.substr(prefixLen, pos - prefixLen);
            }
        }

        // --- WPF ---
        if (cls.rfind(L"HwndWrapper", 0) == 0)
            return L"WPF_HwndWrapper";

        // --- WinUI ---
        if (cls == L"ApplicationFrameWindow")
            return L"WinUI_Frame";

        // --- Обычные Win32 классы ---
        return cls;
    }

    bool IsWindowClass(HWND hWnd, std::wstring_view expected)
    {
        std::wstring cls = NormalizeWindowClass(hWnd);

        // Убираем точку у WinForms-классов
        if (!cls.empty() && cls[0] == L'.')
            cls.erase(0, 1);

        // Убираем точку у expected, если она есть
        if (!expected.empty() && expected[0] == L'.')
            expected.remove_prefix(1);

        return _wcsicmp(cls.c_str(), expected.data()) == 0;
    }

    bool IsGlassSettingsBitmapSwitchHwnd(HWND hWnd) noexcept
    {
        return IsWindowClass(hWnd, L"GlassSettingsBitmapSwitchClass");
    }
}
