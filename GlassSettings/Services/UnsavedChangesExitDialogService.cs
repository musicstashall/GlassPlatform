using GlassSettings.Strings;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Services;

/// <summary>Подтверждение закрытия окна при наличии несохранённых изменений (hosted-диалог).</summary>
public static class UnsavedChangesExitDialogService
{
    public static Task<bool> ConfirmExitAsync() =>
        HostedConfirmationDialogService.ConfirmAsync(new HostedConfirmationSpec
        {
            Title = GlassSettingsResources.Get("Dialog_UnsavedChangesExitTitle"),
            Body = HostedConfirmationDialogService.CreateBody(
                GlassSettingsResources.Get("Dialog_UnsavedChangesExitBody")),
            Footer = HostedConfirmationDialogService.CreateCancelConfirmFooter(
                GlassSettingsResources.Get("Dialog_UnsavedChangesExitConfirm"))
        });
}
