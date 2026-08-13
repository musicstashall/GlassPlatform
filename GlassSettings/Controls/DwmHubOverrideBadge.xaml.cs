using GlassSettings.Strings;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Controls;

public sealed partial class DwmHubOverrideBadge : UserControl
{
    public static readonly DependencyProperty LabelProperty =
        DependencyProperty.Register(
            nameof(Label),
            typeof(string),
            typeof(DwmHubOverrideBadge),
            new PropertyMetadata(string.Empty, OnLabelChanged));

    public DwmHubOverrideBadge()
    {
        InitializeComponent();
        Label = GlassSettingsResources.Get("Dwm_HubOverrideBadge");
    }

    public string Label
    {
        get => (string)GetValue(LabelProperty);
        set => SetValue(LabelProperty, value);
    }

    private static void OnLabelChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is DwmHubOverrideBadge badge)
            badge.LabelText.Text = e.NewValue as string ?? string.Empty;
    }
}
