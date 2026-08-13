using System.Runtime.CompilerServices;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Controls;

/// <summary>Метрики и единая body-таблица text|control для SettingsColorPickerPanel.</summary>
public static class SettingsPickerBodyLayout
{
    public const string BodyTableTag = "GlassSettingsPickerBodyTable";

    private const double DefaultPickerZoneMinWidth = 360;
    private const double DefaultPickerZoneHorizontalPadding = 32;
    private const double DefaultInfoColumnFallbackMinWidth = 200;
    private const double DefaultLabelControlGap = 16;
    private const double DefaultBodyTableRightInset = 20;
    private const double PickerSeparatorWidth = 1;
    private const double InfoColumnPaddingHorizontal = 44;

    public static double InfoColumnHorizontalPadding => InfoColumnPaddingHorizontal;

    private static readonly ConditionalWeakTable<Grid, BodyTableLayoutState> LayoutStates = new();
    private static int _layoutDepth;

    private sealed class BodyTableLayoutState
    {
        public double SyncedLabelWidth;
        public double LastTableWidth = -1;
        public double LastTrackWidth = -1;
        public double LastRightInset = -1;
    }

    public static double LabelColumnMinWidth =>
        ReadResource("GlassSettingsSectionTitleMinWidth", 150);

    public static double PickerZoneMinWidth =>
        ReadResource("GlassSettingsPickerZoneMinWidth", DefaultPickerZoneMinWidth);

    public static double PickerInlineRail
    {
        get
        {
            if (Application.Current.Resources["GlassSettingsPickerInlineRail"] is double value)
                return value;
            return PickerZoneMinWidth + DefaultPickerZoneHorizontalPadding;
        }
    }

    public static double LabelControlGap =>
        ReadResource("GlassSettingsPickerBodyLabelControlGap", DefaultLabelControlGap);

    /// <summary>Правый технологический зазор таблицы до пикера в inline.</summary>
    public static double BodyTableRightInset =>
        ReadResource("GlassSettingsPickerBodyTableRightInset", DefaultBodyTableRightInset);

    public static double InfoColumnFallbackMinWidth =>
        ReadResource("GlassSettingsPickerInfoColumnFallbackMinWidth", DefaultInfoColumnFallbackMinWidth);

    public static bool IsBodyTable(Grid? grid) =>
        grid is not null && Equals(grid.Tag, BodyTableTag);

    public readonly struct BodyTableLayoutMetrics
    {
        public double ContentWidth { get; init; }
        public double TrackWidth { get; init; }
        public double RightInset { get; init; }
        public double TableWidth { get; init; }
        public double LabelWidth { get; init; }
        public double TrailingWidth { get; init; }
    }

    public static Grid CreateBodyTable()
    {
        var grid = new Grid
        {
            HorizontalAlignment = HorizontalAlignment.Left,
            Tag = BodyTableTag
        };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        LayoutStates.Add(grid, new BodyTableLayoutState());
        return grid;
    }

    /// <summary>Одна строка: подпись + контрол (control col — слева).</summary>
    public static void AddLabeledRow(
        Grid table,
        TextBlock label,
        FrameworkElement control,
        bool topMargin = false)
    {
        var row = table.RowDefinitions.Count;
        table.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

        label.VerticalAlignment = VerticalAlignment.Center;
        label.Margin = topMargin
            ? new Thickness(0, 12, LabelControlGap, 0)
            : new Thickness(0, 0, LabelControlGap, 0);

        control.HorizontalAlignment = HorizontalAlignment.Left;
        control.VerticalAlignment = VerticalAlignment.Center;
        if (topMargin)
            control.Margin = new Thickness(0, 12, 0, 0);

        Grid.SetRow(label, row);
        Grid.SetColumn(label, 0);
        Grid.SetRow(control, row);
        Grid.SetColumn(control, 1);
        table.Children.Add(label);
        table.Children.Add(control);

        InvalidateLabelSync(table);
    }

    /// <summary>Компактная строка label|control в уже созданный row (без +12 topMargin).</summary>
    public static void PlaceLabeledRow(
        Grid table,
        int row,
        TextBlock label,
        FrameworkElement control,
        double topGap = 4)
    {
        label.VerticalAlignment = VerticalAlignment.Center;
        label.Margin = new Thickness(0, topGap, LabelControlGap, 0);
        control.HorizontalAlignment = HorizontalAlignment.Left;
        control.VerticalAlignment = VerticalAlignment.Center;
        control.Margin = new Thickness(0, topGap, 0, 0);

        Grid.SetRow(label, row);
        Grid.SetColumn(label, 0);
        Grid.SetRow(control, row);
        Grid.SetColumn(control, 1);
        table.Children.Add(label);
        table.Children.Add(control);
        InvalidateLabelSync(table);
    }

    /// <summary>Одна строка: подпись + контрол (control col — слева).</summary>
    public static void AddLabeledRow(
        Grid table,
        string labelText,
        FrameworkElement control,
        Style? labelStyle = null,
        bool topMargin = false)
    {
        var label = new TextBlock
        {
            Text = labelText,
            Style = labelStyle
        };

        AddLabeledRow(table, label, control, topMargin);
    }

    /// <summary>
    /// Одна строка с произвольной ячейкой подписи (напр. иконка + точка + заголовок + подстрока)
    /// в колонке 0 и контролом в колонке 1.
    /// </summary>
    public static void AddLabeledRowCell(
        Grid table,
        FrameworkElement labelCell,
        FrameworkElement control,
        bool topMargin = false)
    {
        var row = table.RowDefinitions.Count;
        table.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

        labelCell.VerticalAlignment = VerticalAlignment.Center;
        var current = labelCell.Margin;
        labelCell.Margin = new Thickness(
            current.Left,
            topMargin ? 12 : current.Top,
            LabelControlGap,
            current.Bottom);

        control.HorizontalAlignment = HorizontalAlignment.Left;
        control.VerticalAlignment = VerticalAlignment.Center;
        if (topMargin)
            control.Margin = new Thickness(0, 12, 0, 0);

        Grid.SetRow(labelCell, row);
        Grid.SetColumn(labelCell, 0);
        Grid.SetRow(control, row);
        Grid.SetColumn(control, 1);
        table.Children.Add(labelCell);
        table.Children.Add(control);

        InvalidateLabelSync(table);
    }

    /// <summary>Строка только с контролом (напр. CheckBox с собственным Content).</summary>
    public static void AddControlRow(Grid table, FrameworkElement control, bool topMargin = false)
    {
        var row = table.RowDefinitions.Count;
        table.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        control.HorizontalAlignment = HorizontalAlignment.Left;
        control.VerticalAlignment = VerticalAlignment.Center;
        if (topMargin)
            control.Margin = new Thickness(0, 12, 0, 0);

        Grid.SetRow(control, row);
        Grid.SetColumn(control, 1);
        table.Children.Add(control);
    }

    /// <summary>Добавляет optional trailing-контент в третью колонку body-таблицы.</summary>
    public static void AddTrailingContent(
        Grid table,
        int row,
        FrameworkElement trailing,
        bool topMargin = false,
        double leftGap = 8)
    {
        trailing.VerticalAlignment = VerticalAlignment.Center;
        trailing.HorizontalAlignment = HorizontalAlignment.Left;
        trailing.Margin = topMargin
            ? new Thickness(leftGap, 12, 0, 0)
            : new Thickness(leftGap, 0, 0, 0);

        Grid.SetRow(trailing, row);
        Grid.SetColumn(trailing, 2);
        table.Children.Add(trailing);
    }

    /// <summary>Оборачивает одиночный footer-контрол в body-таблицу.</summary>
    public static Grid WrapFooterControl(FrameworkElement control)
    {
        var table = CreateBodyTable();
        AddControlRow(table, control);
        return table;
    }

    public static object? NormalizeFooterContent(object? content)
    {
        if (content is null)
            return null;

        if (content is Grid grid && IsBodyTable(grid))
            return grid;

        if (content is FrameworkElement fe)
            return WrapFooterControl(fe);

        return content;
    }

    public static bool IsFooterEffectivelyVisible(object? content)
    {
        if (content is not FrameworkElement root)
            return false;

        if (root.Visibility != Visibility.Visible)
            return false;

        if (IsBodyTable(root as Grid))
            return HasVisibleBodyContent(root as Grid);

        return true;
    }

    public static double EstimateInlineMinWidth(
        bool showInfo,
        bool hasDescription,
        bool hasDefaultAction,
        object? footerContent)
    {
        return EstimateInlineMinWidth(
            showInfo,
            hasDescription,
            hasDefaultAction,
            footerContent,
            PickerInlineRail);
    }

    /// <summary>Порог inline по фактической ширине picker-rail.</summary>
    public static double EstimateInlineMinWidth(
        bool showInfo,
        bool hasDescription,
        bool hasDefaultAction,
        object? footerContent,
        double pickerRailWidth)
    {
        var rail = Math.Max(0, pickerRailWidth);
        if (!showInfo)
            return rail;

        return InfoColumnHorizontalPadding
               + EstimateMinimumInfoContentWidth(hasDescription, hasDefaultAction, footerContent)
               + PickerSeparatorWidth
               + rail
               + BodyTableRightInset;
    }

    /// <summary>Минимальная ширина info-контента (без padding) при сжатии col1 до track min.</summary>
    public static double EstimateMinimumInfoContentWidth(
        bool hasDescription,
        bool hasDefaultAction,
        object? footerContent)
    {
        if (IsFooterEffectivelyVisible(footerContent) && footerContent is FrameworkElement footerFe)
            return EstimateBodyTableMinWidth(footerFe);

        if (hasDescription || hasDefaultAction)
            return InfoColumnFallbackMinWidth;

        return LabelColumnMinWidth;
    }

    /// <summary>Полная ширина info-контента (без padding) в inline-режиме.</summary>
    public static double ComputeInlineInfoContentWidth(double panelWidth) =>
        ComputeInlineInfoContentWidth(panelWidth, PickerInlineRail);

    /// <summary>Полная ширина info-контента с учётом фактического rail пикера.</summary>
    public static double ComputeInlineInfoContentWidth(double panelWidth, double pickerRailWidth) =>
        Math.Max(
            0,
            panelWidth
            - Math.Max(0, pickerRailWidth)
            - PickerSeparatorWidth
            - InfoColumnHorizontalPadding);

    public static double EstimateBodyTableMinWidth(FrameworkElement content)
    {
        if (content is Grid table && IsBodyTable(table))
            return MeasureBodyTableMinWidth(table);

        content.Measure(new Windows.Foundation.Size(double.PositiveInfinity, double.PositiveInfinity));
        return Math.Ceiling(content.DesiredSize.Width);
    }

    public static BodyTableLayoutMetrics UpdateBodyTableLayout(Grid table, double? layoutWidthOverride = null)
    {
        if (!IsBodyTable(table) || _layoutDepth > 0)
            return default;

        var layoutWidth = layoutWidthOverride ?? table.ActualWidth;
        if (layoutWidth <= 0)
            return default;

        if (!LayoutStates.TryGetValue(table, out var state))
        {
            state = new BodyTableLayoutState();
            LayoutStates.Add(table, state);
        }

        SyncLabelColumnWidths(table, state);

        var labelWidth = state.SyncedLabelWidth > 0
            ? state.SyncedLabelWidth
            : MeasureWidestLabelWidth(table);
        var trailingWidth = MeasureTrailingRailWidth(table);
        var (trackWidth, rightInset) = ComputeBodyTableTrackAndInset(
            layoutWidth,
            labelWidth,
            trailingWidth);
        var tableWidth = labelWidth + trackWidth + trailingWidth;

        var tableTag = $"Table#{table.GetHashCode()}";
        var col0Actual = MeasureColumnActualWidth(table, 0);
        var col1Actual = MeasureColumnActualWidth(table, 1);

        if (Math.Abs(state.LastTableWidth - layoutWidth) < 0.5
            && Math.Abs(state.LastTrackWidth - trackWidth) < 0.5
            && Math.Abs(state.LastRightInset - rightInset) < 0.5
            && Math.Abs(table.Width - tableWidth) < 0.5)
        {
            TraceUpdateBodyTable(
                tableTag,
                layoutWidth,
                labelWidth,
                trackWidth,
                trailingWidth,
                tableWidth,
                rightInset,
                table.ActualWidth,
                col0Actual,
                col1Actual,
                earlyReturn: true);

            return new BodyTableLayoutMetrics
            {
                ContentWidth = layoutWidth,
                TrackWidth = trackWidth,
                RightInset = rightInset,
                TableWidth = tableWidth,
                LabelWidth = labelWidth,
                TrailingWidth = trailingWidth
            };
        }

        try
        {
            _layoutDepth++;
            state.LastTableWidth = layoutWidth;
            state.LastTrackWidth = trackWidth;
            state.LastRightInset = rightInset;

            table.HorizontalAlignment = HorizontalAlignment.Left;
            table.Margin = new Thickness(table.Margin.Left, table.Margin.Top, 0, table.Margin.Bottom);
            table.Width = tableWidth;
            table.MaxWidth = tableWidth;

            foreach (var child in table.Children)
            {
                if (child is not FrameworkElement control || Grid.GetColumn(control) != 1)
                    continue;
                if (control.Visibility != Visibility.Visible)
                    continue;

                ApplyControlColumnChrome(control, trackWidth);
            }

            foreach (var child in table.Children)
            {
                if (child is not FrameworkElement trailing || Grid.GetColumn(trailing) != 2)
                    continue;

                trailing.HorizontalAlignment = HorizontalAlignment.Left;
            }
        }
        finally
        {
            _layoutDepth--;
        }

        col0Actual = MeasureColumnActualWidth(table, 0);
        col1Actual = MeasureColumnActualWidth(table, 1);
        TraceUpdateBodyTable(
            tableTag,
            layoutWidth,
            labelWidth,
            trackWidth,
            trailingWidth,
            tableWidth,
            rightInset,
            table.ActualWidth,
            col0Actual,
            col1Actual,
            earlyReturn: false);

        return new BodyTableLayoutMetrics
        {
            ContentWidth = layoutWidth,
            TrackWidth = trackWidth,
            RightInset = rightInset,
            TableWidth = tableWidth,
            LabelWidth = labelWidth,
            TrailingWidth = trailingWidth
        };
    }

    public static void InvalidateLabelSync(Grid table)
    {
        if (LayoutStates.TryGetValue(table, out var state))
        {
            state.SyncedLabelWidth = 0;
            state.LastTableWidth = -1;
            state.LastTrackWidth = -1;
            state.LastRightInset = -1;
        }
    }

    /// <summary>
    /// Фазы inline: пока W >= maxContent + minInset — col1=200, сжимается только правый отступ;
    /// иначе col1 clamp(120..200), отступ = minInset.
    /// </summary>
    public static (double TrackWidth, double RightInset) ComputeBodyTableTrackAndInset(
        double contentWidth,
        double labelWidth,
        double trailingWidth = 0)
    {
        var minInset = BodyTableRightInset;
        var maxContentWidth = labelWidth + GlassSettingsSliderMetrics.TrackMaxWidth + trailingWidth;
        var minContentWidth = labelWidth + GlassSettingsSliderMetrics.TrackMinWidth + trailingWidth;
        var width = Math.Max(0, contentWidth);

        if (width >= maxContentWidth + minInset)
        {
            return (GlassSettingsSliderMetrics.TrackMaxWidth, width - maxContentWidth);
        }

        if (width >= minContentWidth + minInset)
        {
            var trackWidth = GlassSettingsSliderMetrics.ClampTrack(
                width - minInset - labelWidth - trailingWidth);
            return (trackWidth, minInset);
        }

        return (GlassSettingsSliderMetrics.TrackMinWidth, Math.Max(0, width - minContentWidth));
    }

    /// <summary>Ширина трека col1: clamp(available - trailing, TrackMin, TrackMax).</summary>
    public static double ComputeControlTrackWidth(double layoutWidth, double labelWidth, double trailingWidth = 0) =>
        ComputeBodyTableTrackAndInset(layoutWidth, labelWidth, trailingWidth).TrackWidth;

    /// <summary>Единый chrome col1 для любого контрола (suffix-panel, slider, combo, panel…).</summary>
    public static void ApplyControlColumnChrome(FrameworkElement control, double trackWidth)
    {
        control.HorizontalAlignment = HorizontalAlignment.Left;

        if (control is Grid suffixPanel && SettingsSliderFactory.IsSliderValueSuffixPanel(suffixPanel))
        {
            var suffixCol = SettingsSliderFactory.MeasureSuffixColumnWidth(suffixPanel);
            SettingsSliderFactory.ApplyBodySuffixPanelWidth(suffixPanel, trackWidth + suffixCol);
            return;
        }

        if (control is Slider slider)
        {
            SettingsSliderFactory.Apply(slider, slider.Minimum, slider.Maximum, stretch: true);
            slider.MinWidth = trackWidth;
            slider.Width = trackWidth;
            slider.MaxWidth = trackWidth;
            return;
        }

        if (control is Panel panel)
        {
            ApplyPanelControlColumnChrome(panel, trackWidth);
            return;
        }

        ApplyLeafControlColumnChrome(control, trackWidth);
    }

    private static void ApplyPanelControlColumnChrome(Panel panel, double trackWidth)
    {
        panel.HorizontalAlignment = HorizontalAlignment.Left;
        panel.MaxWidth = trackWidth;
        panel.Width = trackWidth;

        var reserved = 0d;
        foreach (var child in panel.Children)
        {
            if (child is not FrameworkElement fe || fe.Visibility != Visibility.Visible)
                continue;
            if (child is Slider)
                continue;

            fe.Measure(new Windows.Foundation.Size(double.PositiveInfinity, double.PositiveInfinity));
            reserved += Math.Ceiling(fe.DesiredSize.Width) + GetHorizontalMargin(fe);
            if (panel is StackPanel { Spacing: > 0 } stack)
                reserved += stack.Spacing;
        }

        var sliderWidth = GlassSettingsSliderMetrics.ClampTrack(trackWidth - reserved);
        foreach (var child in panel.Children)
        {
            if (child is Slider slider)
            {
                SettingsSliderFactory.Apply(slider, slider.Minimum, slider.Maximum, stretch: true);
                slider.Width = sliderWidth;
                slider.MaxWidth = sliderWidth;
            }
            else if (child is FrameworkElement fe)
            {
                fe.HorizontalAlignment = HorizontalAlignment.Left;
            }
        }
    }

    private static void ApplyLeafControlColumnChrome(FrameworkElement control, double trackWidth)
    {
        control.ClearValue(FrameworkElement.MinWidthProperty);
        control.ClearValue(FrameworkElement.WidthProperty);
        control.MaxWidth = trackWidth;
        control.MinWidth = Math.Min(GlassSettingsSliderMetrics.TrackMinWidth, trackWidth);
    }

    private static double GetHorizontalMargin(FrameworkElement fe) =>
        fe.Margin.Left + fe.Margin.Right;

    private static bool HasVisibleBodyContent(Grid? table)
    {
        if (table is null)
            return false;

        foreach (var child in table.Children)
        {
            if (child is FrameworkElement fe && fe.Visibility == Visibility.Visible)
                return true;
        }

        return false;
    }

    private static double MeasureBodyTableMinWidth(Grid table)
    {
        var labelWidth = MeasureWidestLabelWidth(table);
        var controlMin = 0d;
        var trailingMin = MeasureTrailingRailWidth(table);

        foreach (var child in table.Children)
        {
            if (child is not FrameworkElement fe || Grid.GetColumn(fe) != 1)
                continue;
            if (fe.Visibility != Visibility.Visible)
                continue;

            controlMin = Math.Max(controlMin, EstimateControlRailWidth(fe));
        }

        return labelWidth + controlMin + trailingMin;
    }

    private static double MeasureWidestLabelWidth(Grid table)
    {
        var max = 0d;
        var hasLabel = false;
        foreach (var child in table.Children)
        {
            // Колонка 0: TextBlock (picker-body) или составная ячейка AddLabeledRowCell (composition hero).
            if (child is not FrameworkElement label || Grid.GetColumn(label) != 0)
                continue;
            if (label.Visibility != Visibility.Visible)
                continue;

            hasLabel = true;
            label.Measure(new Windows.Foundation.Size(double.PositiveInfinity, double.PositiveInfinity));
            var w = Math.Ceiling(label.DesiredSize.Width) + label.Margin.Left + label.Margin.Right;
            max = Math.Max(max, w);
        }

        if (!hasLabel)
            return 0d;

        return Math.Max(max, LabelColumnMinWidth);
    }

    private static void SyncLabelColumnWidths(Grid table, BodyTableLayoutState state)
    {
        var target = MeasureWidestLabelWidth(table);
        if (Math.Abs(state.SyncedLabelWidth - target) < 0.5)
            return;

        state.SyncedLabelWidth = target;
        var labelWidth = target;
        foreach (var child in table.Children)
        {
            if (child is not TextBlock label || Grid.GetColumn(label) != 0)
                continue;

            var inner = labelWidth - label.Margin.Left - label.Margin.Right;
            if (Math.Abs(label.Width - inner) > 0.5)
                label.Width = inner;
        }

        var tableTag = $"Table#{table.GetHashCode()}";
        SettingsPickerBodyLayoutTrace.WriteIfChanged(
            $"{tableTag}:SyncLabel",
            tableTag,
            $"SyncLabelColumnWidths synced={SettingsPickerBodyLayoutTrace.Format(target)} target={SettingsPickerBodyLayoutTrace.Format(target)}");
    }

    private static double MeasureTrailingRailWidth(Grid table)
    {
        var max = 0d;
        foreach (var child in table.Children)
        {
            if (child is not FrameworkElement trailing || Grid.GetColumn(trailing) != 2)
                continue;
            if (trailing.Visibility != Visibility.Visible)
                continue;

            trailing.Measure(new Windows.Foundation.Size(double.PositiveInfinity, double.PositiveInfinity));
            var width = Math.Ceiling(trailing.DesiredSize.Width) + trailing.Margin.Left + trailing.Margin.Right;
            max = Math.Max(max, width);
        }

        return max;
    }

    private static double EstimateControlRailWidth(FrameworkElement control)
    {
        if (control is Grid suffixPanel && SettingsSliderFactory.IsSliderValueSuffixPanel(suffixPanel))
        {
            var suffixCol = SettingsSliderFactory.MeasureSuffixColumnWidth(suffixPanel);
            return GlassSettingsSliderMetrics.TrackMinWidth + suffixCol;
        }

        return GlassSettingsSliderMetrics.TrackMinWidth;
    }

    private static double MeasureColumnActualWidth(Grid table, int column)
    {
        var max = 0d;
        foreach (var child in table.Children)
        {
            if (child is not FrameworkElement fe || Grid.GetColumn(fe) != column)
                continue;
            if (fe.Visibility != Visibility.Visible)
                continue;

            if (fe.ActualWidth > 0)
                max = Math.Max(max, fe.ActualWidth + fe.Margin.Left + fe.Margin.Right);
        }

        return max;
    }

    private static void TraceUpdateBodyTable(
        string tableTag,
        double layoutWidth,
        double labelWidth,
        double trackWidth,
        double trailingWidth,
        double tableWidth,
        double rightInset,
        double tableActualWidth,
        double col0Actual,
        double col1Actual,
        bool earlyReturn)
    {
        var f = SettingsPickerBodyLayoutTrace.Format;
        SettingsPickerBodyLayoutTrace.WriteIfChanged(
            $"{tableTag}:UpdateBodyTable",
            tableTag,
            $"UpdateBodyTable layoutW={f(layoutWidth)} labelW={f(labelWidth)} trackW={f(trackWidth)} trailW={f(trailingWidth)} tableW={f(tableWidth)} inset={f(rightInset)} tableActual={f(tableActualWidth)} col0={f(col0Actual)} col1={f(col1Actual)} cached={earlyReturn}");
    }

    private static double ReadResource(string key, double fallback)
    {
        if (Application.Current.Resources.TryGetValue(key, out var resource) && resource is double value)
            return value;

        return fallback;
    }
}
