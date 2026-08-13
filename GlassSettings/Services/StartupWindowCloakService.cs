using System.Runtime.InteropServices;

namespace GlassSettings.Services;

/// <summary>
/// DWM cloak для first-paint splash: Activate под cloak, uncloak когда overlay+icon+backdrop готовы.
/// Не SW_HIDE — см. GlassSettings-startup-white-flash.md (#12/#13).
/// Win32 ShowWindow / AppWindow.Hide→Show не дают системную DWM-анимацию открытия WinUI AppWindow
/// (runtime: ShowWindow(SW_SHOWNORMAL) → false; видимость только через AppWindow/Activate).
/// </summary>
internal static class StartupWindowCloakService
{
    private const int DwmwaCloak = 13;

    public static bool TrySetCloaked(IntPtr hwnd, bool cloaked)
    {
        if (hwnd == IntPtr.Zero)
            return false;

        var value = cloaked ? 1 : 0;
        return DwmSetWindowAttribute(hwnd, DwmwaCloak, ref value, sizeof(int)) == 0;
    }

    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(
        IntPtr hwnd,
        int dwAttribute,
        ref int pvAttribute,
        int cbAttribute);
}
