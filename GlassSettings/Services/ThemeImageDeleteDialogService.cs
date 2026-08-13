using GlassSettings.Strings;

namespace GlassSettings.Services;

/// <summary>Hosted-диалоги удаления пользовательских изображений коллекций стилизации.</summary>
public static class ThemeImageDeleteDialogService
{
    public static Task<bool> ConfirmDeleteAsync(string fileName) =>
        HostedConfirmationDialogService.ConfirmAsync(new HostedConfirmationSpec
        {
            Title = GlassSettingsResources.Get("Dialog_DeleteThemeImageTitle"),
            Body = HostedConfirmationDialogService.CreateBody(
                GlassSettingsResources.Format("Dialog_DeleteThemeImageBody", fileName)),
            Footer = HostedConfirmationDialogService.CreateCancelConfirmFooter(
                GlassSettingsResources.Get("Attach_MenuDelete"))
        });

    public static Task ShowInUseAsync(string fileName) =>
        HostedConfirmationDialogService.ConfirmAsync(new HostedConfirmationSpec
        {
            Title = GlassSettingsResources.Get("Dialog_DeleteThemeImageTitle"),
            Body = HostedConfirmationDialogService.CreateBody(
                GlassSettingsResources.Format("Dialog_DeleteThemeImageInUse", fileName)),
            Footer = HostedConfirmationDialogService.CreateFooter(
                GlassSettingsResources.Get("Dialog_Understood"))
        });
}
