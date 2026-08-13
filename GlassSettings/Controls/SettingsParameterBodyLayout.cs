using System.Runtime.CompilerServices;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.Foundation;

namespace GlassSettings.Controls;

/// <summary>4-col body-таблица SettingsParameterPanel: col0/col1/col2/col3 + adaptive inset/wrap.</summary>
public static class SettingsParameterBodyLayout
{
    public const string BodyTableTag = "GlassSettingsParameterBodyTable";

    private const string DualRowPrimaryTag = "GlassSettingsParameterDualPrimary";

    private const double DefaultInnerPairGap = 12;
    private const double WrapUnstackThresholdPx = 1;

    public static double InfoColumnHorizontalPadding => 44;

    private static readonly ConditionalWeakTable<Grid, ParameterBodyLayoutState> LayoutStates = new();
    private static int _layoutDepth;

    private sealed class LogicalRow
    {
        public required int PrimaryRowIndex { get; init; }
        public required int WrapRowIndex { get; init; }
        public required bool HasSecondary { get; init; }
        public TextBlock? PrimaryLabel { get; init; }
        public required FrameworkElement PrimaryControl { get; init; }
        public TextBlock? SecondaryLabel { get; init; }
        public FrameworkElement? SecondaryControl { get; init; }
        public double InlineMinWidth = -1;
        public bool WrapActive;
        public bool WrapLayoutInitialized;
    }

    private sealed class ParameterBodyLayoutState
    {
        public readonly List<LogicalRow> Rows = new();
        public double SyncedPrimaryLabelWidth;
        public double SyncedSecondaryLabelWidth;
        public double CachedMinInlineWidth = -1;
        public bool RowWrapActive;
        public bool WrapLayoutInitialized;
        public bool LastRowWrapActive;
        public double LastLayoutWidth = -1;
        public double LastTableWidth = -1;
        public double LastRightInset = -1;
    }

    public readonly struct ParameterBodyLayoutMetrics
    {
        public double ContentWidth { get; init; }
        public double TableWidth { get; init; }
        public double RightInset { get; init; }
        public bool RowWrapActive { get; init; }
    }

    public static bool IsBodyTable(Grid? grid) =>
        grid is not null && Equals(grid.Tag, BodyTableTag);

    public static double LabelControlGap =>
        ReadResource("GlassSettingsPickerBodyLabelControlGap", 16);

    public static double BodyTableRightInset =>
        ReadResource("GlassSettingsPickerBodyTableRightInset", 20);

    public static double InnerPairGap =>
        ReadResource("GlassSettingsParameterBodyInnerPairGap", DefaultInnerPairGap);

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
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        GetOrCreateState(grid);
        return grid;
    }

    public static Grid BuildLabeledRowsGrid(
        (TextBlock Label, FrameworkElement Control)[] rows,
        Style? labelStyle)
    {
        var table = CreateBodyTable();
        for (var i = 0; i < rows.Length; i++)
        {
            var (label, control) = rows[i];
            if (labelStyle is not null)
                label.Style = labelStyle;
            AddSingleRow(table, label, control, topMargin: i > 0);
        }

        return table;
    }

    public static void AddSingleRow(
        Grid table,
        TextBlock label,
        FrameworkElement control,
        bool topMargin = false)
    {
        EnsureState(table);
        var rowIndex = table.RowDefinitions.Count;
        table.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

        ApplyPrimaryLabelChrome(label, topMargin);
        ApplyPrimaryControlChrome(control, topMargin);

        Grid.SetRow(label, rowIndex);
        Grid.SetColumn(label, 0);
        Grid.SetRow(control, rowIndex);
        Grid.SetColumn(control, 1);
        table.Children.Add(label);
        table.Children.Add(control);

        GetOrCreateState(table).Rows.Add(new LogicalRow
        {
            PrimaryRowIndex = rowIndex,
            WrapRowIndex = rowIndex,
            HasSecondary = false,
            PrimaryLabel = label,
            PrimaryControl = control
        });
        InvalidateLayoutCache(table);
    }

    public static void AddDualRow(
        Grid table,
        TextBlock primaryLabel,
        FrameworkElement primaryControl,
        TextBlock? secondaryLabel,
        FrameworkElement secondaryControl,
        bool topMargin = false)
    {
        EnsureState(table);
        var primaryRowIndex = table.RowDefinitions.Count;
        table.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        var wrapRowIndex = table.RowDefinitions.Count;
        table.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

        ApplyPrimaryLabelChrome(primaryLabel, topMargin);
        ApplyPrimaryControlChrome(primaryControl, topMargin);
        primaryControl.Tag = DualRowPrimaryTag;
        if (secondaryLabel is not null)
            ApplySecondaryLabelChrome(secondaryLabel, topMargin: false);
        ApplySecondaryControlChrome(secondaryControl, topMargin: false);

        Grid.SetRow(primaryLabel, primaryRowIndex);
        Grid.SetColumn(primaryLabel, 0);
        Grid.SetRow(primaryControl, primaryRowIndex);
        Grid.SetColumn(primaryControl, 1);

        Grid.SetRow(secondaryLabel ?? secondaryControl, primaryRowIndex);
        Grid.SetColumn(secondaryLabel ?? secondaryControl, secondaryLabel is not null ? 2 : 3);
        if (secondaryLabel is not null)
        {
            Grid.SetRow(secondaryControl, primaryRowIndex);
            Grid.SetColumn(secondaryControl, 3);
            table.Children.Add(secondaryLabel);
        }

        table.Children.Add(primaryLabel);
        table.Children.Add(primaryControl);
        table.Children.Add(secondaryControl);

        GetOrCreateState(table).Rows.Add(new LogicalRow
        {
            PrimaryRowIndex = primaryRowIndex,
            WrapRowIndex = wrapRowIndex,
            HasSecondary = true,
            PrimaryLabel = primaryLabel,
            PrimaryControl = primaryControl,
            SecondaryLabel = secondaryLabel,
            SecondaryControl = secondaryControl
        });
        InvalidateLayoutCache(table);
    }

    public static ParameterBodyLayoutMetrics UpdateParameterBodyLayout(Grid table, double layoutWidth)
    {
        if (!IsBodyTable(table) || _layoutDepth > 0 || layoutWidth <= 0)
            return default;

        if (!LayoutStates.TryGetValue(table, out var state))
        {
            state = GetOrCreateState(table);
        }

        EnsureLogicalRows(table, state);
        if (state.Rows.Count == 0)
            return default;

        EnsureCachedMinInlineWidth(table, state);
        var wrapChanged = SyncRowWrapStates(layoutWidth, state);
        var visualMismatch = HasVisualWrapMismatch(state);
        var wrap = AnyRowWrapActive(state);

        if (wrapChanged || !state.WrapLayoutInitialized || visualMismatch)
        {
            state.WrapLayoutInitialized = true;
            state.RowWrapActive = wrap;
            ApplyRowWrapVisuals(table, state);
        }

        SyncLabelColumnWidths(table, state);

        var label0 = state.SyncedPrimaryLabelWidth;
        var label2 = state.SyncedSecondaryLabelWidth;
        var (tableWidth, rightInset) = ComputeTableWidthAndInset(
            table,
            state,
            layoutWidth,
            label0,
            label2);

        if (Math.Abs(state.LastLayoutWidth - layoutWidth) < 0.5
            && Math.Abs(state.LastTableWidth - tableWidth) < 0.5
            && Math.Abs(state.LastRightInset - rightInset) < 0.5
            && state.LastRowWrapActive == wrap
            && table.Width > 0)
        {
            TraceUpdate(table, layoutWidth, tableWidth, rightInset, wrap, label0, label2, cached: true);
            return new ParameterBodyLayoutMetrics
            {
                ContentWidth = layoutWidth,
                TableWidth = tableWidth,
                RightInset = rightInset,
                RowWrapActive = wrap
            };
        }

        try
        {
            _layoutDepth++;
            state.LastLayoutWidth = layoutWidth;
            state.LastTableWidth = tableWidth;
            state.LastRightInset = rightInset;
            state.LastRowWrapActive = wrap;

            table.HorizontalAlignment = HorizontalAlignment.Left;
            table.Margin = new Thickness(table.Margin.Left, table.Margin.Top, 0, table.Margin.Bottom);
            if (tableWidth > 0)
            {
                table.Width = tableWidth;
                table.MaxWidth = tableWidth;
            }
            else
            {
                table.ClearValue(FrameworkElement.WidthProperty);
                table.ClearValue(FrameworkElement.MaxWidthProperty);
            }

            ApplyControlSizing(table, state, label0, label2, layoutWidth, rightInset);
        }
        finally
        {
            _layoutDepth--;
        }

        TraceUpdate(table, layoutWidth, tableWidth, rightInset, wrap, label0, label2, cached: false);

        return new ParameterBodyLayoutMetrics
        {
            ContentWidth = layoutWidth,
            TableWidth = tableWidth,
            RightInset = rightInset,
            RowWrapActive = wrap
        };
    }

    private static void EnsureState(Grid table) => GetOrCreateState(table);

    /// <summary>Сброс cachedMin/wrap после expand или смены BodyContent.</summary>
    public static void InvalidateParameterBodyLayoutCache(Grid table)
    {
        InvalidateLayoutCache(table);
        table.ClearValue(FrameworkElement.WidthProperty);
        table.ClearValue(FrameworkElement.MaxWidthProperty);
    }

    private static ParameterBodyLayoutState GetOrCreateState(Grid table)
    {
        if (LayoutStates.TryGetValue(table, out var state))
            return state;

        state = new ParameterBodyLayoutState();
        LayoutStates.Add(table, state);
        return state;
    }

    private static void EnsureLogicalRows(Grid table, ParameterBodyLayoutState state)
    {
        if (state.Rows.Count > 0)
            return;

        var rowCount = table.RowDefinitions.Count;
        var consumedRows = new HashSet<int>();

        foreach (var child in table.Children)
        {
            if (child is not FrameworkElement fe || !Equals(fe.Tag, DualRowPrimaryTag))
                continue;

            var primaryRowIndex = Grid.GetRow(fe);
            if (consumedRows.Contains(primaryRowIndex))
                continue;

            if (!TryParseRowCells(table, primaryRowIndex, out var c0, out var c1, out var c2, out var c3)
                || c1 is null)
                continue;

            var wrapRowIndex = primaryRowIndex + 1 < rowCount ? primaryRowIndex + 1 : primaryRowIndex;
            var secondaryLabel = c2 as TextBlock;
            var secondaryControl = c3 ?? (c2 as FrameworkElement);

            if (secondaryControl is null && wrapRowIndex != primaryRowIndex)
            {
                TryParseRowCells(table, wrapRowIndex, out var w0, out var w1, out _, out _);
                secondaryLabel = w0 as TextBlock;
                secondaryControl = w1 ?? w0;
            }

            if (secondaryControl is null)
                continue;

            state.Rows.Add(new LogicalRow
            {
                PrimaryRowIndex = primaryRowIndex,
                WrapRowIndex = wrapRowIndex,
                HasSecondary = true,
                PrimaryLabel = c0 as TextBlock,
                PrimaryControl = c1,
                SecondaryLabel = secondaryLabel,
                SecondaryControl = secondaryControl
            });
            consumedRows.Add(primaryRowIndex);
            if (Grid.GetRow(secondaryControl) != primaryRowIndex)
                consumedRows.Add(wrapRowIndex);
        }

        for (var rowIndex = 0; rowIndex < rowCount; rowIndex++)
        {
            if (consumedRows.Contains(rowIndex))
                continue;

            if (!TryParseRowCells(table, rowIndex, out var c0, out var c1, out var c2, out var c3)
                || c1 is null
                || c2 is not null
                || c3 is not null)
                continue;

            state.Rows.Add(new LogicalRow
            {
                PrimaryRowIndex = rowIndex,
                WrapRowIndex = rowIndex,
                HasSecondary = false,
                PrimaryLabel = c0 as TextBlock,
                PrimaryControl = c1
            });
            consumedRows.Add(rowIndex);
        }

        state.Rows.Sort(static (a, b) => a.PrimaryRowIndex.CompareTo(b.PrimaryRowIndex));
    }

    private static bool TryParseRowCells(
        Grid table,
        int rowIndex,
        out TextBlock? col0,
        out FrameworkElement? col1,
        out TextBlock? col2,
        out FrameworkElement? col3)
    {
        col0 = null;
        col1 = null;
        col2 = null;
        col3 = null;

        foreach (var child in table.Children)
        {
            if (child is not FrameworkElement fe || Grid.GetRow(fe) != rowIndex)
                continue;

            switch (Grid.GetColumn(fe))
            {
                case 0 when child is TextBlock label:
                    col0 = label;
                    break;
                case 1:
                    col1 = fe;
                    break;
                case 2 when child is TextBlock label2:
                    col2 = label2;
                    break;
                case 3:
                    col3 = fe;
                    break;
            }
        }

        return col0 is not null || col1 is not null || col2 is not null || col3 is not null;
    }

    private static void InvalidateLayoutCache(Grid table)
    {
        if (LayoutStates.TryGetValue(table, out var state))
        {
            state.CachedMinInlineWidth = -1;
            foreach (var row in state.Rows)
            {
                row.InlineMinWidth = -1;
                row.WrapActive = false;
                row.WrapLayoutInitialized = false;
            }

            state.WrapLayoutInitialized = false;
            state.RowWrapActive = false;
            state.LastRowWrapActive = false;
            state.LastLayoutWidth = -1;
            state.LastTableWidth = -1;
            state.LastRightInset = -1;
        }
    }

    private static bool ResolveWrapForThreshold(
        double layoutWidth,
        double threshold,
        bool wrapLayoutInitialized,
        bool wrapActive)
    {
        if (threshold <= 0)
            return false;

        if (wrapLayoutInitialized && wrapActive)
            return layoutWidth < threshold + WrapUnstackThresholdPx;

        return layoutWidth < threshold;
    }

    private static bool SyncRowWrapStates(double layoutWidth, ParameterBodyLayoutState state)
    {
        var changed = false;
        foreach (var row in state.Rows)
        {
            if (!row.HasSecondary)
                continue;

            var threshold = row.InlineMinWidth > 0 ? row.InlineMinWidth : state.CachedMinInlineWidth;
            var wrap = ResolveWrapForThreshold(layoutWidth, threshold, row.WrapLayoutInitialized, row.WrapActive);
            if (wrap != row.WrapActive || !row.WrapLayoutInitialized)
                changed = true;

            row.WrapActive = wrap;
            row.WrapLayoutInitialized = true;
        }

        return changed;
    }

    private static bool AnyRowWrapActive(ParameterBodyLayoutState state)
    {
        foreach (var row in state.Rows)
        {
            if (row.HasSecondary && row.WrapActive)
                return true;
        }

        return false;
    }

    private static bool HasVisualWrapMismatch(ParameterBodyLayoutState state)
    {
        foreach (var row in state.Rows)
        {
            if (!row.HasSecondary || row.SecondaryControl is null)
                continue;

            var visualWrapped = Grid.GetRow(row.SecondaryControl) != row.PrimaryRowIndex;
            if (visualWrapped != row.WrapActive)
                return true;
        }

        return false;
    }

    private static void EnsureCachedMinInlineWidth(Grid table, ParameterBodyLayoutState state)
    {
        if (state.CachedMinInlineWidth > 0)
            return;

        var restoreWrap = AnyRowWrapActive(state);
        if (restoreWrap)
        {
            foreach (var row in state.Rows)
            {
                if (row.HasSecondary)
                    ApplyDualRowWrapVisuals(row, wrap: false);
            }
        }

        try
        {
            var label0 = MeasureWidestPrimaryLabelWidth(table, state);
            var label2 = MeasureWidestSecondaryLabelWidth(table, state);
            var maxRow = 0d;

            foreach (var row in state.Rows)
            {
                if (!row.HasSecondary)
                    continue;

                var rowWidth = MeasureRowInlineMinWidth(row, label0, label2);
                row.InlineMinWidth = rowWidth;
                maxRow = Math.Max(maxRow, rowWidth);
            }

            state.CachedMinInlineWidth = maxRow;
            state.SyncedPrimaryLabelWidth = label0;
            state.SyncedSecondaryLabelWidth = label2;
        }
        finally
        {
            if (restoreWrap)
                ApplyRowWrapVisuals(table, state);
        }
    }

    private static double MeasureRowInlineMinWidth(LogicalRow row, double label0, double label2)
    {
        var rowWidth = label0 + LabelControlGap + MeasureControlMinWidth(row.PrimaryControl);
        if (row.HasSecondary && row.SecondaryControl is not null)
        {
            rowWidth += InnerPairGap + label2 + LabelControlGap + MeasureControlMinWidth(row.SecondaryControl);
        }

        return rowWidth;
    }

    private static (double TableWidth, double RightInset) ComputeTableWidthAndInset(
        Grid table,
        ParameterBodyLayoutState state,
        double layoutWidth,
        double label0,
        double label2)
    {
        var minInset = BodyTableRightInset;
        var maxRowContent = 0d;
        var minRowContent = 0d;

        foreach (var row in state.Rows)
        {
            var rowWrap = row.HasSecondary && row.WrapActive;
            var (rowMax, rowMin) = MeasureRowContentWidths(row, label0, label2, rowWrap, useTrackMax: true);
            maxRowContent = Math.Max(maxRowContent, rowMax);
            minRowContent = Math.Max(minRowContent, rowMin);
        }

        if (maxRowContent <= 0)
            return (0, 0);

        if (layoutWidth >= maxRowContent + minInset)
        {
            var tableWidth = maxRowContent;
            var rightInset = layoutWidth - maxRowContent;
            return ClampTableWidthToBudget(tableWidth, rightInset, layoutWidth, minInset);
        }

        if (layoutWidth >= minRowContent + minInset)
        {
            var tableWidth = layoutWidth - minInset;
            return ClampTableWidthToBudget(tableWidth, minInset, layoutWidth, minInset);
        }

        var minTableWidth = minRowContent;
        var minRightInset = Math.Max(0, layoutWidth - minRowContent);
        return ClampTableWidthToBudget(minTableWidth, minRightInset, layoutWidth, minInset);
    }

    private static (double TableWidth, double RightInset) ClampTableWidthToBudget(
        double tableWidth,
        double rightInset,
        double layoutWidth,
        double minInset)
    {
        var maxTable = Math.Max(0, layoutWidth - minInset);
        if (tableWidth <= maxTable)
            return (tableWidth, rightInset);

        tableWidth = maxTable;
        rightInset = Math.Max(minInset, layoutWidth - tableWidth);
        return (tableWidth, rightInset);
    }

    private static (double MaxContent, double MinContent) MeasureRowContentWidths(
        LogicalRow row,
        double label0,
        double label2,
        bool wrap,
        bool useTrackMax)
    {
        var c1 = useTrackMax
            ? MeasureControlMaxWidth(row.PrimaryControl)
            : MeasureControlMinWidth(row.PrimaryControl);
        var c1Min = MeasureControlMinWidth(row.PrimaryControl);

        if (!row.HasSecondary || row.SecondaryControl is null)
        {
            var single = label0 + LabelControlGap + c1;
            var singleMin = label0 + LabelControlGap + c1Min;
            return (single, singleMin);
        }

        var c3 = useTrackMax
            ? MeasureControlMaxWidth(row.SecondaryControl)
            : MeasureControlMinWidth(row.SecondaryControl);
        var c3Min = MeasureControlMinWidth(row.SecondaryControl);

        if (wrap)
        {
            var primaryMax = label0 + LabelControlGap + c1;
            var primaryMin = label0 + LabelControlGap + c1Min;
            var wrapMax = label2 + LabelControlGap + c3;
            var wrapMin = label2 + LabelControlGap + c3Min;
            return (Math.Max(primaryMax, wrapMax), Math.Max(primaryMin, wrapMin));
        }

        var inlineMax = label0 + LabelControlGap + c1 + InnerPairGap + label2 + LabelControlGap + c3;
        var inlineMin = label0 + LabelControlGap + c1Min + InnerPairGap + label2 + LabelControlGap + c3Min;
        return (inlineMax, inlineMin);
    }

    private static void ApplyControlSizing(
        Grid table,
        ParameterBodyLayoutState state,
        double label0,
        double label2,
        double layoutWidth,
        double rightInset)
    {
        var contentBudget = Math.Max(0, layoutWidth - rightInset);

        foreach (var row in state.Rows)
        {
            if (row.WrapActive && row.HasSecondary)
            {
                if (row.PrimaryControl is not null)
                    ApplyPrimaryControlColumnWidth(row.PrimaryControl, contentBudget - label0 - LabelControlGap);
                if (row.SecondaryControl is not null)
                    ApplyPrimaryControlColumnWidth(row.SecondaryControl, contentBudget - label2 - LabelControlGap);
                continue;
            }

            if (!row.HasSecondary || row.SecondaryControl is null)
            {
                if (row.PrimaryControl is not null)
                    ApplyPrimaryControlColumnWidth(row.PrimaryControl, contentBudget - label0 - LabelControlGap);
                continue;
            }

            var reserved = label0 + LabelControlGap + InnerPairGap + label2 + LabelControlGap;
            var secondaryWidth = MeasureControlMinWidth(row.SecondaryControl);
            if (row.SecondaryControl is Slider)
            {
                var (_, track) = SettingsPickerBodyLayout.ComputeBodyTableTrackAndInset(
                    contentBudget - reserved,
                    0,
                    secondaryWidth);
                secondaryWidth = track;
                SettingsPickerBodyLayout.ApplyControlColumnChrome(row.SecondaryControl, track);
            }

            var primaryBudget = Math.Max(GlassSettingsSliderMetrics.TrackMinWidth, contentBudget - reserved - secondaryWidth);
            if (row.PrimaryControl is not null)
                ApplyPrimaryControlColumnWidth(row.PrimaryControl, primaryBudget);
        }
    }

    private static void ApplyPrimaryControlColumnWidth(FrameworkElement control, double trackWidth)
    {
        trackWidth = Math.Max(0, trackWidth);
        if (control is Grid suffixPanel && SettingsSliderFactory.IsSliderValueSuffixPanel(suffixPanel))
        {
            var suffixCol = SettingsSliderFactory.MeasureSuffixColumnWidth(suffixPanel);
            var track = GlassSettingsSliderMetrics.ClampTrack(Math.Max(0, trackWidth - suffixCol));
            SettingsPickerBodyLayout.ApplyControlColumnChrome(suffixPanel, track);
            return;
        }

        if (control is Slider)
            ApplyControlColumnWidth(control, trackWidth, isSliderTrack: true);
    }

    private static void ApplyControlColumnWidth(FrameworkElement control, double trackWidth, bool isSliderTrack)
    {
        trackWidth = Math.Max(0, trackWidth);
        if (!isSliderTrack)
            return;

        var clamped = GlassSettingsSliderMetrics.ClampTrack(trackWidth);
        SettingsPickerBodyLayout.ApplyControlColumnChrome(control, clamped);
    }

    private static void ApplyRowWrapVisuals(Grid table, ParameterBodyLayoutState state)
    {
        foreach (var row in state.Rows)
        {
            if (!row.HasSecondary || row.SecondaryControl is null)
                continue;

            ApplyDualRowWrapVisuals(row, row.WrapActive);
        }
    }

    private static void ApplyDualRowWrapVisuals(LogicalRow row, bool wrap)
    {
        if (row.SecondaryControl is not { } secondaryControl)
            return;

        if (wrap)
        {
            if (row.SecondaryLabel is not null)
            {
                Grid.SetRow(row.SecondaryLabel, row.WrapRowIndex);
                Grid.SetColumn(row.SecondaryLabel, 0);
                row.SecondaryLabel.Visibility = Visibility.Visible;
                row.SecondaryLabel.Margin = new Thickness(0, 12, LabelControlGap, 0);
            }

            Grid.SetRow(secondaryControl, row.WrapRowIndex);
            Grid.SetColumn(secondaryControl, 1);
            secondaryControl.Visibility = Visibility.Visible;
            if (secondaryControl.Margin.Top < 12)
                secondaryControl.Margin = new Thickness(secondaryControl.Margin.Left, 12, secondaryControl.Margin.Right, secondaryControl.Margin.Bottom);
            return;
        }

        var topMargin = row.PrimaryRowIndex > 0;
        if (row.SecondaryLabel is not null)
        {
            Grid.SetRow(row.SecondaryLabel, row.PrimaryRowIndex);
            Grid.SetColumn(row.SecondaryLabel, 2);
            row.SecondaryLabel.Visibility = Visibility.Visible;
            ApplySecondaryLabelChrome(row.SecondaryLabel, topMargin);
        }

        Grid.SetRow(secondaryControl, row.PrimaryRowIndex);
        Grid.SetColumn(secondaryControl, 3);
        secondaryControl.Visibility = Visibility.Visible;
        ApplySecondaryControlChrome(secondaryControl, topMargin);
    }

    private static void SyncLabelColumnWidths(Grid table, ParameterBodyLayoutState state)
    {
        var target0 = MeasureWidestPrimaryLabelWidth(table, state);
        if (Math.Abs(state.SyncedPrimaryLabelWidth - target0) >= 0.5)
            state.SyncedPrimaryLabelWidth = target0;

        var target2 = MeasureWidestSecondaryLabelWidth(table, state);
        if (Math.Abs(state.SyncedSecondaryLabelWidth - target2) >= 0.5)
            state.SyncedSecondaryLabelWidth = target2;

        foreach (var row in state.Rows)
        {
            if (row.PrimaryLabel is not null)
                ApplySyncedLabelWidth(row.PrimaryLabel, state.SyncedPrimaryLabelWidth);

            if (row.SecondaryLabel is not null)
                ApplySyncedLabelWidth(row.SecondaryLabel, state.SyncedSecondaryLabelWidth);
        }
    }

    private static void ApplySyncedLabelWidth(TextBlock label, double syncedWidth)
    {
        var inner = syncedWidth - label.Margin.Left - label.Margin.Right;
        if (Math.Abs(label.Width - inner) > 0.5)
            label.Width = inner;
    }

    private static double MeasureWidestPrimaryLabelWidth(Grid table, ParameterBodyLayoutState state)
    {
        var max = 0d;
        var hasLabel = false;
        foreach (var row in state.Rows)
        {
            if (row.PrimaryLabel is null || row.PrimaryLabel.Visibility != Visibility.Visible)
                continue;

            hasLabel = true;
            row.PrimaryLabel.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
            var w = Math.Ceiling(row.PrimaryLabel.DesiredSize.Width)
                    + row.PrimaryLabel.Margin.Left
                    + row.PrimaryLabel.Margin.Right;
            max = Math.Max(max, w);
        }

        if (!hasLabel)
            return 0d;

        return Math.Max(max, ReadResource("GlassSettingsSectionTitleMinWidth", 150));
    }

    private static double MeasureWidestSecondaryLabelWidth(Grid table, ParameterBodyLayoutState state)
    {
        var max = 0d;
        var hasLabel = false;
        foreach (var row in state.Rows)
        {
            if (row.SecondaryLabel is null || row.SecondaryLabel.Visibility != Visibility.Visible)
                continue;

            hasLabel = true;
            row.SecondaryLabel.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
            var w = Math.Ceiling(row.SecondaryLabel.DesiredSize.Width)
                    + row.SecondaryLabel.Margin.Left
                    + row.SecondaryLabel.Margin.Right;
            max = Math.Max(max, w);
        }

        return hasLabel ? max : 0d;
    }

    private static double MeasureControlMinWidth(FrameworkElement control)
    {
        if (control is Grid suffixPanel && SettingsSliderFactory.IsSliderValueSuffixPanel(suffixPanel))
        {
            var suffixCol = SettingsSliderFactory.MeasureSuffixColumnWidth(suffixPanel);
            return GlassSettingsSliderMetrics.TrackMinWidth + suffixCol;
        }

        if (control is Slider)
            return GlassSettingsSliderMetrics.TrackMinWidth;

        control.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
        var measured = Math.Ceiling(control.DesiredSize.Width);
        if (control.MinWidth > 0)
            measured = Math.Max(measured, control.MinWidth);

        return measured;
    }

    private static double MeasureControlMaxWidth(FrameworkElement control)
    {
        if (control is Grid suffixPanel && SettingsSliderFactory.IsSliderValueSuffixPanel(suffixPanel))
        {
            var suffixCol = SettingsSliderFactory.MeasureSuffixColumnWidth(suffixPanel);
            return GlassSettingsSliderMetrics.TrackMaxWidth + suffixCol;
        }

        if (control is Slider)
            return GlassSettingsSliderMetrics.TrackMaxWidth;

        return MeasureControlMinWidth(control);
    }

    private static void ApplyPrimaryLabelChrome(TextBlock label, bool topMargin)
    {
        label.VerticalAlignment = VerticalAlignment.Center;
        label.Margin = topMargin
            ? new Thickness(0, 12, LabelControlGap, 0)
            : new Thickness(0, 0, LabelControlGap, 0);
    }

    private static void ApplyPrimaryControlChrome(FrameworkElement control, bool topMargin)
    {
        control.HorizontalAlignment = HorizontalAlignment.Left;
        control.VerticalAlignment = VerticalAlignment.Center;
        if (topMargin)
            control.Margin = new Thickness(0, 12, 0, 0);
    }

    private static void ApplySecondaryLabelChrome(TextBlock label, bool topMargin)
    {
        label.VerticalAlignment = VerticalAlignment.Center;
        label.Margin = topMargin
            ? new Thickness(InnerPairGap, 12, LabelControlGap, 0)
            : new Thickness(InnerPairGap, 0, LabelControlGap, 0);
    }

    private static void ApplySecondaryControlChrome(FrameworkElement control, bool topMargin)
    {
        control.HorizontalAlignment = HorizontalAlignment.Left;
        control.VerticalAlignment = VerticalAlignment.Center;
        if (topMargin)
            control.Margin = new Thickness(0, 12, 0, 0);
    }

    private static void TraceUpdate(
        Grid table,
        double layoutWidth,
        double tableWidth,
        double rightInset,
        bool wrap,
        double label0,
        double label2,
        bool cached)
    {
        var f = SettingsParameterBodyLayoutTrace.Format;
        SettingsParameterBodyLayoutTrace.WriteIfChanged(
            $"Table#{table.GetHashCode()}:Update",
            $"Table#{table.GetHashCode()}",
            $"UpdateParameterBody layoutW={f(layoutWidth)} tableW={f(tableWidth)} inset={f(rightInset)} wrap={wrap} label0={f(label0)} label2={f(label2)} cached={cached}");
    }

    private static double ReadResource(string key, double fallback)
    {
        if (Application.Current.Resources.TryGetValue(key, out var resource) && resource is double value)
            return value;

        return fallback;
    }
}
