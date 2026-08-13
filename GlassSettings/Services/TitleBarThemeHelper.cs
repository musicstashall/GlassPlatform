using GlassSettings.Core.Registry;
using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Windows.UI;
using WinRT.Interop;

namespace GlassSettings.Services;

internal static class TitleBarThemeHelper
{
    private const int DwmwaUseImmersiveDarkModeBefore20H1 = 19;
    private const int DwmwaUseImmersiveDarkMode = 20;

    private static bool? _lastDark;
    private static TitleBarTheme _lastPreferredTheme;

    public static void Apply(Window window, bool extendsContentIntoTitleBar = true)
    {
        var dark = AppThemeVisualResolver.ResolveIsDarkMode();
        var preferred = ResolvePreferredTheme();
        if (_lastDark == dark && _lastPreferredTheme == preferred)
            return;

        _lastDark = dark;
        _lastPreferredTheme = preferred;
        ApplyCore(window, extendsContentIntoTitleBar);
    }

    private static void ApplyCore(Window window, bool extendsContentIntoTitleBar)
    {
        var dark = AppThemeVisualResolver.ResolveIsDarkMode();

        try
        {
            var hwnd = WindowNative.GetWindowHandle(window);
            var darkMode = dark ? 1 : 0;
            _ = DwmSetWindowAttribute(hwnd, DwmwaUseImmersiveDarkModeBefore20H1, ref darkMode, sizeof(int));
            _ = DwmSetWindowAttribute(hwnd, DwmwaUseImmersiveDarkMode, ref darkMode, sizeof(int));
        }
        catch { /* ignore */ }

        try
        {
            var titleBar = window.AppWindow.TitleBar;
            titleBar.PreferredTheme = ResolvePreferredTheme();

            var bg = dark ? Color.FromArgb(255, 32, 32, 32) : Color.FromArgb(255, 243, 243, 243);
            var fg = dark ? Colors.White : Color.FromArgb(255, 0, 0, 0);
            var inactiveBg = dark ? Color.FromArgb(255, 28, 28, 28) : Color.FromArgb(255, 230, 230, 230);
            var inactiveFg = dark
                ? Color.FromArgb(255, 180, 180, 180)
                : Color.FromArgb(255, 107, 114, 128);
            var hover = dark
                ? Color.FromArgb(255, 48, 48, 48)
                : Color.FromArgb(255, 220, 220, 220);
            var pressed = dark
                ? Color.FromArgb(255, 56, 56, 56)
                : Color.FromArgb(255, 200, 200, 200);

            titleBar.BackgroundColor = bg;
            titleBar.ForegroundColor = fg;
            titleBar.InactiveBackgroundColor = inactiveBg;
            titleBar.InactiveForegroundColor = inactiveFg;
            titleBar.ButtonForegroundColor = fg;
            titleBar.ButtonHoverBackgroundColor = hover;
            titleBar.ButtonPressedBackgroundColor = pressed;
            titleBar.ButtonHoverForegroundColor = fg;
            titleBar.ButtonPressedForegroundColor = fg;
            titleBar.ButtonInactiveForegroundColor = inactiveFg;

            if (extendsContentIntoTitleBar)
            {
                // Только фон кнопок прозрачный — caption остаётся тематическим, без DWM-синевы.
                titleBar.ButtonBackgroundColor = Colors.Transparent;
                titleBar.ButtonInactiveBackgroundColor = Colors.Transparent;
            }
            else
            {
                titleBar.ButtonBackgroundColor = bg;
                titleBar.ButtonInactiveBackgroundColor = inactiveBg;
            }
        }
        catch { /* ignore */ }
    }

    private static TitleBarTheme ResolvePreferredTheme() =>
        GlassSettingsRegistry.ReadAppTheme() switch
        {
            AppThemeMode.Light => TitleBarTheme.Light,
            AppThemeMode.Dark => TitleBarTheme.Dark,
            _ => TitleBarTheme.UseDefaultAppMode
        };

    [System.Runtime.InteropServices.DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int attrValue, int attrSize);
}
