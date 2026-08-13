using GlassSettings.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;
using Windows.Foundation;

namespace GlassSettings.Controls;

/// <summary>
/// Общий chrome для кастомных спойлеров на базе WinUI <see cref="Expander"/>:
/// скрытие системного шеврона, вращающийся шеврон в <see cref="SpoilerHeaderGrid"/>,
/// padding и hover заголовка. Единый вид с <see cref="SettingsSystemGateExpander"/>.
/// </summary>
internal static class SpoilerExpanderChrome
{
    private static readonly TimeSpan ChevronRotationDuration = TimeSpan.FromMilliseconds(250);

    /// <summary>Скрыть встроенный шеврон Expander.</summary>
    public static void HideSystemChevron(Expander expander)
    {
        if (FindDescendantByName(expander, "ExpandCollapseChevronBorder") is UIElement host)
            host.Visibility = Visibility.Collapsed;
    }

    /// <summary>Системный шеврон Expander скрыт (chrome divider применён).</summary>
    public static bool IsSystemChevronHidden(Expander expander)
    {
        if (FindDescendantByName(expander, "ExpandCollapseChevronBorder") is UIElement host)
            return host.Visibility == Visibility.Collapsed;

        return false;
    }

    /// <summary>Expander только для programmatic reveal: без видимой шапки и отступов тела.</summary>
    public static void ApplyHeaderlessExpanderChrome(Expander expander)
    {
        HideSystemChevron(expander);

        var transparent = Application.Current.Resources["SystemControlTransparentBrush"] as Brush
                          ?? new SolidColorBrush(Microsoft.UI.Colors.Transparent);

        expander.MinHeight = 0;
        expander.Background = transparent;
        expander.BorderBrush = transparent;
        expander.BorderThickness = new Thickness(0);
        expander.Padding = new Thickness(0);
        expander.Margin = new Thickness(0);

        if (FindDescendant<ToggleButton>(expander) is ToggleButton header)
        {
            header.MinHeight = 0;
            header.MaxHeight = 0;
            header.Height = 0;
            header.Padding = new Thickness(0);
            header.Margin = new Thickness(0);
            header.BorderThickness = new Thickness(0);
            header.Visibility = Visibility.Collapsed;
            header.IsHitTestVisible = false;
            header.UseSystemFocusVisuals = false;
            header.Background = transparent;
        }

        ApplyHeaderlessBodyChrome(expander);
    }

    /// <summary>Headerless-chrome применён: шеврон скрыт, шапка collapsed.</summary>
    public static bool IsHeaderlessChromeApplied(Expander expander)
    {
        if (!IsSystemChevronHidden(expander))
            return false;

        if (FindDescendant<ToggleButton>(expander) is not ToggleButton header)
            return false;

        return header.Visibility == Visibility.Collapsed
               && header.MinHeight <= 0.01
               && header.MaxHeight <= 0.01;
    }

    /// <summary>Компактный заголовок-разделитель: прозрачный фон; hover — в SettingsDividerExpander.</summary>
    public static void ApplyDividerExpanderChrome(Expander expander)
    {
        HideSystemChevron(expander);

        var transparent = Application.Current.Resources["SystemControlTransparentBrush"] as Brush
                          ?? new SolidColorBrush(Microsoft.UI.Colors.Transparent);

        expander.Background = transparent;
        expander.BorderBrush = transparent;
        expander.BorderThickness = new Thickness(0);
        expander.Padding = new Thickness(0);

        if (TryGetHeaderToggleButton(expander) is not ToggleButton header)
            return;

        header.Padding = new Thickness(2, 0, 0, 0);
        header.MinHeight = 0;
        header.BorderThickness = new Thickness(0);
        header.VerticalContentAlignment = VerticalAlignment.Center;
        header.HorizontalContentAlignment = HorizontalAlignment.Stretch;
        header.Background = transparent;
        header.UseSystemFocusVisuals = false;
    }

    public static ToggleButton? TryGetHeaderToggleButton(Expander expander) =>
        FindDescendant<ToggleButton>(expander);

    /// <summary>Скрыть встроенный шеврон Expander и настроить собственный (в шапке).</summary>
    public static void Initialize(Expander expander, SpoilerHeaderGrid header)
    {
        HideSystemChevron(expander);
        ApplyHeaderPadding(expander);
        EnsureChevronRotation(header);
        AttachHeaderHover(expander);
    }

    public static void EnsureChevronRotation(SpoilerHeaderGrid header)
    {
        EnsureChevronRotation(header.Chevron);
    }

    public static void EnsureChevronRotation(FontIcon chevron)
    {
        chevron.Glyph = "\uE76C";
        chevron.RenderTransformOrigin = new Point(0.5, 0.5);
        if (chevron.RenderTransform is not RotateTransform)
            chevron.RenderTransform = new RotateTransform();
    }

    public static void AnimateChevron(SpoilerHeaderGrid header, bool expanded, bool animate) =>
        AnimateChevron(header.Chevron, expanded, animate);

    public static void AnimateChevron(FontIcon chevron, bool expanded, bool animate)
    {
        EnsureChevronRotation(chevron);
        if (chevron.RenderTransform is not RotateTransform rotate)
            return;

        var target = expanded ? 90.0 : 0.0;
        if (!animate || !ExpanderBodyTransition.ShouldAnimate())
        {
            rotate.Angle = target;
            return;
        }

        var from = rotate.Angle;
        if (Math.Abs(from - target) < 0.01)
            return;

        var animation = new DoubleAnimation
        {
            From = from,
            To = target,
            Duration = ChevronRotationDuration,
            FillBehavior = FillBehavior.HoldEnd,
            EasingFunction = new CubicEase { EasingMode = EasingMode.EaseInOut }
        };
        var storyboard = new Storyboard();
        Storyboard.SetTarget(animation, rotate);
        Storyboard.SetTargetProperty(animation, nameof(RotateTransform.Angle));
        storyboard.Children.Add(animation);
        storyboard.Begin();
    }

    private static void ApplyHeaderPadding(Expander expander)
    {
        if (FindDescendant<ToggleButton>(expander) is not ToggleButton header)
            return;

        if (Application.Current.Resources["GlassSettingsSystemExpanderHeaderPadding"] is Thickness padding)
            header.Padding = padding;
    }

    private static void AttachHeaderHover(Expander expander)
    {
        if (FindDescendant<ToggleButton>(expander) is not ToggleButton header)
            return;

        header.PointerEntered += (_, _) =>
        {
            header.Background = AppThemeVisualResolver.ResolveThemeBrush(
                "SubtleFillColorSecondaryBrush");
        };
        header.PointerExited += (_, _) =>
        {
            header.Background = AppThemeVisualResolver.ResolveThemeBrush(
                "LayerFillColorDefaultBrush");
        };
    }

    private static void ApplyHeaderlessBodyChrome(Expander expander)
    {
        foreach (var name in new[] { "ExpandContent", "ExpanderContent", "Content" })
        {
            if (FindDescendantByName(expander, name) is not FrameworkElement host)
                continue;

            host.Margin = new Thickness(0);
            if (host is Control control)
                control.Padding = new Thickness(0);

            return;
        }

        ZeroBodyContentPresenterChrome(expander);
    }

    private static void ZeroBodyContentPresenterChrome(DependencyObject root)
    {
        var count = VisualTreeHelper.GetChildrenCount(root);
        for (var i = 0; i < count; i++)
        {
            var child = VisualTreeHelper.GetChild(root, i);
            if (child is ContentPresenter presenter && !IsDescendantOfToggleButton(presenter))
            {
                presenter.Margin = new Thickness(0);
                presenter.Padding = new Thickness(0);
            }

            ZeroBodyContentPresenterChrome(child);
        }
    }

    private static bool IsDescendantOfToggleButton(DependencyObject element)
    {
        for (var current = VisualTreeHelper.GetParent(element);
             current is not null;
             current = VisualTreeHelper.GetParent(current))
        {
            if (current is ToggleButton)
                return true;
        }

        return false;
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

    private static DependencyObject? FindDescendantByName(DependencyObject root, string name)
    {
        var count = VisualTreeHelper.GetChildrenCount(root);
        for (var i = 0; i < count; i++)
        {
            var child = VisualTreeHelper.GetChild(root, i);
            if (child is FrameworkElement fe && fe.Name == name)
                return child;

            var nested = FindDescendantByName(child, name);
            if (nested is not null)
                return nested;
        }

        return null;
    }
}
