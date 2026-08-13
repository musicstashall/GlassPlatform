using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Windows.UI;

namespace GlassSettings.Controls;

/// <summary>Подпись + split-button для PlatformSchedulerRow (HeaderActions / ActionsBelowHost).</summary>
public sealed partial class PlatformSchedulerStackableActions : UserControl
{
    private const double SplitFlyoutPartWidth = 36;

    private enum StatusTone
    {
        Disabled,
        Running,
        Ready,
        Unloading,
    }

    private bool _statusHovered;
    private string _lastStatusText = string.Empty;
    private StatusTone _lastTone = StatusTone.Disabled;
    private Brush? _statusBaseBrush;

    public event EventHandler<SplitButtonClickEventArgs>? StatusSplitClicked;
    public event EventHandler<RoutedEventArgs>? MenuToggleClicked;
    public event EventHandler<RoutedEventArgs>? MenuDeleteClicked;

    public PlatformSchedulerStackableActions()
    {
        InitializeComponent();
        ApplyLocalizedStrings();
        StatusSplit.SizeChanged += (_, _) => UpdateStatusFillSize();
        ActualThemeChanged += (_, _) => RefreshStatusChrome();
    }

    public void ApplyLocalizedStrings() =>
        MenuDelete.Text = GlassSettingsResources.Get("Status_DeleteTask");

    public void SetOrchestrationKindLabel(string text) =>
        OrchestrationKindText.Text = text;

    public void SetMenuToggleText(string text) =>
        MenuToggle.Text = text;

    public void SetSplitEnabled(bool enabled) =>
        StatusSplit.IsEnabled = enabled;

    public void SetBusyVisual(bool busy, bool splitEnabled)
    {
        StatusSplit.IsEnabled = splitEnabled && !busy;
        StatusSplit.Opacity = busy ? 0.55 : 1.0;
        BusyRing.Visibility = busy ? Visibility.Visible : Visibility.Collapsed;
    }

    private void SetStatus(string text, StatusTone tone)
    {
        _lastStatusText = text;
        StatusText.Text = text;
        ApplyStatusBrush(tone);
        UpdateStatusFillSize();
    }

    public void RefreshStatusChrome()
    {
        if (string.IsNullOrEmpty(_lastStatusText))
            return;

        StatusText.Text = _lastStatusText;
        ApplyStatusBrush(_lastTone);
        UpdateStatusFillSize();
    }

    private void ApplyStatusBrush(StatusTone tone)
    {
        _lastTone = tone;
        _statusBaseBrush = ResolveStatusBrush(tone);
        StatusColorFill.Background = _statusHovered && _statusBaseBrush is not null
            ? LightenStatusBrush(_statusBaseBrush)
            : _statusBaseBrush;
    }

    private static Brush LightenStatusBrush(Brush brush, byte delta = 28)
    {
        if (brush is not SolidColorBrush solid)
            return brush;

        var c = solid.Color;
        return new SolidColorBrush(Color.FromArgb(
            c.A,
            (byte)Math.Min(255, c.R + delta),
            (byte)Math.Min(255, c.G + delta),
            (byte)Math.Min(255, c.B + delta)));
    }

    private void StatusSplit_OnPointerEntered(object sender, PointerRoutedEventArgs e)
    {
        if (!StatusSplit.IsEnabled || _statusBaseBrush is null)
            return;

        _statusHovered = true;
        StatusColorFill.Background = LightenStatusBrush(_statusBaseBrush);
    }

    private void StatusSplit_OnPointerExited(object sender, PointerRoutedEventArgs e)
    {
        _statusHovered = false;
        if (_statusBaseBrush is not null)
            StatusColorFill.Background = _statusBaseBrush;
    }

    private void UpdateStatusFillSize()
    {
        if (StatusSplit.ActualWidth <= 0 || StatusSplit.ActualHeight <= 0)
            return;

        var primaryWidth = Math.Max(1, StatusSplit.ActualWidth - SplitFlyoutPartWidth);
        StatusColorFill.Width = primaryWidth;
        StatusColorFill.Height = StatusSplit.ActualHeight;
    }

    private static Brush ResolveStatusBrush(StatusTone tone) => tone switch
    {
        StatusTone.Running => SettingsAlertPalette.ResolveForegroundBrush(SettingsNoticeSeverity.Success),
        StatusTone.Ready => SettingsAlertPalette.ResolveForegroundBrush(SettingsNoticeSeverity.Warning),
        StatusTone.Unloading => new SolidColorBrush(PlatformStatusColors.GlassPlatformUnloading),
        _ => new SolidColorBrush(Color.FromArgb(255, 160, 160, 160)),
    };

    private void StatusSplit_OnClick(SplitButton sender, SplitButtonClickEventArgs args) =>
        StatusSplitClicked?.Invoke(this, args);

    private void MenuToggle_OnClick(object sender, RoutedEventArgs e) =>
        MenuToggleClicked?.Invoke(this, e);

    private void MenuDelete_OnClick(object sender, RoutedEventArgs e) =>
        MenuDeleteClicked?.Invoke(this, e);

    public enum StatusToneKind
    {
        Disabled,
        Running,
        Ready,
        Unloading,
    }

    public void SetStatusTone(string text, StatusToneKind tone) =>
        SetStatus(text, tone switch
        {
            StatusToneKind.Running => StatusTone.Running,
            StatusToneKind.Ready => StatusTone.Ready,
            StatusToneKind.Unloading => StatusTone.Unloading,
            _ => StatusTone.Disabled
        });
}
