using GlassSettings.Controls;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;

namespace GlassSettings.Services;

/// <summary>Обновление alert-точек и полос уведомлений после смены темы.</summary>
internal static class SettingsAlertThemeRefresh
{
    public static void RefreshVisible(FrameworkElement root)
    {
        if (root.IsLoaded)
        {
            WalkVisualTree(root);
            return;
        }

        void OnLoaded(object sender, RoutedEventArgs e)
        {
            root.Loaded -= OnLoaded;
            WalkVisualTree(root);
        }

        root.Loaded += OnLoaded;
    }

    private static void WalkVisualTree(DependencyObject node)
    {
        try
        {
            if (node is UnsavedIndicatorDot dot && dot.IsLoaded)
                dot.RefreshChrome();
            else if (node is SettingsUnsavedNoticeStrip strip && strip.IsLoaded)
                strip.RefreshSeverityChrome();
            else if (node is HomeSettingsRow row && row.IsLoaded)
                row.RefreshAccentAppearance();
            else if (node is HomeComponentGroup group && group.IsLoaded)
                group.RefreshAccentAppearance();
            else if (node is SpoilerHeaderGrid header && header.IsLoaded)
                header.RefreshAlertChrome();
            else if (node is PlatformSchedulerStackableActions stackable && stackable.IsLoaded)
                stackable.RefreshStatusChrome();
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"GlassSettings SettingsAlertThemeRefresh node: {ex}");
        }

        var count = VisualTreeHelper.GetChildrenCount(node);
        for (var i = 0; i < count; i++)
            WalkVisualTree(VisualTreeHelper.GetChild(node, i));
    }
}
