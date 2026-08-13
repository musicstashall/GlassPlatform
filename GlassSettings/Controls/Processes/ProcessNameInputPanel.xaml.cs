using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Windows.System;

namespace GlassSettings.Controls.Processes;

public sealed partial class ProcessNameInputPanel : UserControl
{
    private readonly string? _initialText;
    private HostedDialogHostContext? _dialogContext;

    public ProcessNameInputPanel(string? initialText = null)
    {
        _initialText = initialText;
        PlaceholderText = GlassSettingsResources.Get("Attach_ProcessNameInputPlaceholder");
        InitializeComponent();

        if (!string.IsNullOrEmpty(_initialText))
            InputBox.Text = _initialText;
    }

    public string PlaceholderText { get; }

    public void AttachDialogContext(HostedDialogHostContext context) =>
        _dialogContext = context;

    public string GetEnteredText() => InputBox.Text;

    private void ProcessNameInputPanel_OnLoaded(object sender, RoutedEventArgs e)
    {
        InputBox.Focus(FocusState.Programmatic);
        if (!string.IsNullOrEmpty(InputBox.Text))
            InputBox.SelectAll();
    }

    private void InputBox_OnPreviewKeyDown(object sender, KeyRoutedEventArgs e)
    {
        if (e.Key != VirtualKey.Enter)
            return;

        e.Handled = true;
        _dialogContext?.ClosePrimary();
    }
}
