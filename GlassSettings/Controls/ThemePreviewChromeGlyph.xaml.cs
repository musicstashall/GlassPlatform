using GlassSettings.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.UI;

namespace GlassSettings.Controls;

public sealed partial class ThemePreviewChromeGlyph : UserControl
{
    public static readonly DependencyProperty LayoutProperty =
        DependencyProperty.Register(
            nameof(Layout),
            typeof(ThemePreviewChromeLayout),
            typeof(ThemePreviewChromeGlyph),
            new PropertyMetadata(ThemePreviewChromeLayout.Hero, OnLayoutChanged));

    public ThemePreviewChromeGlyph()
    {
        InitializeComponent();
        Loaded += (_, _) => ApplyLayoutMetrics();
    }

    public ThemePreviewChromeLayout Layout
    {
        get => (ThemePreviewChromeLayout)GetValue(LayoutProperty);
        set => SetValue(LayoutProperty, value);
    }

    public void ApplyTheme(VisualStyleThemeEntry? entry)
    {
        var isDark = Layout == ThemePreviewChromeLayout.Tile
            ? ThemePreviewChromeModeResolver.ResolveIsDarkModeForEntry(entry)
            : ThemePreviewChromeModeResolver.ResolveIsDarkMode(entry);
        var accent = Layout == ThemePreviewChromeLayout.Tile
            ? ThemePreviewChromeModeResolver.ResolveAccentColorForEntry(entry)
            : ThemePreviewChromeModeResolver.ResolveAccentColor(entry);
        ApplyPalette(ThemePreviewChromePalette.Create(isDark, accent));
    }

    public void ApplyPalette(ThemePreviewChromeColors colors)
    {
        ChromeRoot.Background = ThemePreviewChromeBackground.CreateBrush(colors.AcrylicBackground);
        Line0.Fill = new SolidColorBrush(colors.LineStrong);
        Line1.Fill = new SolidColorBrush(colors.LineSoft);
        Line2.Fill = new SolidColorBrush(colors.LineSoft);
        Line3.Fill = new SolidColorBrush(colors.LineSoft);
        AccentStripe.Background = new SolidColorBrush(colors.AccentStripe);
    }

    private static void OnLayoutChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is ThemePreviewChromeGlyph glyph)
            glyph.ApplyLayoutMetrics();
    }

    private void ApplyLayoutMetrics()
    {
        if (Layout == ThemePreviewChromeLayout.Tile)
        {
            ChromeRoot.Width = 42;
            ChromeRoot.Height = 42;
            ChromeRoot.CornerRadius = new CornerRadius(4);
            ChromeRoot.BorderThickness = new Thickness(0);
            ContentHost.Margin = new Thickness(0);
            LinesPanel.Visibility = Visibility.Collapsed;
            AccentStripe.Width = 22;
            AccentStripe.Height = 4;
            AccentStripe.HorizontalAlignment = HorizontalAlignment.Center;
            AccentStripe.VerticalAlignment = VerticalAlignment.Bottom;
            AccentStripe.Margin = new Thickness(0, 0, 0, 7);
            AccentStripe.CornerRadius = new CornerRadius(2);
            return;
        }

        ChromeRoot.Width = 96;
        ChromeRoot.Height = 96;
        ChromeRoot.CornerRadius = new CornerRadius(6);
        ChromeRoot.BorderThickness = new Thickness(0);
        ContentHost.Margin = new Thickness(10, 9, 10, 9);
        LinesPanel.Visibility = Visibility.Visible;
        AccentStripe.Width = 26;
        AccentStripe.Height = 7;
        AccentStripe.HorizontalAlignment = HorizontalAlignment.Right;
        AccentStripe.VerticalAlignment = VerticalAlignment.Bottom;
        AccentStripe.Margin = new Thickness(0, 0, 1, 1);
        AccentStripe.CornerRadius = new CornerRadius(2);
    }
}
