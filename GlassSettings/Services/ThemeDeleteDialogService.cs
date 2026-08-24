using GlassSettings.Strings;

namespace GlassSettings.Services;

/// <summary>Hosted-подтверждение удаления визуального стиля (только .theme или вся папка).</summary>
public static class ThemeDeleteDialogService
{
    public static async Task<bool> ConfirmAsync(ThemeDeleteService.ThemeDeletePlan plan)
    {
        if (ThemeDeleteService.IsActiveLibrary(plan))
        {
            await HostedConfirmationDialogService.ConfirmAsync(new HostedConfirmationSpec
            {
                Title = GlassSettingsResources.Get("Themes_DeleteMenu"),
                Body = HostedConfirmationDialogService.CreateBody(
                    GlassSettingsResources.Get("Themes_DeleteActiveLibraryBody")),
                Footer = HostedConfirmationDialogService.CreateFooter(
                    GlassSettingsResources.Get("CmdOk"))
            }).ConfigureAwait(true);
            return false;
        }

        var bodyKey = plan.Kind == ThemeDeleteService.ThemeDeleteKind.FullLibrary
            ? "Themes_DeleteConfirmFullFormat"
            : "Themes_DeleteConfirmThemeOnlyFormat";

        return await HostedConfirmationDialogService.ConfirmAsync(new HostedConfirmationSpec
        {
            Title = GlassSettingsResources.Get("Themes_DeleteMenu"),
            Body = HostedConfirmationDialogService.CreateBody(
                GlassSettingsResources.Format(bodyKey, plan.DisplayName)),
            Footer = HostedConfirmationDialogService.CreateCancelConfirmFooter(
                GlassSettingsResources.Get("CmdOk"))
        }).ConfigureAwait(true);
    }
}
