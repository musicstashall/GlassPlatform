using Microsoft.UI.Input;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Windows.Foundation;

namespace GlassSettings.Controls;

public sealed partial class ThemeQuickActionCard : UserControl
{
    public static readonly DependencyProperty TrailingContentProperty =
        DependencyProperty.Register(
            nameof(TrailingContent),
            typeof(object),
            typeof(ThemeQuickActionCard),
            new PropertyMetadata(null, OnTrailingContentChanged));

    private uint? _activePointerId;

    public event RoutedEventHandler? Click;

    public ThemeQuickActionCard()
    {
        InitializeComponent();
        Loaded += (_, _) => UpdateCursor();
    }

    public object? TrailingContent
    {
        get => GetValue(TrailingContentProperty);
        set => SetValue(TrailingContentProperty, value);
    }

    public string Title
    {
        get => TitleText.Text;
        set => TitleText.Text = value;
    }

    public string Description
    {
        get => DescriptionText.Text;
        set => DescriptionText.Text = value;
    }

    public string IconGlyphChar
    {
        get => IconGlyph.Glyph;
        set =>         IconGlyph.Glyph = value;
    }

    public double MeasurePreferredWidth()
    {
        Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
        return Math.Ceiling(DesiredSize.Width);
    }

    public double MeasureMinWidth()
    {
        // Measure(0) в WinUI даёт DesiredSize.Width=0 — минимум считаем структурно:
        // иконка + TextColumn.MinWidth (100) + trailing + padding/border/gaps.
        Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
        if (RootBorder.Child is not Grid grid)
            return 0;

        var trailing = TrailingHost.Visibility == Visibility.Visible
            ? TrailingHost.DesiredSize.Width
            : 0;
        var gapCount = 1 + (trailing > 0 ? 1 : 0);
        var pad = RootBorder.Padding;
        var border = RootBorder.BorderThickness;
        return Math.Ceiling(
            pad.Left + pad.Right + border.Left + border.Right
            + IconGlyph.DesiredSize.Width
            + (gapCount * grid.ColumnSpacing)
            + TextColumn.MinWidth
            + trailing);
    }

    private void RootBorder_OnPointerEntered(object sender, PointerRoutedEventArgs e) =>
        VisualStateManager.GoToState(this, "PointerOver", true);

    private void RootBorder_OnPointerExited(object sender, PointerRoutedEventArgs e)
    {
        if (_activePointerId.HasValue)
            return;

        VisualStateManager.GoToState(this, "Normal", true);
    }

    private void RootBorder_OnPointerPressed(object sender, PointerRoutedEventArgs e)
    {
        _activePointerId = e.Pointer.PointerId;
        VisualStateManager.GoToState(this, "PointerOver", true);
    }

    private void RootBorder_OnPointerReleased(object sender, PointerRoutedEventArgs e)
    {
        if (_activePointerId != e.Pointer.PointerId)
            return;

        _activePointerId = null;
        VisualStateManager.GoToState(this, "Normal", true);

        if (IsInTrailingContent(e.OriginalSource as DependencyObject))
            return;

        var pt = e.GetCurrentPoint(RootBorder);
        if (pt.Position.X >= 0 && pt.Position.Y >= 0 &&
            pt.Position.X <= RootBorder.ActualWidth &&
            pt.Position.Y <= RootBorder.ActualHeight)
        {
            Click?.Invoke(this, new RoutedEventArgs());
        }
    }

    private bool IsInTrailingContent(DependencyObject? source)
    {
        while (source is not null)
        {
            if (ReferenceEquals(source, TrailingHost))
                return true;
            source = VisualTreeHelper.GetParent(source);
        }

        return false;
    }

    private static void OnTrailingContentChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is not ThemeQuickActionCard card)
            return;

        card.TrailingHost.Content = e.NewValue;
        card.TrailingHost.Visibility = e.NewValue is null ? Visibility.Collapsed : Visibility.Visible;
        card.UpdateCursor();
    }

    private void UpdateCursor()
    {
        ProtectedCursor = InputSystemCursor.Create(
            TrailingContent is null ? InputSystemCursorShape.Hand : InputSystemCursorShape.Arrow);
    }
}
