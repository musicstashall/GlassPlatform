using GlassSettings.Core.Layout;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI;
using Windows.UI;

namespace GlassSettings.Services;

/// <summary>
/// Фон overlay-pane NavigationView: in-app acrylic при LeftMinimal (см. WindowBackdropService).
/// В режиме Left (pane рядом) — прозрачный, как NavigationViewExpandedPaneBackground в теме.
/// </summary>
public static class NavigationPaneChromeService
{
    private static readonly SolidColorBrush TransparentPaneBrush = new(Colors.Transparent);
    private static bool _lastIsOverlay;
    private static bool _hasLastOverlay;

    public static void Apply(NavigationView nav, SettingsLayoutMode layoutMode)
    {
        if (!nav.IsLoaded)
        {
            RoutedEventHandler? handler = null;
            handler = (_, _) =>
            {
                nav.Loaded -= handler;
                Apply(nav, layoutMode);
            };
            nav.Loaded += handler;
            return;
        }

        if (nav.DispatcherQueue is null)
        {
            ApplyCore(nav, layoutMode);
            return;
        }

        nav.DispatcherQueue.TryEnqueue(DispatcherQueuePriority.Normal, () => ApplyCore(nav, layoutMode));
    }

    private static void ApplyCore(NavigationView nav, SettingsLayoutMode layoutMode)
    {
        if (FindTemplateChild<SplitView>(nav, "RootSplitView") is not SplitView splitView)
            return;

        var isOverlay = layoutMode == SettingsLayoutMode.Narrow && nav.IsPaneOpen;
        if (_hasLastOverlay && _lastIsOverlay == isOverlay)
            return;

        _hasLastOverlay = true;
        _lastIsOverlay = isOverlay;
        splitView.PaneBackground = isOverlay
            ? WindowBackdropService.GetNavigationOverlayPaneBrush()
            : TransparentPaneBrush;
    }

    private static T? FindTemplateChild<T>(DependencyObject root, string name)
        where T : FrameworkElement
    {
        var count = VisualTreeHelper.GetChildrenCount(root);
        for (var i = 0; i < count; i++)
        {
            var child = VisualTreeHelper.GetChild(root, i);
            if (child is T match && match.Name == name)
                return match;

            if (FindTemplateChild<T>(child, name) is { } found)
                return found;
        }

        return null;
    }
}
