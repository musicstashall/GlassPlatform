using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Controls;

/// <summary>Мост между row-контролами и единым layout-host.</summary>
internal static class SpoilerHeaderRowLayoutBridge
{
    public static void ApplyHeaderControls(
        SpoilerHeaderGrid chrome,
        SpoilerHeaderLayoutHost host,
        object? actions,
        object? slider)
    {
        ApplyCell(chrome.HeaderControlsTable, 0, actions);
        ApplyCell(chrome.HeaderControlsTable, 1, slider);
        chrome.HeaderControlsTable.SyncLabelColumnWidths();
        SpoilerHeaderLayoutMetrics.DetachFromVisualParent(chrome.HeaderControlsTable);
        chrome.AttachControlsTable(chrome.HeaderControlsTable);
        host.Invalidate();
    }

    /// <summary>Gate off: actions → StaticHost, slider → cell 0.</summary>
    public static void ApplyHeaderControlsWithoutGate(
        SpoilerHeaderGrid chrome,
        SpoilerHeaderLayoutHost host,
        object? staticContent,
        object? slider)
    {
        SpoilerHeaderLayoutMetrics.DetachFromVisualParent(staticContent);
        chrome.StaticContent = staticContent;
        ApplyCell(chrome.HeaderControlsTable, 0, slider);
        chrome.HeaderControlsTable.ClearCell(1);
        chrome.HeaderControlsTable.SyncLabelColumnWidths();
        SpoilerHeaderLayoutMetrics.DetachFromVisualParent(chrome.HeaderControlsTable);
        chrome.AttachControlsTable(chrome.HeaderControlsTable);
        host.Invalidate();
    }

    public static SpoilerHeaderLayoutOptions CreateOptions(SpoilerHeaderGrid chrome, bool hasGateRail, double trailingExtra = 0) =>
        new()
        {
            HasGateRail = hasGateRail,
            TrailingExtra = trailingExtra
        };

    private static void ApplyCell(SpoilerHeaderControlsTable table, int index, object? content)
    {
        if (content is null)
            table.ClearCell(index);
        else
            table.SetCellFromLegacy(index, content);
    }
}
