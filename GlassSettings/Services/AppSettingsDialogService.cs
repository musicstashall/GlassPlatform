using GlassSettings.Controls;
using GlassSettings.Strings;
using Microsoft.UI.Xaml;

namespace GlassSettings.Services;

/// <summary>Диалог настроек самого приложения GlassSettings.</summary>
public static class AppSettingsDialogService
{
    private static AppSettingsPanel? _activePanel;

    public static async Task ShowAsync(XamlRoot xamlRoot)
    {
        _ = xamlRoot;

        var panel = new AppSettingsPanel();
        _activePanel = panel;

        try
        {
            _ = await HostedDialogService.ShowAsync(new HostedDialogSpec
            {
                Title = GlassSettingsResources.Get("Dialog_AppSettingsTitle"),
                Body = HostedDialogLayout.WrapBodyWithContentPadding(panel),
                MinWidth = 580,
                MaxWidth = 660
            });
        }
        finally
        {
            _activePanel = null;
        }
    }

    internal static void ApplyToActiveDialog()
    {
        if (_activePanel is null)
            return;

        App.ShellWindow?.ApplyHostedDialogTheme();
    }

    internal static void RefreshActiveDialog()
    {
        if (_activePanel is null)
            return;

        App.ShellWindow?.RefreshHostedDialogTitle(GlassSettingsResources.Get("Dialog_AppSettingsTitle"));
        _activePanel.RefreshLocalizedContent();
        App.ShellWindow?.ApplyHostedDialogTheme();
    }
}
