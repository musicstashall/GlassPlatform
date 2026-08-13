using GlassSettings.Controls;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Media;

namespace GlassSettings.Services;

/// <summary>
/// WinUI Expander кэширует ExpanderHeaderBackground из StaticResource — после смены темы обновляем вручную.
/// </summary>
internal static class ExpanderThemeRefresh
{
    private const string HeaderBackgroundKey = "ExpanderHeaderBackground";
    private const string HeaderBackgroundPointerOverKey = "ExpanderHeaderBackgroundPointerOver";
    private const string HeaderBackgroundPressedKey = "ExpanderHeaderBackgroundPressed";

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
            if (node is SettingsSystemGateExpander systemExpander)
            {
                if (IsVisibleInTree(systemExpander))
                    systemExpander.RefreshThemeChrome();
            }
            else if (node is SettingsDividerExpander dividerExpander)
            {
                // Divider — прозрачный chrome; LayerFill из RefreshExpanderChrome ломает hover после темы.
                if (IsVisibleInTree(dividerExpander))
                    dividerExpander.RefreshDividerChrome();
            }
            else if (node is Expander expander
                     && !IsInsideSystemGateExpander(expander)
                     && !IsInsideDividerExpander(expander))
            {
                if (IsVisibleInTree(expander))
                    RefreshExpanderChrome(expander);
            }
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"GlassSettings ExpanderThemeRefresh node: {ex}");
        }

        var count = VisualTreeHelper.GetChildrenCount(node);
        for (var i = 0; i < count; i++)
            WalkVisualTree(VisualTreeHelper.GetChild(node, i));
    }

    public static void RefreshExpanderChrome(Expander expander)
    {
        if (!expander.IsLoaded)
            return;

        var background = ResolveBrush("LayerFillColorDefaultBrush");
        var border = ResolveBrush("CardStrokeColorDefaultBrush");
        if (background is null)
            return;

        // Во время ActualThemeChanged indexer set на StaticResource-алиас даёт 0x800F0902 —
        // свойства Background всё равно обновляем; словарь — best-effort.
        TryReplaceResourceBrush(expander.Resources, HeaderBackgroundKey, background);
        TryReplaceResourceBrush(expander.Resources, HeaderBackgroundPointerOverKey, background);
        TryReplaceResourceBrush(expander.Resources, HeaderBackgroundPressedKey, background);

        expander.Background = background;
        if (border is not null)
            expander.BorderBrush = border;

        if (FindDescendant<ToggleButton>(expander) is ToggleButton header)
            header.Background = background;
    }

    private static void TryReplaceResourceBrush(ResourceDictionary resources, string key, Brush brush)
    {
        try
        {
            if (resources.ContainsKey(key))
                resources.Remove(key);
            resources[key] = brush;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(
                $"GlassSettings ExpanderThemeRefresh resource '{key}': 0x{ex.HResult:X8}");
        }
    }

    private static Brush? ResolveBrush(string key)
    {
        try
        {
            return AppThemeVisualResolver.ResolveThemeBrush(key);
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"GlassSettings ExpanderThemeRefresh resolve '{key}': {ex}");
            return null;
        }
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

    private static bool IsInsideSystemGateExpander(DependencyObject node)
    {
        for (var parent = VisualTreeHelper.GetParent(node);
             parent is not null;
             parent = VisualTreeHelper.GetParent(parent))
        {
            if (parent is SettingsSystemGateExpander)
                return true;
        }

        return false;
    }

    private static bool IsInsideDividerExpander(DependencyObject node)
    {
        for (var parent = VisualTreeHelper.GetParent(node);
             parent is not null;
             parent = VisualTreeHelper.GetParent(parent))
        {
            if (parent is SettingsDividerExpander)
                return true;
        }

        return false;
    }

    private static FrameworkElement? GetParentElement(FrameworkElement element) =>
        VisualTreeHelper.GetParent(element) as FrameworkElement;

    private static T? FindDescendant<T>(DependencyObject root) where T : DependencyObject
    {
        var count = VisualTreeHelper.GetChildrenCount(root);
        for (var i = 0; i < count; i++)
        {
            var child = VisualTreeHelper.GetChild(root, i);
            if (child is T match)
                return match;

            var nested = FindDescendant<T>(child);
            if (nested is not null)
                return nested;
        }

        return null;
    }
}
