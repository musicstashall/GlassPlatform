using GlassSettings.Controls.Processes;
using GlassSettings.Strings;
using Microsoft.UI.Dispatching;

namespace GlassSettings.Services;

/// <summary>Hosted-диалог ручного ввода имени процесса (Enter → штора + progress → резолв).</summary>
public static class ProcessNameInputDialogService
{
    /// <summary>Attach-list: штора с progress; при успехе может остаться открытой.</summary>
    public static async Task<string?> PickAttachListAsync(
        DispatcherQueue dispatcherQueue,
        bool retainOverlayAfterSelection = false)
    {
        _ = dispatcherQueue;
        var retainOverlay = false;
        WindowBusyOverlayService.Push(showProgress: true);
        try
        {
            var picked = await PickAsync().ConfigureAwait(true);
            retainOverlay = retainOverlayAfterSelection && picked is not null;
            return picked;
        }
        finally
        {
            if (!retainOverlay)
                WindowBusyOverlayService.Pop();
        }
    }

    /// <summary>Block-list: штора на время всего сценария.</summary>
    public static Task<string?> PickBlockListAsync(DispatcherQueue dispatcherQueue) =>
        PickAttachListAsync(dispatcherQueue, retainOverlayAfterSelection: false);

    private static async Task<string?> PickAsync()
    {
        string? draftText = null;

        while (true)
        {
            var panel = new ProcessNameInputPanel(draftText);

            var result = await HostedDialogService.ShowAsync(
                new HostedDialogSpec
                {
                    Title = GlassSettingsResources.Get("Attach_ProcessNameInputTitle"),
                    Body = HostedDialogLayout.WrapBodyWithContentPadding(panel),
                    MinWidth = 350,
                    MaxWidth = 350,
                    ManageOverlay = false,
                    Footer = null
                },
                panel.AttachDialogContext).ConfigureAwait(true);

            if (result != HostedDialogResult.Primary)
                return null;

            WindowBusyOverlayService.SetProgressVisible(true);

            var rawText = panel.GetEnteredText();
            draftText = rawText;

            if (string.IsNullOrWhiteSpace(rawText))
                return null;

            var resolved = await Task.Run(() => ProcessIconResolver.TryResolveManualProcessName(rawText))
                .ConfigureAwait(true);

            switch (resolved.Status)
            {
                case ProcessIconResolver.ManualProcessNameResolveStatus.InvalidExtension:
                    await ShowInvalidTypeDialogAsync().ConfigureAwait(true);
                    WindowBusyOverlayService.SetProgressVisible(false);
                    continue;

                case ProcessIconResolver.ManualProcessNameResolveStatus.ExtensionRequired:
                    await ShowExtensionRequiredDialogAsync().ConfigureAwait(true);
                    WindowBusyOverlayService.SetProgressVisible(false);
                    continue;

                case ProcessIconResolver.ManualProcessNameResolveStatus.Success:
                    await ProcessIconResolver.PreloadExeAssetsAsync(resolved.ExeName!).ConfigureAwait(true);
                    return resolved.ExeName;

                default:
                    return null;
            }
        }
    }

    private static Task ShowInvalidTypeDialogAsync() =>
        HostedConfirmationDialogService.ConfirmAsync(new HostedConfirmationSpec
        {
            Title = GlassSettingsResources.Get("Attach_FilePickerInvalidTypeTitle"),
            Body = HostedConfirmationDialogService.CreateBody(
                GlassSettingsResources.Get("Attach_FilePickerInvalidTypeBody")),
            Footer = HostedConfirmationDialogService.CreateFooter(
                GlassSettingsResources.Get("Dialog_Understood"),
                primaryButtonWidth: 108),
            ManageOverlay = false,
            MinWidth = 420,
            MaxWidth = 480
        });

    private static Task ShowExtensionRequiredDialogAsync() =>
        HostedConfirmationDialogService.ConfirmAsync(new HostedConfirmationSpec
        {
            Title = GlassSettingsResources.Get("Attach_ProcessNameExtensionRequiredTitle"),
            Body = HostedConfirmationDialogService.CreateBody(
                GlassSettingsResources.Get("Attach_ProcessNameExtensionRequiredBody")),
            Footer = HostedConfirmationDialogService.CreateFooter(
                GlassSettingsResources.Get("Dialog_Understood"),
                primaryButtonWidth: 108),
            ManageOverlay = false,
            MinWidth = 420,
            MaxWidth = 480
        });
}
