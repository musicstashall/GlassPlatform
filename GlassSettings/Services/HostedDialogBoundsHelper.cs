namespace GlassSettings.Services;

/// <summary>Эффективные границы hosted-диалога с учётом размера контейнера bounds.</summary>
public readonly struct HostedDialogEffectiveBounds
{
    public double EffectiveMinWidth { get; init; }

    public double EffectiveMaxWidth { get; init; }

    public double EffectiveWidth { get; init; }

    public double EffectiveMaxHeight { get; init; }

    /// <summary>NaN — высота по контенту.</summary>
    public double EffectiveHeight { get; init; }
}

/// <summary>Вычисление границ hosted-диалога относительно <c>DialogBoundsHost</c> (margin 20 — только в XAML).</summary>
public static class HostedDialogBoundsHelper
{
    public static HostedDialogEffectiveBounds Compute(
        HostedDialogSpec requested,
        double availableWidth,
        double availableHeight,
        double? minWidthOverride = null)
    {
        var requestedMinWidth = minWidthOverride ?? requested.MinWidth;

        if (availableWidth <= 0 || availableHeight <= 0)
        {
            var fallbackHeight = requested.MaxHeight ?? double.NaN;
            return new HostedDialogEffectiveBounds
            {
                EffectiveMinWidth = requestedMinWidth,
                EffectiveMaxWidth = requested.MaxWidth,
                EffectiveWidth = requested.MaxWidth,
                EffectiveMaxHeight = requested.MaxHeight ?? double.PositiveInfinity,
                EffectiveHeight = fallbackHeight
            };
        }

        var effectiveMaxWidth = Math.Min(requested.MaxWidth, availableWidth);
        var requestedMaxHeight = requested.MaxHeight ?? double.PositiveInfinity;
        var effectiveMaxHeight = Math.Min(requestedMaxHeight, availableHeight);
        var effectiveMinWidth = Math.Min(requestedMinWidth, effectiveMaxWidth);
        var effectiveWidth = effectiveMaxWidth;
        var effectiveHeight = requested.MaxHeight.HasValue ? effectiveMaxHeight : double.NaN;

        return new HostedDialogEffectiveBounds
        {
            EffectiveMinWidth = effectiveMinWidth,
            EffectiveMaxWidth = effectiveMaxWidth,
            EffectiveWidth = effectiveWidth,
            EffectiveMaxHeight = effectiveMaxHeight,
            EffectiveHeight = effectiveHeight
        };
    }
}
