using GlassSettings.Core.Snapshot;
using GlassSettings.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Controls;

/// <summary>Заголовок вкладки Pivot: точка несохранённых изменений перед подписью.</summary>
public sealed class SettingsPivotTabHeader
{
    public UnsavedIndicatorDot Dot { get; }

    public StackPanel Root { get; }

    private readonly TextBlock _label;
    private readonly TextBlock _parallelMarker;

    private SettingsPivotTabHeader(
        UnsavedIndicatorDot dot,
        TextBlock label,
        TextBlock parallelMarker,
        StackPanel root)
    {
        Dot = dot;
        _label = label;
        _parallelMarker = parallelMarker;
        Root = root;
    }

    public static SettingsPivotTabHeader Create(string labelText)
    {
        var dot = new UnsavedIndicatorDot
        {
            VerticalAlignment = VerticalAlignment.Center,
            Visibility = Visibility.Collapsed,
            Severity = SettingsNoticeSeverity.Warning
        };
        var label = new TextBlock
        {
            Text = labelText,
            VerticalAlignment = VerticalAlignment.Center
        };
        var titleLine = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 6,
            Children = { dot, label }
        };
        var parallelMarker = new TextBlock
        {
            Foreground = AppThemeVisualResolver.ResolveThemeBrush("SystemFillColorCautionBrush"),
            Style = Application.Current.Resources["CaptionTextBlockStyle"] as Style,
            Visibility = Visibility.Collapsed
        };
        var root = new StackPanel
        {
            Orientation = Orientation.Vertical,
            Spacing = 1,
            Children = { titleLine, parallelMarker }
        };
        return new SettingsPivotTabHeader(dot, label, parallelMarker, root);
    }

    public void SetLabel(string labelText) => _label.Text = labelText;

    public void SetUnsavedVisible(bool visible)
    {
        Dot.Visibility = visible ? Visibility.Visible : Visibility.Collapsed;
        if (visible)
            Dot.RefreshChrome();
    }

    public void SetParallelBranchWarning(bool visible, string? markerText, string? toolTip)
    {
        _parallelMarker.Text = markerText ?? string.Empty;
        _parallelMarker.Visibility = visible ? Visibility.Visible : Visibility.Collapsed;
        ToolTipService.SetToolTip(_parallelMarker, visible ? toolTip : null);
    }

    public void RefreshUnsaved(SettingsCommitScope? scope)
    {
        var pending = scope is { } s && SettingsSession.Current.HasUnsavedInScope(s);
        Dot.Visibility = pending ? Visibility.Visible : Visibility.Collapsed;
        if (pending)
            Dot.RefreshChrome();
    }
}
