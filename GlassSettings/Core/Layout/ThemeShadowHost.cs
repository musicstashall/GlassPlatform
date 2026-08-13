using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace GlassSettings.Core.Layout;

/// <summary>Подключение ThemeShadow к ShadowCastGrid (receiver-сibling, не предок caster).</summary>
public static class ThemeShadowHost
{
    private static readonly DependencyProperty IsAttachedProperty =
        DependencyProperty.RegisterAttached(
            "IsAttached",
            typeof(bool),
            typeof(ThemeShadowHost),
            new PropertyMetadata(false));

    /// <summary>
    /// Receiver — только <paramref name="castGrid"/> (sibling shadowBorder в одном родителе).
    /// Нельзя передавать предка caster: ThemeShadow.Receivers.Add бросает ArgumentException.
    /// </summary>
    public static void Attach(Border shadowBorder, Grid castGrid, ThemeShadow shadow)
    {
        if (shadowBorder.GetValue(IsAttachedProperty) is true)
            return;

        shadowBorder.SetValue(IsAttachedProperty, true);

        void OnLoaded(object sender, RoutedEventArgs _)
        {
            if (!shadow.Receivers.Contains(castGrid))
                shadow.Receivers.Add(castGrid);
        }

        void OnUnloaded(object sender, RoutedEventArgs _)
        {
            shadow.Receivers.Remove(castGrid);
            shadowBorder.Loaded -= OnLoaded;
            shadowBorder.Unloaded -= OnUnloaded;
            shadowBorder.ClearValue(IsAttachedProperty);
        }

        shadowBorder.Loaded += OnLoaded;
        shadowBorder.Unloaded += OnUnloaded;

        if (shadowBorder.IsLoaded)
            OnLoaded(shadowBorder, new RoutedEventArgs());
    }
}
