using GlassSettings.Strings;

namespace GlassSettings.Services;

/// <summary>Hosted-диалоги экспорта атласа активной темы в каталог AtlasTheme.</summary>
public static class ThemeAtlasExportDialogService
{
    public static Task<bool> ConfirmExportAsync(string fileName) =>
        HostedConfirmationDialogService.ConfirmAsync(new HostedConfirmationSpec
        {
            Title = GlassSettingsResources.Get("Dialog_ExportAtlasTitle"),
            Body = HostedConfirmationDialogService.CreateBody(
                GlassSettingsResources.Format("Dialog_ExportAtlasBody", fileName)),
            Footer = HostedConfirmationDialogService.CreateCancelConfirmFooter(
                GlassSettingsResources.Get("Dialog_ExportAtlasConfirm"))
        });

    public static Task<bool> ConfirmOverwriteAsync(string fileName) =>
        HostedConfirmationDialogService.ConfirmAsync(new HostedConfirmationSpec
        {
            Title = GlassSettingsResources.Get("Dialog_ExportAtlasOverwriteTitle"),
            Body = HostedConfirmationDialogService.CreateBody(
                GlassSettingsResources.Format("Dialog_ExportAtlasOverwriteBody", fileName)),
            Footer = HostedConfirmationDialogService.CreateCancelConfirmFooter(
                GlassSettingsResources.Get("Dialog_ExportAtlasOverwriteConfirm"))
        });
}
