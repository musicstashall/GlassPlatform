using Microsoft.UI.Xaml;
using Microsoft.Win32;

namespace GlassSettings.Services;

/// <summary>
/// Синхронизация ElementTheme окна GlassSettings с AppsUseLightTheme (Personalize).
/// Application.RequestedTheme — только в AppThemeService.ApplyApplicationThemeAtStartup (до первого окна).
/// </summary>
public static class WindowThemeBootstrap
{
    public static ElementTheme ResolveElementTheme()
    {
        // Паритет обычных приложений: AppsUseLightTheme, без AppMode из .theme.
        var fromRegistry = ResolveAppsUseLightTheme();
        if (fromRegistry != ElementTheme.Default)
            return fromRegistry;

        return ElementTheme.Default;
    }

    public static void ApplyToRoot(FrameworkElement root)
    {
        var theme = ResolveElementTheme();
        if (theme == ElementTheme.Default)
            return;

        if (root.RequestedTheme != theme)
            root.RequestedTheme = theme;

        ScheduleExpanderRefresh(root);
    }

    /// <summary>Для AppThemeService после принудительной light/dark темы.</summary>
    internal static void ScheduleExpanderRefreshPublic(FrameworkElement root) =>
        ScheduleExpanderRefresh(root);

    private static void ScheduleExpanderRefresh(FrameworkElement root)
    {
        if (root.DispatcherQueue is null)
            return;

        root.DispatcherQueue.TryEnqueue(() =>
        {
            try
            {
                if (root.IsLoaded)
                {
                    ExpanderThemeRefresh.RefreshVisible(root);
                    AccentTextThemeRefresh.RefreshVisible(root);
                    SettingsAlertThemeRefresh.RefreshVisible(root);
                    App.ShellWindow?.RefreshMirrorStripChrome();
                }
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"GlassSettings ExpanderThemeRefresh: {ex}");
            }
        });
    }

    private static ElementTheme ResolveAppsUseLightTheme()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(
                @"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize");
            var value = key?.GetValue("AppsUseLightTheme");
            if (value is int i)
                return i == 0 ? ElementTheme.Dark : ElementTheme.Light;
        }
        catch
        {
            /* ignore */
        }

        return ElementTheme.Default;
    }
}
