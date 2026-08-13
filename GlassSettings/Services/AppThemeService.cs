using GlassSettings.Core.Registry;
using Microsoft.UI.Xaml;

namespace GlassSettings.Services;

/// <summary>Тема окна GlassSettings: пользовательский выбор или следование системе.</summary>
public static class AppThemeService
{
    public static bool ShouldFollowDesktopTheme() =>
        GlassSettingsRegistry.ReadAppTheme() == AppThemeMode.System;

    /// <summary>
    /// Задать Application.RequestedTheme до создания первого окна.
    /// После InitializeComponent окна WinUI бросает COMException при смене темы приложения.
    /// </summary>
    public static void ApplyApplicationThemeAtStartup()
    {
        try
        {
            var mode = GlassSettingsRegistry.ReadAppTheme();
            ApplicationTheme target;

            if (mode == AppThemeMode.System)
            {
                var elementTheme = WindowThemeBootstrap.ResolveElementTheme();
                if (elementTheme == ElementTheme.Default)
                {
                    var isDark = AppThemeVisualResolver.ResolveIsDarkMode();
                    target = isDark ? ApplicationTheme.Dark : ApplicationTheme.Light;
                }
                else
                {
                    target = elementTheme == ElementTheme.Dark ? ApplicationTheme.Dark : ApplicationTheme.Light;
                }
            }
            else
            {
                target = mode == AppThemeMode.Light ? ApplicationTheme.Light : ApplicationTheme.Dark;
            }

            if (Application.Current.RequestedTheme != target)
                Application.Current.RequestedTheme = target;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"GlassSettings ApplyApplicationThemeAtStartup: {ex}");
        }
    }

    public static void ApplyFromRegistry(FrameworkElement root) =>
        ApplyMode(GlassSettingsRegistry.ReadAppTheme(), root);

    /// <summary>Тема для произвольного элемента (диалог, панель) без смены Application.RequestedTheme.</summary>
    public static void ApplyThemeToElement(FrameworkElement element)
    {
        ApplyElementThemeOnly(element);
        WindowThemeBootstrap.ScheduleExpanderRefreshPublic(element);
    }

    /// <summary>Только RequestedTheme — без каскадного SettingsAlert/Expander refresh (зеркало notice).</summary>
    internal static void ApplyElementThemeOnly(FrameworkElement element)
    {
        var mode = GlassSettingsRegistry.ReadAppTheme();
        if (mode == AppThemeMode.System)
            element.RequestedTheme = ElementTheme.Default;
        else
            element.RequestedTheme = mode == AppThemeMode.Light ? ElementTheme.Light : ElementTheme.Dark;
    }

    public static void ApplyMode(AppThemeMode mode, FrameworkElement root)
    {
        if (mode == AppThemeMode.System)
        {
            WindowThemeBootstrap.ApplyToRoot(root);
            return;
        }

        var theme = mode == AppThemeMode.Light ? ElementTheme.Light : ElementTheme.Dark;

        if (root.RequestedTheme != theme)
            root.RequestedTheme = theme;

        WindowThemeBootstrap.ScheduleExpanderRefreshPublic(root);
    }
}
