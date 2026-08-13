using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Controls;

/// <summary>Параметры единого layout-движка шапки спойлера.</summary>
public sealed class SpoilerHeaderLayoutOptions
{
    /// <summary>Доп. ширина trailing (navigate chevron rail и т.п.).</summary>
    public double TrailingExtra { get; init; }

    /// <summary>Учитывать gate rail в fixed rails (false — static host измеряется напрямую).</summary>
    public bool HasGateRail { get; init; } = true;
}

/// <summary>Хосты для <see cref="SpoilerHeaderLayoutEngine.UpdateLayout"/>.</summary>
public sealed class SpoilerHeaderLayoutContext
{
    public required SpoilerHeaderGrid Chrome { get; init; }
    public required ContentControl BelowHost { get; init; }
}

/// <summary>
/// Единый measure-based layout шапки: title 150…max → controls table → stack на вторую строку.
/// Unstack: inlineMin + 1 px.
/// </summary>
public static class SpoilerHeaderLayoutEngine
{
    private const double UnstackThresholdPx = 1;

    public static bool UpdateLayout(
        double availableWidth,
        ref bool wasStacked,
        ref bool layoutInitialized,
        SpoilerHeaderLayoutContext context,
        SpoilerHeaderLayoutOptions options)
    {
        var table = context.Chrome.HeaderControlsTable;
        if (table.IsEmpty)
        {
            context.BelowHost.Content = null;
            context.BelowHost.Visibility = Visibility.Collapsed;
            context.Chrome.ApplyTitleRailWidth(null);
            ClearInlineTableWidth(table);
            wasStacked = false;
            layoutInitialized = false;
            return false;
        }

        if (availableWidth <= 0)
            return false;

        var inlineMin = MeasureInlineMinWidth(context.Chrome, table, options);
        var stacked = ResolveStackState(availableWidth, wasStacked, layoutInitialized, inlineMin);

        if (stacked)
            ApplyStackedLayout(availableWidth, context, table);
        else
            ApplyInlineLayout(availableWidth, context, table, options);

        var changed = stacked != wasStacked;
        wasStacked = stacked;
        layoutInitialized = true;
        return changed;
    }

    public static double MeasureInlineMinWidth(
        SpoilerHeaderGrid chrome,
        SpoilerHeaderControlsTable table,
        SpoilerHeaderLayoutOptions options) =>
        MeasureFixedRails(chrome, options)
        + SpoilerHeaderLayoutMetrics.TitleMinWidth
        + (table.IsEmpty ? 0 : table.MeasureMinWidth() + SpoilerHeaderLayoutMetrics.TitleControlsGap);

    private static bool ResolveStackState(
        double available,
        bool wasStacked,
        bool layoutInitialized,
        double inlineMin)
    {
        if (layoutInitialized && wasStacked)
            return available < inlineMin + UnstackThresholdPx;

        return available < inlineMin;
    }

    private static void ApplyInlineLayout(
        double availableWidth,
        SpoilerHeaderLayoutContext context,
        SpoilerHeaderControlsTable table,
        SpoilerHeaderLayoutOptions options)
    {
        context.BelowHost.Content = null;
        context.BelowHost.Visibility = Visibility.Collapsed;

        SpoilerHeaderLayoutMetrics.DetachFromVisualParent(table);
        context.Chrome.AttachControlsTable(table);

        var fixedRails = MeasureFixedRails(context.Chrome, options);
        var controlsMin = table.MeasureMinWidth();
        var controlsMax = table.MeasureMaxWidth();
        var titleMax = context.Chrome.MeasureTitleRailWidth();

        var (titleRail, controlsBudget) = ComputeInlineLayout(
            availableWidth,
            fixedRails,
            controlsMin,
            controlsMax,
            titleMax);
        context.Chrome.ApplyTitleRailWidth(titleRail);

        table.ApplyInlineWidths(controlsBudget);
        table.MaxWidth = controlsBudget;
        table.ClearValue(FrameworkElement.WidthProperty);
        table.HorizontalAlignment = HorizontalAlignment.Left;
    }

    private static void ClearInlineTableWidth(SpoilerHeaderControlsTable table)
    {
        table.ClearValue(FrameworkElement.MaxWidthProperty);
        table.ClearValue(FrameworkElement.WidthProperty);
    }

    private static void ApplyStackedLayout(
        double availableWidth,
        SpoilerHeaderLayoutContext context,
        SpoilerHeaderControlsTable table)
    {
        context.Chrome.ApplyTitleRailWidth(null);
        context.Chrome.DetachControlsTable();

        ClearInlineTableWidth(table);
        SpoilerHeaderLayoutMetrics.DetachFromVisualParent(table);
        table.ApplyStackedChrome(availableWidth);
        table.HorizontalAlignment = HorizontalAlignment.Right;

        context.BelowHost.Content = table;
        context.BelowHost.Visibility = Visibility.Visible;
    }

    /// <summary>
    /// Фаза 1 (сужение): controls max→min при title@max — слайдер сжимается первым.
    /// Фаза 2: controls@min, title max→min — заголовок сжимается последним.
    /// </summary>
    private static (double TitleRail, double ControlsBudget) ComputeInlineLayout(
        double availableWidth,
        double fixedRails,
        double controlsMin,
        double controlsMax,
        double titleMax)
    {
        var gap = SpoilerHeaderLayoutMetrics.TitleControlsGap;
        var titleMin = SpoilerHeaderLayoutMetrics.TitleMinWidth;
        titleMax = Math.Max(titleMin, titleMax);

        var needMaxMax = fixedRails + titleMax + gap + controlsMax;
        if (availableWidth >= needMaxMax)
            return (titleMax, controlsMax);

        var needMaxMin = fixedRails + titleMax + gap + controlsMin;
        if (availableWidth >= needMaxMin)
        {
            var controlsBudget = availableWidth - fixedRails - titleMax - gap;
            return (titleMax, Math.Clamp(controlsBudget, controlsMin, controlsMax));
        }

        var needMinMin = fixedRails + titleMin + gap + controlsMin;
        if (availableWidth >= needMinMin)
        {
            var titleBudget = availableWidth - fixedRails - gap - controlsMin;
            return (Math.Clamp(titleBudget, titleMin, titleMax), controlsMin);
        }

        return (titleMin, controlsMin);
    }

    private static double MeasureFixedRails(SpoilerHeaderGrid chrome, SpoilerHeaderLayoutOptions options)
    {
        var trailingWidth = chrome.MeasureTrailingRailWidth();
        if (chrome.StaticHostPresenter.Content is not null)
            trailingWidth = Math.Max(trailingWidth, SpoilerHeaderLayoutMetrics.TrailingToggleRail);

        var rails = SpoilerHeaderLayoutMetrics.HeaderPadding
                    + SpoilerHeaderLayoutMetrics.IconRail
                    + SpoilerHeaderLayoutMetrics.ColumnGap;

        if (trailingWidth > 0)
            rails += trailingWidth;
        else if (options.HasGateRail)
            rails += SpoilerHeaderLayoutMetrics.TrailingToggleRail + SpoilerHeaderLayoutMetrics.ColumnGap;

        return rails + options.TrailingExtra;
    }
}
