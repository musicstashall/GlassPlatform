using GlassSettings.Core.Snapshot;
using GlassSettings.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;

namespace GlassSettings.Controls;

/// <summary>
/// Обновляет индикаторы несохранённости: точки на SpoilerHeaderGrid / gate-row и подсветку
/// свёрнутого <see cref="SettingsDividerExpander"/>.
/// </summary>
public static class SettingsUnsavedIndicatorRefresh
{
    public static void Refresh(FrameworkElement root)
    {
        Walk(root, host =>
        {
            if (host is SettingsDividerExpander divider)
            {
                divider.RefreshUnsavedIndicator();
                return;
            }

            if (host.UnsavedScope is not { } scope)
            {
                host.IsUnsavedPending = false;
                return;
            }

            host.IsUnsavedPending = SettingsSession.Current.HasUnsavedInScope(scope);
        });
    }

    private static void Walk(DependencyObject node, Action<ISettingsUnsavedScopeHost> visit)
    {
        if (node is ISettingsUnsavedScopeHost host)
            visit(host);

        var count = VisualTreeHelper.GetChildrenCount(node);
        for (var i = 0; i < count; i++)
            Walk(VisualTreeHelper.GetChild(node, i), visit);
    }
}

public interface ISettingsUnsavedScopeHost
{
    SettingsCommitScope? UnsavedScope { get; set; }
    bool IsUnsavedPending { get; set; }
}
