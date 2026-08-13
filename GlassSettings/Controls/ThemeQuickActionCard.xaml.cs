using Microsoft.UI.Input;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;

namespace GlassSettings.Controls;

public sealed partial class ThemeQuickActionCard : UserControl
{
    private uint? _activePointerId;

    public event RoutedEventHandler? Click;

    public ThemeQuickActionCard()
    {
        InitializeComponent();
        Loaded += (_, _) => ProtectedCursor = InputSystemCursor.Create(InputSystemCursorShape.Hand);
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
        set => IconGlyph.Glyph = value;
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

        var pt = e.GetCurrentPoint(RootBorder);
        if (pt.Position.X >= 0 && pt.Position.Y >= 0 &&
            pt.Position.X <= RootBorder.ActualWidth &&
            pt.Position.Y <= RootBorder.ActualHeight)
        {
            Click?.Invoke(this, new RoutedEventArgs());
        }
    }
}
