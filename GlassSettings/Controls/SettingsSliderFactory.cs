using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.Foundation;

namespace GlassSettings.Controls;

/// <summary>Фабрика слайдеров; min/max трека — <see cref="GlassSettingsSliderMetrics"/>.</summary>
public static class SettingsSliderFactory
{
    public static double MinWidth => GlassSettingsSliderMetrics.TrackMinWidth;
    public static double MaxWidth => GlassSettingsSliderMetrics.TrackMaxWidth;
    public static double DefaultSuffixReserve => GlassSettingsSliderMetrics.SuffixReserve;

    public static double SuffixPanelMinWidth(double suffixReserve = 48) =>
        GlassSettingsSliderMetrics.TrackMinWidth + suffixReserve;

    public static double SuffixPanelMaxWidth(double suffixReserve = 48) =>
        GlassSettingsSliderMetrics.TrackMaxWidth + suffixReserve;

    /// <summary>Grid из CreateSliderValueSuffixPanel: star-колонка слайдера + Auto suffix.</summary>
    public static bool IsSliderValueSuffixPanel(Grid panel) =>
        panel.ColumnDefinitions.Count >= 2
        && panel.ColumnDefinitions[0].Width.GridUnitType == GridUnitType.Star
        && panel.ColumnDefinitions[1].Width == GridLength.Auto;

    public const string LabeledHeaderSliderTag = "GlassSettingsLabeledHeaderSliderPanel";

    private static double _syncedLabeledHeaderSliderLabelWidth;

    /// <summary>Шапка спойлера: подпись + slider+suffix; переносится на новую строку целиком.</summary>
    public static bool IsLabeledHeaderSliderPanel(Grid panel) =>
        Equals(panel.Tag, LabeledHeaderSliderTag);

    public static bool IsHeaderSliderPanel(Grid? panel) =>
        panel is not null && (IsSliderValueSuffixPanel(panel) || IsLabeledHeaderSliderPanel(panel));

    public static Slider Create(double minimum = 0, double maximum = 100, bool stretch = false)
    {
        var slider = new Slider();
        Apply(slider, minimum, maximum, stretch);
        return slider;
    }

    public static void Apply(Slider slider, double minimum = 0, double maximum = 100, bool stretch = false)
    {
        slider.Minimum = minimum;
        slider.Maximum = maximum;
        slider.MinWidth = MinWidth;
        slider.MaxWidth = MaxWidth;
        slider.VerticalAlignment = VerticalAlignment.Center;

        if (stretch)
        {
            slider.ClearValue(FrameworkElement.WidthProperty);
            slider.HorizontalAlignment = HorizontalAlignment.Stretch;
        }
        else
        {
            slider.Width = MaxWidth;
            slider.HorizontalAlignment = HorizontalAlignment.Left;
        }
    }

    /// <summary>Слайдер + суффикс (напр. «NN%»): трек 120–200 px, адаптивная ширина.</summary>
    public static Grid CreateSliderValueSuffixPanel(Slider slider, TextBlock valueText, double suffixWidth = 48)
    {
        Apply(slider, slider.Minimum, slider.Maximum, stretch: true);

        var panel = new Grid
        {
            HorizontalAlignment = HorizontalAlignment.Stretch,
            VerticalAlignment = VerticalAlignment.Center,
            ColumnSpacing = 8,
            MinWidth = MinWidth + suffixWidth,
            MaxWidth = MaxWidth + suffixWidth
        };
        panel.ColumnDefinitions.Add(new ColumnDefinition
        {
            Width = new GridLength(1, GridUnitType.Star),
            MinWidth = MinWidth,
            MaxWidth = MaxWidth
        });
        panel.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        panel.Children.Add(slider);
        panel.Children.Add(valueText);
        Grid.SetColumn(valueText, 1);
        return panel;
    }

    /// <summary>Подпись параметра + slider+suffix для шапки спойлера (коллекции DWM Theme).</summary>
    public static Grid CreateLabeledHeaderSliderPanel(
        string labelText,
        Slider slider,
        TextBlock valueText,
        Style? labelStyle = null,
        double labelGap = 8)
    {
        var suffixPanel = CreateSliderValueSuffixPanel(slider, valueText);

        var label = new TextBlock
        {
            Text = labelText,
            Style = labelStyle ?? Application.Current.Resources["GlassSettingsSpoilerSecondaryTextStyle"] as Style,
            VerticalAlignment = VerticalAlignment.Center,
            TextTrimming = TextTrimming.CharacterEllipsis
        };

        var panel = new Grid
        {
            Tag = LabeledHeaderSliderTag,
            HorizontalAlignment = HorizontalAlignment.Stretch,
            VerticalAlignment = VerticalAlignment.Center,
            ColumnSpacing = labelGap
        };
        panel.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        panel.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        panel.Children.Add(label);
        panel.Children.Add(suffixPanel);
        Grid.SetColumn(suffixPanel, 1);
        return panel;
    }

    public static TextBlock? TryGetLabeledHeaderSliderLabel(Grid panel) =>
        IsLabeledHeaderSliderPanel(panel) && panel.Children.Count > 0 && panel.Children[0] is TextBlock label
            ? label
            : null;

    public static Grid? TryGetLabeledHeaderSliderSuffixPanel(Grid panel) =>
        IsLabeledHeaderSliderPanel(panel) && panel.Children.Count > 1 && panel.Children[1] is Grid suffix
            ? suffix
            : null;

    public static double MeasureLabeledHeaderSliderLabelColumnWidth(Grid panel)
    {
        if (TryGetLabeledHeaderSliderLabel(panel) is not TextBlock label)
            return _syncedLabeledHeaderSliderLabelWidth;

        if (label.Width > 0)
            return label.Width;

        label.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
        return Math.Ceiling(label.DesiredSize.Width);
    }

    public static double MeasureLabeledHeaderSliderSuffixReserve(Grid panel)
    {
        if (TryGetLabeledHeaderSliderSuffixPanel(panel) is not Grid suffixPanel)
            return DefaultSuffixReserve;

        return MeasureSuffixColumnWidth(suffixPanel);
    }

    /// <summary>Выравнивает ширину колонки подписи между несколькими шапками спойлеров.</summary>
    public static void SyncLabeledHeaderSliderLabelWidths(IReadOnlyList<Grid> panels)
    {
        var labels = new List<TextBlock>();
        var max = 0d;

        foreach (var panel in panels)
        {
            if (TryGetLabeledHeaderSliderLabel(panel) is not TextBlock label)
                continue;

            label.ClearValue(FrameworkElement.WidthProperty);
            label.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
            max = Math.Max(max, Math.Ceiling(label.DesiredSize.Width));
            labels.Add(label);
        }

        if (max <= 0)
            return;

        _syncedLabeledHeaderSliderLabelWidth = max;
        foreach (var label in labels)
            label.Width = max;
    }

    /// <summary>Выравнивает ширину трека между парными labeled-panel (коллекции Theme).</summary>
    public static void SyncLabeledHeaderSliderTrackWidths(IReadOnlyList<Grid> panels)
    {
        if (panels.Count < 2)
            return;

        var tracks = new List<double>(panels.Count);
        foreach (var panel in panels)
        {
            if (TryGetLabeledHeaderSliderSuffixPanel(panel) is not Grid suffixPanel)
                continue;

            tracks.Add(ReadAppliedTrackWidth(suffixPanel));
        }

        if (tracks.Count < 2)
            return;

        var synced = tracks.Min();
        if (tracks.All(track => Math.Abs(track - synced) < 0.5))
            return;

        foreach (var panel in panels)
            ApplyAdaptiveLabeledHeaderSliderWidth(panel, synced);
    }

    private static double ReadAppliedTrackWidth(Grid suffixPanel)
    {
        if (suffixPanel.Width > 0 && !double.IsNaN(suffixPanel.Width))
        {
            var suffixColWidth = MeasureSuffixColumnWidth(suffixPanel);
            return GlassSettingsSliderMetrics.ClampTrack(suffixPanel.Width - suffixColWidth);
        }

        foreach (var child in suffixPanel.Children)
        {
            if (child is Slider slider && Grid.GetColumn(slider) == 0 && slider.MaxWidth > 0)
                return slider.MaxWidth;
        }

        if (suffixPanel.ColumnDefinitions.Count > 0 && suffixPanel.ColumnDefinitions[0].MaxWidth > 0)
            return suffixPanel.ColumnDefinitions[0].MaxWidth;

        return MaxWidth;
    }

    public static double MeasureLabeledHeaderSliderMinWidth(Grid panel)
    {
        if (!IsLabeledHeaderSliderPanel(panel))
            return 0;

        var labelWidth = MeasureLabeledHeaderSliderLabelColumnWidth(panel);
        return labelWidth + panel.ColumnSpacing + GlassSettingsSliderMetrics.SuffixPanelMinWidth;
    }

    public static double ApplyAdaptiveLabeledHeaderSliderWidth(Grid panel, double sliderTrackWidth)
    {
        if (TryGetLabeledHeaderSliderSuffixPanel(panel) is not Grid suffixPanel)
            return 0;

        return ApplyAdaptiveSliderPanelWidth(suffixPanel, sliderTrackWidth);
    }

    /// <summary>Снимает фиксированные min/max с suffix-panel для body (сжатие до метрик).</summary>
    public static void PrepareSuffixPanelForBody(Grid panel)
    {
        if (!IsSliderValueSuffixPanel(panel))
            return;

        panel.ClearValue(FrameworkElement.MinWidthProperty);
        panel.ClearValue(FrameworkElement.MaxWidthProperty);
        if (panel.ColumnDefinitions.Count > 0)
            panel.ColumnDefinitions[0].ClearValue(ColumnDefinition.MinWidthProperty);
    }

    public static double MeasureSuffixColumnWidth(Grid panel)
    {
        if (!IsSliderValueSuffixPanel(panel))
            return 0;

        foreach (var child in panel.Children)
        {
            if (child is FrameworkElement fe && Grid.GetColumn(fe) == 1)
            {
                fe.Measure(new Windows.Foundation.Size(double.PositiveInfinity, double.PositiveInfinity));
                return Math.Ceiling(fe.DesiredSize.Width) + panel.ColumnSpacing;
            }
        }

        return DefaultSuffixReserve;
    }

    public static void NormalizeSuffixPanelSliderChrome(Grid panel)
    {
        if (!IsSliderValueSuffixPanel(panel))
            return;

        foreach (var child in panel.Children)
        {
            if (child is Slider slider && Grid.GetColumn(slider) == 0)
            {
                Apply(slider, slider.Minimum, slider.Maximum, stretch: true);
                return;
            }
        }
    }

    public static double ApplyAdaptiveSliderPanelWidth(Grid panel, double sliderWidth)
    {
        NormalizeSuffixPanelSliderChrome(panel);

        var suffixColWidth = MeasureSuffixColumnWidth(panel);
        var clampedSlider = GlassSettingsSliderMetrics.ClampTrack(sliderWidth);
        var panelWidth = clampedSlider + suffixColWidth;
        panel.Width = panelWidth;
        panel.MinWidth = MinWidth + suffixColWidth;
        panel.MaxWidth = MaxWidth + suffixColWidth;

        if (panel.ColumnDefinitions.Count > 0)
            panel.ColumnDefinitions[0].MaxWidth = clampedSlider;

        foreach (var child in panel.Children)
        {
            if (child is Slider slider && Grid.GetColumn(slider) == 0)
            {
                slider.MaxWidth = clampedSlider;
                break;
            }
        }

        return panelWidth;
    }

    /// <summary>Body/footer: трек 120–200 + suffix, без принудительного outer MinWidth.</summary>
    public static void ApplyBodySuffixPanelWidth(Grid panel, double panelWidth)
    {
        if (!IsSliderValueSuffixPanel(panel))
            return;

        NormalizeSuffixPanelSliderChrome(panel);
        PrepareSuffixPanelForBody(panel);

        var suffixColWidth = MeasureSuffixColumnWidth(panel);
        var cappedPanel = Math.Min(Math.Max(0, panelWidth), GlassSettingsSliderMetrics.SuffixPanelMaxWidth);
        var trackSpace = Math.Max(0, cappedPanel - suffixColWidth);
        var trackWidth = GlassSettingsSliderMetrics.ClampTrack(trackSpace);

        panel.HorizontalAlignment = HorizontalAlignment.Left;
        panel.MaxWidth = cappedPanel;
        panel.Width = cappedPanel;

        if (panel.ColumnDefinitions.Count > 0)
            panel.ColumnDefinitions[0].MaxWidth = trackWidth;

        foreach (var child in panel.Children)
        {
            if (child is Slider slider && Grid.GetColumn(slider) == 0)
            {
                Apply(slider, slider.Minimum, slider.Maximum, stretch: true);
                slider.MaxWidth = trackWidth;
                return;
            }
        }
    }

    public static double HeaderSliderActionsWidthEstimate(double suffixWidth = 48) =>
        MaxWidth + suffixWidth;

    public static bool IsLabeledSliderValueSuffixRow(Grid row) =>
        SettingsPickerBodyLayout.IsBodyTable(row) && HasSliderWithValueColumn(row);

    private static bool HasSliderWithValueColumn(Grid row)
    {
        var hasSlider = false;
        var hasValue = false;

        foreach (var child in row.Children)
        {
            if (child is Slider slider && Grid.GetColumn(slider) == 1)
            {
                hasSlider = true;
                continue;
            }

            if (child is FrameworkElement fe && Grid.GetColumn(fe) == 2)
                hasValue = true;
        }

        return hasSlider && hasValue;
    }

    public static Grid CreateLabeledSliderValueSuffixRow(
        string labelText,
        Slider slider,
        TextBlock valueText,
        Style? labelStyle = null,
        Thickness? labelMargin = null)
    {
        Apply(slider, slider.Minimum, slider.Maximum, stretch: true);
        slider.HorizontalAlignment = HorizontalAlignment.Left;
        slider.MinWidth = MinWidth;
        slider.MaxWidth = MaxWidth;

        var table = SettingsPickerBodyLayout.CreateBodyTable();
        SettingsPickerBodyLayout.AddLabeledRow(table, labelText, slider, labelStyle);
        SettingsPickerBodyLayout.AddTrailingContent(table, row: 0, trailing: valueText, leftGap: 8);
        return table;
    }

    public static void UpdateLabeledSliderValueSuffixRowLayout(Grid row)
    {
        if (SettingsPickerBodyLayout.IsBodyTable(row))
            SettingsPickerBodyLayout.UpdateBodyTableLayout(row);
    }
}
