namespace GlassSettings.Services;

/// <summary>Обновление UI после смены языка.</summary>
public static class UiLanguageRefreshService
{
    public static void RefreshAll() => App.ShellWindow?.RefreshUiLanguage();
}
