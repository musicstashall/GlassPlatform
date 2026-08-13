using GlassSettings.Strings;

namespace GlassSettings.Services;

/// <summary>Подтверждение сохранения настроек перед запуском Glass Platform (hosted-диалог).</summary>
public static class SaveBeforePlatformDialogService
{
    public static Task<bool> ConfirmSaveAndStartAsync() =>
        HostedConfirmationDialogService.ConfirmAsync(new HostedConfirmationSpec
        {
            Title = GlassSettingsResources.Get("Dialog_SaveBeforePlatformTitle"),
            Body = HostedConfirmationDialogService.CreateBody(
                GlassSettingsResources.Get("Dialog_SaveBeforePlatformBody")),
            Footer = HostedConfirmationDialogService.CreateCancelConfirmFooter(
                GlassSettingsResources.Get("Dialog_SaveBeforePlatformConfirm"),
                confirmButtonWidth: 160)
        });
}
