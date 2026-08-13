using GlassSettings.Strings;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Controls;

public sealed partial class GlassSettingsLabeledToggleSwitch : UserControl
{
    public static readonly DependencyProperty ShowStateTextProperty =
        DependencyProperty.Register(
            nameof(ShowStateText),
            typeof(bool),
            typeof(GlassSettingsLabeledToggleSwitch),
            new PropertyMetadata(true, OnShowStateTextChanged));

    private bool _suppress;

    public event RoutedEventHandler? Toggled;

    public GlassSettingsLabeledToggleSwitch()
    {
        InitializeComponent();
        UpdateStateText();
        Loaded += (_, _) => SyncInnerEnabled();
        IsEnabledChanged += (_, _) => SyncInnerEnabled();
    }

    public bool ShowStateText
    {
        get => (bool)GetValue(ShowStateTextProperty);
        set => SetValue(ShowStateTextProperty, value);
    }

    public bool IsOn
    {
        get => InnerSwitch.IsOn;
        set
        {
            _suppress = true;
            try
            {
                InnerSwitch.IsOn = value;
                // После программного IsOn при смене IsEnabled WinUI может оставить
                // локальный InnerSwitch.IsEnabled=false — всегда синхронизируем.
                SyncInnerEnabled();
                UpdateStateText();
            }
            finally
            {
                _suppress = false;
            }
        }
    }

    /// <summary>
    /// Синхронизировать IsEnabled внутреннего ToggleSwitch с UserControl.
    /// Нужен после wipe/disable: локальный false на InnerSwitch не восстанавливается
    /// наследованием, если UserControl.IsEnabled не менялся (no-op).
    /// </summary>
    public void SyncInnerEnabled() => InnerSwitch.IsEnabled = IsEnabled;

    private void InnerSwitch_OnToggled(object sender, RoutedEventArgs e)
    {
        UpdateStateText();
        if (!_suppress)
            Toggled?.Invoke(this, e);
    }

    private static void OnShowStateTextChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is GlassSettingsLabeledToggleSwitch toggle)
            toggle.UpdateStateText();
    }

    private void UpdateStateText()
    {
        if (!ShowStateText)
        {
            StateText.Visibility = Visibility.Collapsed;
            return;
        }

        StateText.Visibility = Visibility.Visible;
        StateText.Text = InnerSwitch.IsOn
            ? GlassSettingsResources.Get("Toggle_On")
            : GlassSettingsResources.Get("Toggle_Off");
    }
}
