using System.Runtime.InteropServices;
using Microsoft.Win32;

namespace GlassSettings.Core.Registry;

public static class ThemeRegistry
{
    /// <summary>BWM Root/Menu/Flyout: COLOR_WINDOW (паритет runtime).</summary>
    public static bool IsDarkMode() => GetSysColor(5) < 0xFFFFFF;

    /// <summary>
    /// Tray\Xaml* / смежные: uxtheme #132 (ShouldAppsUseDarkMode),
    /// fallback AppsUseLightTheme==0. Без #139 (как ThemeHelper::IsTrayXamlDarkMode).
    /// </summary>
    public static bool IsTrayXamlDarkMode()
    {
        try
        {
            if (ShouldAppsUseDarkMode())
                return true;
        }
        catch (EntryPointNotFoundException)
        {
        }
        catch (DllNotFoundException)
        {
        }

        return ReadAppsUseLightThemeIsDark();
    }

    public static string GradientColorName()
        => (IsDarkMode() ? "DarkMode_" : "LightMode_") + "GradientColor";

    public static string BorderColorName()
        => (IsDarkMode() ? "DarkMode_" : "LightMode_") + "BorderColor";

    public static string TrayXamlGradientColorName()
        => (IsTrayXamlDarkMode() ? "DarkMode_" : "LightMode_") + "GradientColor";

    public static string TrayXamlBorderColorName()
        => (IsTrayXamlDarkMode() ? "DarkMode_" : "LightMode_") + "BorderColor";

    public static string GlobalTextGlowName()
        => (IsDarkMode() ? "DarkMode_" : "LightMode_") + "GlobalTextGlow";

    private static bool ReadAppsUseLightThemeIsDark()
    {
        try
        {
            using var key = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(
                @"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize");
            if (key?.GetValue("AppsUseLightTheme") is int v)
                return v == 0;
            if (key?.GetValue("AppsUseLightTheme") is uint u)
                return u == 0;
        }
        catch
        {
        }

        return false;
    }

    [DllImport("user32.dll")]
    private static extern uint GetSysColor(int nIndex);

    [DllImport("uxtheme.dll", EntryPoint = "#132")]
    [return: MarshalAs(UnmanagedType.I1)]
    private static extern bool ShouldAppsUseDarkMode();
}
