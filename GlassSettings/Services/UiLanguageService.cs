using GlassSettings.Core.Registry;
using GlassSettings.Strings;
using Microsoft.UI.Dispatching;

namespace GlassSettings.Services;

/// <summary>Override языка UI GlassSettings через ResourceContext MRT Core.</summary>
public static class UiLanguageService
{
    public static void ApplyAtStartup() => ApplyFromRegistry();

    public static void ApplyFromRegistry() =>
        GlassSettingsResources.SetLanguageOverride(GlassSettingsRegistry.ReadAppUiLanguage());

    public static void WriteAndApply(string? languageTag)
    {
        GlassSettingsRegistry.WriteAppUiLanguage(languageTag);
        ApplyFromRegistry();

        if (App.ShellWindow?.DispatcherQueue is { } queue)
            queue.TryEnqueue(DispatcherQueuePriority.Low, UiLanguageRefreshService.RefreshAll);
        else
            UiLanguageRefreshService.RefreshAll();
    }

    public static string GetDisplayName(string? languageTag) =>
        AppUiLanguage.IsSystem(languageTag)
            ? GlassSettingsResources.Get("AppSettings_LanguageSystem")
            : UiLanguageCatalog.GetDisplayName(languageTag!);
}
