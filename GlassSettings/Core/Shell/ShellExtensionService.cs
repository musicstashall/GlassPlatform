using GlassSettings.Core.Platform;
using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Core.Shell;

public static class ShellExtensionService
{
    public static bool IsBundledDllRegistered()
    {
        try
        {
            return NativeHostInterop.IsShellExtensionRegistered();
        }
        catch
        {
            return false;
        }
    }

    public static bool IsBundledShellExtensionPresent() =>
        ShellExtensionAvailability.IsBundledShellExtensionPresent();

    public static bool CanToggleShellExtension =>
        IsBundledDllRegistered()
        || IsBundledShellExtensionPresent()
        || GlassAutorunTaskExists();

    public static bool CanRegisterShellExtension(bool autorunTaskExists) =>
        ShellExtensionAvailability.CanRegisterShellExtension(autorunTaskExists);

    public static bool IsShellExtensionOperationInProgress()
    {
        try
        {
            return NativeHostInterop.IsShellExtensionOperationInProgress();
        }
        catch
        {
            return false;
        }
    }

    public static async Task<bool> SetShellExtensionRegisteredAsync(bool register)
    {
        if (IsShellExtensionOperationInProgress())
            return false;

        var taskExists = GlassAutorunTaskExists();
        var currentlyRegistered = IsBundledDllRegistered();

        if (!register && !currentlyRegistered)
            return true;

        if (register && !CanRegisterShellExtension(taskExists))
            return false;

        if (!NativeHostInterop.BeginShellExtensionChange(register))
        {
            await ShowShellExtensionErrorAsync().ConfigureAwait(true);
            return false;
        }

        await WindowBusyOverlayService.RunAsync(async () =>
        {
            while (IsShellExtensionOperationInProgress())
                await Task.Delay(250).ConfigureAwait(false);
        }, showProgress: true).ConfigureAwait(true);

        var ok = IsBundledDllRegistered() == register;
        if (ok)
            NativeHostInterop.NotifyShellCachesAfterRegistrationChange();
        else
            await ShowShellExtensionErrorAsync().ConfigureAwait(true);

        return ok;
    }

    private static bool GlassAutorunTaskExists() => GlassPlatformService.GlassAutorunTaskExists();

    private static async Task ShowShellExtensionErrorAsync()
    {
        var details = ResolveShellExtensionErrorMessage(NativeHostInterop.GetShellExtensionLastOpMessageId());
        if (string.IsNullOrWhiteSpace(details))
            details = GlassSettingsResources.Get("Error_ShellExtensionFailed");

        if (App.ShellWindow?.WindowXamlRoot is null)
            return;

        var dialog = new ContentDialog
        {
            Title = GlassSettingsResources.Get("Error_ShellExtensionTitle"),
            Content = details,
            CloseButtonText = GlassSettingsResources.Get("Dialog_Close"),
            XamlRoot = App.ShellWindow.WindowXamlRoot
        };

        _ = await dialog.ShowAsync();
    }

    private static string ResolveShellExtensionErrorMessage(int messageId) =>
        messageId switch
        {
            2 => GlassSettingsResources.Get("Error_ShellExtensionTimedOut"),
            3 => GlassSettingsResources.Get("Error_ShellExtensionInconsistentState"),
            4 => GlassSettingsResources.Get("Error_ShellExtensionFailedToStart"),
            5 => GlassSettingsResources.Get("Error_ShellExtensionFailedToApply"),
            6 => GlassSettingsResources.Get("Error_ShellExtensionNeedsExplorerRestart"),
            7 => GlassSettingsResources.Get("Error_ShellExtensionSparseArtifactsMissing"),
            8 => GlassSettingsResources.Get("Error_ShellExtensionSparseCertFailed"),
            9 => GlassSettingsResources.Get("Error_ShellExtensionSparseDeployFailed"),
            10 => GlassSettingsResources.Get("Error_ShellExtensionFailedToRegister"),
            11 => GlassSettingsResources.Get("Error_ShellExtensionFailedToUnregister"),
            12 => GlassSettingsResources.Get("Error_ShellExtensionSparseExternalLocationConflict"),
            _ => string.Empty,
        };
}
