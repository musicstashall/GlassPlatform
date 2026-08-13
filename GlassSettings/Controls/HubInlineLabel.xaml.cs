using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Controls;

public sealed partial class HubInlineLabel : UserControl
{
    public HubInlineLabel() => InitializeComponent();

    public string Label
    {
        get => LabelText.Text;
        set => LabelText.Text = value;
    }

    public UIElement? Control
    {
        get => ControlHost.Content as UIElement;
        set => ControlHost.Content = value;
    }
}
