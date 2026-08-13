#include "SharedPch.h"

#include "framework.h"
#include "LogViewerBackdrop.h"
#include <Helpers/SwcaBackdrop.h>
#include <Helpers/SystemHelper.h>
#include <Helpers/WinApiUtils.h>

#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")

namespace LogViewer
{
    void ApplyWindowBackdrop(HWND hwnd) noexcept
    {
        const bool dark = (GetSysColor(COLOR_WINDOW) < 0xFFFFFF);
        // Та же ветка реестра, что и root-окна рабочего стола (GlassCore::Settings::kRootRegistryPart).
        Shared::SwcaBackdrop::ApplyChromeFromBwmPart(hwnd, dark, L"Root");
    }

    void ClearWindowBackdrop(HWND hwnd) noexcept
    {
        if (!hwnd || !IsWindow(hwnd))
            return;
        (void)Shared::WinApiUtils::EnableBlurBehindWindow(hwnd, false);
        (void)Shared::WinApiUtils::ExtendFrameIntoEntireClientArea(hwnd, false);
        Shared::SwcaBackdrop::SetWindowBackdrop(hwnd, FALSE, 0, 0);
        Shared::SwcaBackdrop::SetImmersiveDarkMode(hwnd, FALSE);
        if (Shared::SystemHelper::g_buildNumber >= 22000)
        {
            constexpr DWORD kDwmwaSystemBackdropType = 38;
            const DWM_SYSTEMBACKDROP_TYPE autoBackdrop = DWMSBT_AUTO;
            (void)DwmSetWindowAttribute(
                hwnd,
                kDwmwaSystemBackdropType,
                &autoBackdrop,
                sizeof(autoBackdrop));
            DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_DEFAULT;
            (void)DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
            COLORREF border = DWMWA_COLOR_DEFAULT;
            (void)DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
        }
        (void)InvalidateRect(hwnd, nullptr, TRUE);
    }
}
