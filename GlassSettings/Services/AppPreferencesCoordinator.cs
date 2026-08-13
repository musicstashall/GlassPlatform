using GlassSettings.Core.Registry;
using Microsoft.UI.Xaml;

namespace GlassSettings.Services;

/// <summary>Применение настроек приложения к главному окну и открытому диалогу.</summary>
public static class AppPreferencesCoordinator
{
    public static void ApplyFromRegistry()
    {
        App.ShellWindow?.ApplyAppPreferencesFromRegistry();
        AppSettingsDialogService.ApplyToActiveDialog();
    }
}
