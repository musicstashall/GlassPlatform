using GlassSettings.Controls;
using GlassSettings.Core.Snapshot;
using GlassSettings.Pages;
using Microsoft.UI.Xaml;

namespace GlassSettings.Services;

/// <summary>Единая точка обновления точек несохранённости: nav, «Обзор», активная страница.</summary>
public static class SettingsUnsavedIndicatorCoordinator
{
    private static readonly IReadOnlyDictionary<string, SettingsCommitScope> NavScopes =
        new Dictionary<string, SettingsCommitScope>
        {
            ["glass"] = SettingsCommitScope.GlassPlatform,
            ["dwm"] = SettingsCommitScope.DwmPlatform,
        };

    /// <summary>Вызывается из <see cref="SettingsSession"/> через DirtyChanged в MainWindow.</summary>
    public static void RefreshAll(MainWindow window, HomePage? home = null)
    {
        RefreshNav(window);
        home?.RefreshCustomizationUnsavedIndicators();
        if (window.ActivePageRoot is { } pageRoot)
            SettingsUnsavedIndicatorRefresh.Refresh(pageRoot);
    }

    public static void RefreshNav(MainWindow window)
    {
        foreach (var (tag, scope) in NavScopes)
        {
            if (!window.TryGetNavUnsavedDot(tag, out var dot))
                continue;

            dot.Visibility = SettingsSession.Current.HasUnsavedInScope(scope)
                ? Visibility.Visible
                : Visibility.Collapsed;
        }
    }
}
