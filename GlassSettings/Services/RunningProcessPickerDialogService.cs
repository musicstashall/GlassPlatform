using GlassSettings.Controls.Processes;
using GlassSettings.Strings;
using Microsoft.UI.Dispatching;

namespace GlassSettings.Services;

/// <summary>Диалог выбора exe из списка запущенных процессов.</summary>
public static class RunningProcessPickerDialogService
{
    /// <summary>Attach-list: штора сразу, затем сбор списка и hosted-диалог.</summary>
    /// <param name="retainOverlayAfterSelection">
    /// Если true и процесс выбран — штора остаётся открытой; вызывающий код обязан вызвать <see cref="WindowBusyOverlayService.Pop"/>.
    /// </param>
    public static async Task<string?> PickAttachListAsync(
        DispatcherQueue dispatcherQueue,
        bool retainOverlayAfterSelection = false)
    {
        var retainOverlay = false;
        WindowBusyOverlayService.Push(showProgress: true);
        try
        {
            var processes = await RunningProcessCatalog.GetRunningProcessesAsync().ConfigureAwait(true);
            var items = await RunningProcessPickerItemsBuilder.BuildAsync(processes, dispatcherQueue)
                .ConfigureAwait(true);
            var picked = await PickAsync(items).ConfigureAwait(true);
            retainOverlay = retainOverlayAfterSelection && picked is not null;
            return picked;
        }
        finally
        {
            if (!retainOverlay)
                WindowBusyOverlayService.Pop();
        }
    }

    /// <summary>Block-list: штора сразу, затем сбор списка и hosted-диалог.</summary>
    public static async Task<string?> PickBlockListAsync(DispatcherQueue dispatcherQueue)
    {
        WindowBusyOverlayService.Push(showProgress: true);
        try
        {
            var processes = await RunningProcessCatalog.GetRunningProcessesForBlockListAsync().ConfigureAwait(true);
            var items = await RunningProcessPickerItemsBuilder.BuildAsync(processes, dispatcherQueue)
                .ConfigureAwait(true);
            return await PickAsync(items).ConfigureAwait(true);
        }
        finally
        {
            WindowBusyOverlayService.Pop();
        }
    }

    public static async Task<string?> PickAsync(IReadOnlyList<RunningProcessPickerItem> processes)
    {
        var panel = new RunningProcessPickerPanel(processes);

        var result = await HostedDialogService.ShowAsync(
            new HostedDialogSpec
            {
                Title = GlassSettingsResources.Get("Attach_ProcessPickerTitle"),
                Body = HostedDialogLayout.WrapBodyWithContentPadding(panel),
                MinWidth = 460,
                MaxWidth = 560,
                MaxHeight = RunningProcessPickerLayoutMetrics.DialogMaxHeight,
                ManageOverlay = false,
                BodyHandlesScroll = true,
                Footer = new HostedDialogFooterSpec
                {
                    PrimaryButtonText = GlassSettingsResources.Get("Attach_ProcessPickerSelect"),
                    PrimaryButtonWidth = 108,
                    IsPrimaryEnabled = false
                }
            },
            panel.AttachDialogContext).ConfigureAwait(true);

        if (result != HostedDialogResult.Primary)
            return null;

        return panel.GetSelectedExeName();
    }
}
