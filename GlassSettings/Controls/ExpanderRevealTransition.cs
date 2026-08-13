using Microsoft.UI.Xaml.Controls;
using Windows.UI.ViewManagement;

namespace GlassSettings.Controls;

/// <summary>Системный expand/collapse Expander с awaitable Task (для параллели с pivot).</summary>
internal static class ExpanderRevealTransition
{
    public static async Task SetExpandedAsync(
        Expander expander,
        bool expanded,
        CancellationToken cancellationToken = default)
    {
        if (expander is null)
            return;

        if (expander.IsExpanded == expanded)
            return;

        if (!ShouldAnimate())
        {
            expander.IsExpanded = expanded;
            return;
        }

        expander.IsExpanded = expanded;

        try
        {
            await Task.Delay(PivotTabContentTransition.TabChangeTotalDuration, cancellationToken)
                .ConfigureAwait(true);
        }
        catch (OperationCanceledException)
        {
            expander.IsExpanded = expanded;
        }
    }

    private static bool ShouldAnimate()
    {
        try
        {
            return new UISettings().AnimationsEnabled;
        }
        catch
        {
            return true;
        }
    }
}
