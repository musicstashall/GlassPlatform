using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Controls;

public sealed partial class PlatformLibraryRow : UserControl
{
    public event EventHandler? Changed;
    public event EventHandler? StatusMenuRequested;

    private bool _suppressNotify;

    public PlatformLibraryRow() => InitializeComponent();

    public void SetStatusItems(string[] labels, int selected)
    {
        _suppressNotify = true;
        try
        {
            StatusCombo.Items.Clear();
            foreach (var l in labels) StatusCombo.Items.Add(l);
            if (selected >= 0 && selected < labels.Length) StatusCombo.SelectedIndex = selected;
        }
        finally
        {
            _suppressNotify = false;
        }
    }

    public bool IsOn
    {
        get => EnableSwitch.IsOn;
        set
        {
            if (EnableSwitch.IsOn == value) return;
            _suppressNotify = true;
            try { EnableSwitch.IsOn = value; }
            finally { _suppressNotify = false; }
        }
    }

    private void EnableSwitch_OnToggled(object sender, RoutedEventArgs e)
    {
        if (!_suppressNotify) Changed?.Invoke(this, EventArgs.Empty);
    }

    private void StatusCombo_OnSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (!_suppressNotify) Changed?.Invoke(this, EventArgs.Empty);
    }
    private void ActionsBtn_OnClick(object sender, RoutedEventArgs e) => StatusMenuRequested?.Invoke(this, EventArgs.Empty);
}
