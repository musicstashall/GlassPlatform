using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.Foundation;

namespace GlassSettings.Controls;

/// <summary>Ячейка таблицы контролов шапки: подпись (Auto) + контрол.</summary>
public sealed partial class SpoilerHeaderControlCell : UserControl
{
    private const double LabelControlGap = 8;

    public SpoilerHeaderControlCell()
    {
        InitializeComponent();
    }

    public string Label
    {
        get => LabelText.Text;
        set
        {
            LabelText.Text = value ?? string.Empty;
            UpdateLabelVisibility();
        }
    }

    public UIElement? Control
    {
        get => ControlHost.Content as UIElement;
        set
        {
            if (value is not null)
                SpoilerHeaderLayoutMetrics.DetachFromVisualParent(value);

            NormalizeToggleSwitchWidth(value);

            ControlHost.Content = value;
            ControlHost.Visibility = value is null ? Visibility.Collapsed : Visibility.Visible;
            Grid.SetColumn(ControlHost, string.IsNullOrEmpty(LabelText.Text) ? 0 : 1);
            if (value is null)
                Grid.SetColumnSpan(ControlHost, 2);
            else if (string.IsNullOrEmpty(LabelText.Text))
                Grid.SetColumnSpan(ControlHost, 2);
            else
                ControlHost.ClearValue(Grid.ColumnSpanProperty);
        }
    }

    public bool HasContent => Control is not null;

    public bool IsAdaptiveControl => ClassifyControl(Control) == ControlWidthKind.AdaptiveSlider;

    /// <summary>Минимальная ширина ячейки (slider min / fixed desired).</summary>
    public double MeasureMinWidth()
    {
        if (Control is null)
            return 0;

        var label = MeasureLabelWidth();
        var controlMin = MeasureControlMinWidth(Control);
        return label + (label > 0 && controlMin > 0 ? LabelControlGap : 0) + controlMin;
    }

    /// <summary>Максимальная ширина ячейки (slider max / fixed desired).</summary>
    public double MeasureMaxWidth()
    {
        if (Control is null)
            return 0;

        var label = MeasureLabelWidth();
        var controlMax = MeasureControlMaxWidth(Control);
        return label + (label > 0 && controlMax > 0 ? LabelControlGap : 0) + controlMax;
    }

    /// <summary>Inline-ширина ячейки при заданном budget (adaptive slider clamp).</summary>
    public double ApplyInlineWidth(double budget)
    {
        if (Control is null)
            return 0;

        var label = MeasureLabelWidth();
        var gap = label > 0 ? LabelControlGap : 0;
        var controlBudget = Math.Max(0, budget - label - gap);
        var appliedControl = ApplyControlInlineWidth(Control, controlBudget);
        Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
        return label + gap + appliedControl;
    }

    /// <summary>Stacked: выравнивание вправо, adaptive slider на max available.</summary>
    public void ApplyStackedChrome(double availableWidth)
    {
        HorizontalAlignment = HorizontalAlignment.Right;
        if (Control is null)
            return;

        var label = MeasureLabelWidth();
        var gap = label > 0 ? LabelControlGap : 0;
        var controlBudget = Math.Max(0, availableWidth - label - gap);
        ApplyControlInlineWidth(Control, controlBudget);
    }

    internal static void SyncLabelColumnWidths(IReadOnlyList<SpoilerHeaderControlCell> cells)
    {
        var max = 0d;
        foreach (var cell in cells)
        {
            if (!cell.HasContent)
                continue;
            max = Math.Max(max, cell.MeasureLabelWidth());
        }

        if (max <= 0)
            return;

        foreach (var cell in cells)
        {
            if (!cell.HasContent || string.IsNullOrEmpty(cell.LabelText.Text))
                continue;

            cell.LabelText.Width = max;
            cell.LabelText.MinWidth = max;
        }
    }

    private void UpdateLabelVisibility()
    {
        var hasLabel = !string.IsNullOrEmpty(LabelText.Text);
        LabelText.Visibility = hasLabel ? Visibility.Visible : Visibility.Collapsed;
        if (Control is not null)
        {
            Grid.SetColumn(ControlHost, hasLabel ? 1 : 0);
            if (!hasLabel)
                Grid.SetColumnSpan(ControlHost, 2);
            else
                ControlHost.ClearValue(Grid.ColumnSpanProperty);
        }
    }

    private double MeasureLabelWidth()
    {
        if (string.IsNullOrEmpty(LabelText.Text) || LabelText.Visibility != Visibility.Visible)
            return 0;

        LabelText.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
        return Math.Ceiling(LabelText.DesiredSize.Width);
    }

    /// <summary>
    /// Шаблон WinUI ToggleSwitch резервирует ~154 px под On/Off-контент —
    /// в ячейке шапки зажимаем до видимой «пилюли» 44 px (как gate спойлера).
    /// </summary>
    private static void NormalizeToggleSwitchWidth(UIElement? control)
    {
        if (control is not ToggleSwitch toggle)
            return;

        toggle.Width = ToggleSwitchPillWidth;
        toggle.MinWidth = ToggleSwitchPillWidth;
        toggle.MaxWidth = ToggleSwitchPillWidth;
    }

    private const double ToggleSwitchPillWidth = 44;

    private enum ControlWidthKind
    {
        Fixed,
        AdaptiveSlider
    }

    private static ControlWidthKind ClassifyControl(UIElement? control) =>
        control switch
        {
            null => ControlWidthKind.Fixed,
            Slider => ControlWidthKind.AdaptiveSlider,
            Grid grid when SettingsSliderFactory.IsHeaderSliderPanel(grid) => ControlWidthKind.AdaptiveSlider,
            _ => ControlWidthKind.Fixed
        };

    private static double MeasureControlMinWidth(UIElement control) =>
        ClassifyControl(control) switch
        {
            ControlWidthKind.AdaptiveSlider => MeasureAdaptiveMin(control),
            _ => MeasureFixedWidth(control)
        };

    private static double MeasureControlMaxWidth(UIElement control) =>
        ClassifyControl(control) switch
        {
            ControlWidthKind.AdaptiveSlider => MeasureAdaptiveMax(control),
            _ => MeasureFixedWidth(control)
        };

    private static double MeasureAdaptiveMin(UIElement control)
    {
        if (control is Grid labeled && SettingsSliderFactory.IsLabeledHeaderSliderPanel(labeled))
            return SettingsSliderFactory.MeasureLabeledHeaderSliderMinWidth(labeled);

        if (control is Grid suffix && SettingsSliderFactory.IsSliderValueSuffixPanel(suffix))
            return GlassSettingsSliderMetrics.SuffixPanelMinWidth;

        return GlassSettingsSliderMetrics.TrackMinWidth;
    }

    private static double MeasureAdaptiveMax(UIElement control)
    {
        if (control is Grid labeled && SettingsSliderFactory.IsLabeledHeaderSliderPanel(labeled))
        {
            var suffixCol = SettingsSliderFactory.MeasureSuffixColumnWidth(FindSuffixPanel(labeled)!);
            return GlassSettingsSliderMetrics.TrackMaxWidth + suffixCol
                   + SettingsSliderFactory.MeasureLabeledHeaderSliderLabelColumnWidth(labeled) + 8;
        }

        if (control is Grid suffix && SettingsSliderFactory.IsSliderValueSuffixPanel(suffix))
            return GlassSettingsSliderMetrics.SuffixPanelMaxWidth;

        return GlassSettingsSliderMetrics.TrackMaxWidth;
    }

    private static double MeasureFixedWidth(UIElement control)
    {
        if (control is FrameworkElement fe)
        {
            fe.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
            return Math.Ceiling(fe.DesiredSize.Width);
        }

        return 0;
    }

    private static double ApplyControlInlineWidth(UIElement control, double budget)
    {
        if (ClassifyControl(control) == ControlWidthKind.AdaptiveSlider)
            return ApplyAdaptiveInlineWidth(control, budget);

        if (control is FrameworkElement fe)
        {
            fe.HorizontalAlignment = HorizontalAlignment.Left;
            // ToggleSwitch остаётся зажатым на 44 px — сброс вернул бы шаблонные ~154 px.
            if (fe is not ToggleSwitch)
            {
                fe.ClearValue(FrameworkElement.MaxWidthProperty);
                fe.ClearValue(FrameworkElement.WidthProperty);
            }
            fe.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
            return Math.Ceiling(fe.DesiredSize.Width);
        }

        return 0;
    }

    private static double ApplyAdaptiveInlineWidth(UIElement control, double budget)
    {
        if (control is Grid labeled && SettingsSliderFactory.IsLabeledHeaderSliderPanel(labeled))
        {
            labeled.HorizontalAlignment = HorizontalAlignment.Stretch;
            var labelW = SettingsSliderFactory.MeasureLabeledHeaderSliderLabelColumnWidth(labeled);
            var suffixReserve = GlassSettingsSliderMetrics.SuffixReserve;
            var track = GlassSettingsSliderMetrics.ClampTrack(Math.Max(0, budget - labelW - 8 - suffixReserve));
            SettingsSliderFactory.ApplyAdaptiveLabeledHeaderSliderWidth(labeled, track);
            labeled.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
            return Math.Ceiling(labeled.DesiredSize.Width);
        }

        if (control is Grid suffix && SettingsSliderFactory.IsSliderValueSuffixPanel(suffix))
        {
            suffix.HorizontalAlignment = HorizontalAlignment.Stretch;
            SettingsSliderFactory.NormalizeSuffixPanelSliderChrome(suffix);
            var suffixCol = SettingsSliderFactory.MeasureSuffixColumnWidth(suffix);
            var track = GlassSettingsSliderMetrics.ClampTrack(Math.Max(0, budget - suffixCol));
            SettingsSliderFactory.ApplyAdaptiveSliderPanelWidth(suffix, track);
            suffix.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
            return Math.Ceiling(suffix.DesiredSize.Width);
        }

        if (control is Slider slider)
        {
            SettingsSliderFactory.Apply(slider, slider.Minimum, slider.Maximum, stretch: true);
            var track = GlassSettingsSliderMetrics.ClampTrack(budget);
            slider.Width = track;
            slider.MaxWidth = track;
            return track;
        }

        return 0;
    }

    private static Grid? FindSuffixPanel(Grid labeledPanel)
    {
        foreach (var child in labeledPanel.Children)
        {
            if (child is Grid grid && Grid.GetColumn(grid) == 1 && SettingsSliderFactory.IsSliderValueSuffixPanel(grid))
                return grid;
        }

        return null;
    }
}
