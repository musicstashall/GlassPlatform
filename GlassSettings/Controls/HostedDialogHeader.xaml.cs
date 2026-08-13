using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Controls;

public sealed partial class HostedDialogHeader : UserControl
{
    public event EventHandler? CloseRequested;

    public HostedDialogHeader()
    {
        InitializeComponent();
        ApplyLocalizedStrings();
    }

    public string Title
    {
        get => TitleText.Text;
        set => TitleText.Text = value;
    }

    public void ApplyLocalizedStrings()
    {
        var closeLabel = GlassSettingsResources.Get("Dialog_Close");
        GlassSettingsToolTipService.SetToolTip(CloseButton, closeLabel);
        AutomationProperties.SetName(CloseButton, closeLabel);
    }

    private void CloseButton_OnClick(object sender, RoutedEventArgs e) =>
        CloseRequested?.Invoke(this, EventArgs.Empty);
}
