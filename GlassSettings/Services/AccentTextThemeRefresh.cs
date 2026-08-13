using GlassSettings.Controls;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;

namespace GlassSettings.Services;

/// <summary>
/// Подсветка подстрок главной (SystemFillColorCautionBrush): обновляем при смене AppTheme.
/// </summary>
internal static class AccentTextThemeRefresh
{
    public static void RefreshVisible(FrameworkElement root)
    {
        if (!root.IsLoaded)
            return;

        WalkVisualTree(root);
    }

    private static void WalkVisualTree(DependencyObject node)
    {
        try
        {
            if (node is HomeSettingsRow row && IsVisibleInTree(row))
                row.RefreshAccentAppearance();
            else if (node is HomeCustomizationFooterLine footer && IsVisibleInTree(footer))
                footer.RefreshAccentAppearance();
            else if (node is HomeComponentGroup group && IsVisibleInTree(group))
                group.RefreshAccentAppearance();
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"GlassSettings AccentTextThemeRefresh node: {ex}");
        }

        var count = VisualTreeHelper.GetChildrenCount(node);
        for (var i = 0; i < count; i++)
            WalkVisualTree(VisualTreeHelper.GetChild(node, i));
    }

    private static bool IsVisibleInTree(FrameworkElement element)
    {
        if (!element.IsLoaded || element.Visibility != Visibility.Visible)
            return false;

        for (var current = element; current is not null; current = GetParentElement(current))
        {
            if (current.Visibility != Visibility.Visible)
                return false;
        }

        return true;
    }

    private static FrameworkElement? GetParentElement(FrameworkElement element) =>
        VisualTreeHelper.GetParent(element) as FrameworkElement;
}
