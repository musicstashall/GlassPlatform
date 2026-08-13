using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace GlassSettings.Controls;

/// <summary>Layout-хуки для <see cref="SettingsSystemGateExpander"/>.</summary>
public static class SettingsSystemGateExpanderLayout
{
    /// <summary>Callback после разворота (напр. <see cref="SettingsColorPickerPanel.RefreshLayout"/>).</summary>
    public static void WireExpandedLayoutRefresh(
        this SettingsSystemGateExpander gateExpander,
        Action onExpanded)
    {
        void Hook()
        {
            if (FindDescendant<Expander>(gateExpander) is not Expander rootExpander)
                return;

            rootExpander.RegisterPropertyChangedCallback(
                Expander.IsExpandedProperty,
                (_, _) =>
                {
                    if (!rootExpander.IsExpanded)
                        return;

                    var queue = gateExpander.DispatcherQueue;
                    if (queue is not null)
                        queue.TryEnqueue(DispatcherQueuePriority.Normal, () => onExpanded());
                    else
                        onExpanded();
                });
        }

        if (gateExpander.IsLoaded)
            Hook();
        else
            gateExpander.Loaded += (_, _) => Hook();
    }

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
