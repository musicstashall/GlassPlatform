using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml.Media;

namespace GlassSettings.Services;

/// <summary>Иконки и строки для диалога выбора процесса.</summary>
public static class RunningProcessPickerItemsBuilder
{
    public static async Task<IReadOnlyList<RunningProcessPickerItem>> BuildAsync(
        IReadOnlyList<RunningProcessInfo> processes,
        DispatcherQueue dispatcherQueue)
    {
        if (processes.Count == 0)
            return [];

        await ProcessIconResolver.EnsureIconsAsync(processes.Select(static p => p.ExeName)).ConfigureAwait(false);

        var items = new List<RunningProcessPickerItem>(processes.Count);
        foreach (var info in processes)
        {
            var icon = await ResolveIconAsync(info.ExeName, dispatcherQueue).ConfigureAwait(true);
            items.Add(RunningProcessPickerItem.FromInfo(info, icon));
        }

        return items;
    }

    private static async Task<ImageSource?> ResolveIconAsync(string exeName, DispatcherQueue dispatcherQueue)
    {
        if (ProcessIconResolver.TryGetCachedIcon(exeName) is { } cached)
            return cached;

        return await ProcessIconResolver.GetIconAsync(exeName, dispatcherQueue).ConfigureAwait(true)
               ?? ProcessIconResolver.TryGetDefaultFileTypeIcon(exeName);
    }
}
