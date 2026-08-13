using GlassSettings.Strings;

namespace GlassSettings.Services;

/// <summary>Hosted-диалоги сценариев загрузки символов OpenGlass.</summary>
public static class OpenGlassSymbolDownloadDialogService
{
    public static Task ShowMissingSymbolToolsAsync(IReadOnlyList<string> missingDlls)
    {
        var missing = missingDlls.Count > 0
            ? string.Join(", ", missingDlls)
            : "dbghelp.dll, symsrv.dll";

        return HostedConfirmationDialogService.ConfirmAsync(new HostedConfirmationSpec
        {
            Title = GlassSettingsResources.Get("Dialog_SymbolsMissingToolsTitle"),
            Body = HostedConfirmationDialogService.CreateBody(
                GlassSettingsResources.Format("Dialog_SymbolsMissingToolsBody", missing)),
            Footer = HostedConfirmationDialogService.CreateFooter(
                GlassSettingsResources.Get("Dialog_Understood"))
        });
    }

    public static Task<bool> ShowNoInternetAsync() =>
        HostedConfirmationDialogService.ConfirmAsync(new HostedConfirmationSpec
        {
            Title = GlassSettingsResources.Get("Dialog_SymbolsNoInternetTitle"),
            Body = HostedConfirmationDialogService.CreateBody(
                GlassSettingsResources.Get("Dialog_SymbolsNoInternetBody")),
            Footer = HostedConfirmationDialogService.CreateCancelConfirmFooter(
                GlassSettingsResources.Get("Dialog_Retry"),
                confirmButtonWidth: 120)
        });

    public static Task<bool> ConfirmRetryAfterConnectionLostAsync() =>
        HostedConfirmationDialogService.ConfirmAsync(new HostedConfirmationSpec
        {
            Title = GlassSettingsResources.Get("Dialog_SymbolsConnectionLostTitle"),
            Body = HostedConfirmationDialogService.CreateBody(
                GlassSettingsResources.Get("Dialog_SymbolsConnectionLostBody")),
            Footer = HostedConfirmationDialogService.CreateCancelConfirmFooter(
                GlassSettingsResources.Get("Dialog_Retry"),
                confirmButtonWidth: 120)
        });

    public static Task ShowDownloadSucceededAsync() =>
        HostedConfirmationDialogService.ConfirmAsync(new HostedConfirmationSpec
        {
            Title = GlassSettingsResources.Get("Dialog_SymbolsDownloadSucceededTitle"),
            Body = HostedConfirmationDialogService.CreateBody(
                GlassSettingsResources.Get("Dialog_SymbolsDownloadSucceededBody")),
            Footer = HostedConfirmationDialogService.CreateFooter(
                GlassSettingsResources.Get("Dialog_Understood"))
        });

    public static Task ShowDownloadFailedAsync(string message)
    {
        var body = string.IsNullOrWhiteSpace(message)
            ? GlassSettingsResources.Get("Dialog_SymbolsDownloadFailedBodyFallback")
            : GlassSettingsResources.Format("Dialog_SymbolsDownloadFailedBody", message);

        return HostedConfirmationDialogService.ConfirmAsync(new HostedConfirmationSpec
        {
            Title = GlassSettingsResources.Get("Dialog_SymbolsDownloadFailedTitle"),
            Body = HostedConfirmationDialogService.CreateBody(body),
            Footer = HostedConfirmationDialogService.CreateFooter(
                GlassSettingsResources.Get("Dialog_Understood"))
        });
    }

    public static Task<bool> ConfirmCloseWhileDownloadingAsync() =>
        HostedConfirmationDialogService.ConfirmAsync(new HostedConfirmationSpec
        {
            Title = GlassSettingsResources.Get("Dialog_SymbolsCloseWhileDownloadingTitle"),
            Body = HostedConfirmationDialogService.CreateBody(
                GlassSettingsResources.Get("Dialog_SymbolsCloseWhileDownloadingBody")),
            Footer = HostedConfirmationDialogService.CreateCancelConfirmFooter(
                GlassSettingsResources.Get("Dialog_SymbolsCloseWhileDownloadingConfirm"),
                confirmButtonWidth: 148)
        });

    public static Task<bool> ConfirmClearExistingSymbolsAsync(string cacheDisplayPath) =>
        HostedConfirmationDialogService.ConfirmAsync(new HostedConfirmationSpec
        {
            Title = GlassSettingsResources.Get("Dialog_SymbolsClearCacheTitle"),
            Body = HostedConfirmationDialogService.CreateBody(
                GlassSettingsResources.Format("Dialog_SymbolsClearCacheBody", cacheDisplayPath)),
            Footer = HostedConfirmationDialogService.CreateCancelConfirmFooter(
                GlassSettingsResources.Get("Dialog_SymbolsClearCacheConfirm"),
                confirmButtonWidth: 120)
        });
}
