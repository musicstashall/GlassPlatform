using GlassSettings.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Windows.UI;

namespace GlassSettings.Controls;

/// <summary>Плитка недавнего акцента (~48×48) с hover и галочкой выбора.</summary>
public sealed partial class AccentRecentColorSwatch : UserControl
{
    public static readonly DependencyProperty SwatchColorProperty =
        DependencyProperty.Register(
            nameof(SwatchColor),
            typeof(Color),
            typeof(AccentRecentColorSwatch),
            new PropertyMetadata(default(Color), OnSwatchColorChanged));

    public static readonly DependencyProperty IsSelectedProperty =
        DependencyProperty.Register(
            nameof(IsSelected),
            typeof(bool),
            typeof(AccentRecentColorSwatch),
            new PropertyMetadata(false, OnIsSelectedChanged));

    private bool _pointerOver;

    public AccentRecentColorSwatch()
    {
        InitializeComponent();
        ActualThemeChanged += (_, _) => ApplyChrome();
        ApplyChrome();
    }

    public event EventHandler? Clicked;

    public Color SwatchColor
    {
        get => (Color)GetValue(SwatchColorProperty);
        set => SetValue(SwatchColorProperty, value);
    }

    public bool IsSelected
    {
        get => (bool)GetValue(IsSelectedProperty);
        set => SetValue(IsSelectedProperty, value);
    }

    private static void OnSwatchColorChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is AccentRecentColorSwatch swatch)
            swatch.ApplyChrome();
    }

    private static void OnIsSelectedChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is AccentRecentColorSwatch swatch)
            swatch.ApplyChrome();
    }

    private void ApplyChrome()
    {
        RootBorder.Background = new SolidColorBrush(SwatchColor);

        // Ховер/селект: TextFillColorPrimary — тёмный в light, светлый в dark (не accent).
        var chrome = AppThemeVisualResolver.ResolveThemeBrush("TextFillColorPrimaryBrush");
        var checkFg = AppThemeVisualResolver.ResolveThemeBrush("SolidBackgroundFillColorBaseBrush");

        if (IsSelected || _pointerOver)
        {
            RootBorder.BorderThickness = new Thickness(2);
            RootBorder.BorderBrush = chrome;
        }
        else
        {
            RootBorder.BorderThickness = new Thickness(1);
            RootBorder.BorderBrush =
                AppThemeVisualResolver.ResolveThemeBrush("CardStrokeColorDefaultBrush");
        }

        CheckBadge.Visibility = IsSelected ? Visibility.Visible : Visibility.Collapsed;
        CheckBadge.Background = chrome;
        CheckIcon.Foreground = checkFg;
    }

    private void RootBorder_OnPointerEntered(object sender, PointerRoutedEventArgs e)
    {
        _pointerOver = true;
        ApplyChrome();
    }

    private void RootBorder_OnPointerExited(object sender, PointerRoutedEventArgs e)
    {
        _pointerOver = false;
        ApplyChrome();
    }

    private void RootBorder_OnPointerPressed(object sender, PointerRoutedEventArgs e)
    {
        /* визуал нажатия не нужен — клик через Tapped */
    }

    private void RootBorder_OnPointerReleased(object sender, PointerRoutedEventArgs e)
    {
        /* no-op */
    }

    private void RootBorder_OnTapped(object sender, TappedRoutedEventArgs e)
    {
        Clicked?.Invoke(this, EventArgs.Empty);
        e.Handled = true;
    }
}
