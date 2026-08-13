using GlassSettings.Strings;

namespace GlassSettings.Services;

/// <summary>Подтверждение отключения global gate Menu Advanced (wipe всех Menu\*).</summary>
public static class MenuAdvancedRemoveDialogService
{
    public static Task<bool> ConfirmAsync() =>
        HostedConfirmationDialogService.ConfirmAsync(CreateSpec());

    public static Task<bool> ConfirmThenAsync(Func<Task> procedure) =>
        HostedConfirmationDialogService.ConfirmThenAsync(CreateSpec(), procedure);

    private static HostedConfirmationSpec CreateSpec() =>
        new()
        {
            Title = GlassSettingsResources.Get("Dialog_MenuAdvancedRemoveTitle"),
            Body = HostedConfirmationDialogService.CreateBody(
                GlassSettingsResources.Get("Dialog_MenuAdvancedRemoveBody")),
            MinWidth = 420,
            MaxWidth = 480,
            Footer = HostedConfirmationDialogService.CreateCancelConfirmFooter(
                GlassSettingsResources.Get("Dialog_RemoveSectionConfirm"))
        };
}
