namespace GlassSettings.Core.Snapshot;

/// <summary>
/// Scope notice/точек панели задач:
/// бар = Tray + Tray\Xaml (exact); смежные = Tray\Xaml\* (descendants).
/// </summary>
internal static class TaskbarScopeCatalog
{
    public const string TraySub = "Tray";
    public const string BarXamlSub = "Tray\\Xaml";

    public static readonly string[] AdjacentSurfaceSubs =
    [
        "Tray\\Xaml\\Popup",
        "Tray\\Xaml\\Preview",
        "Tray\\Xaml\\Menu",
        "Tray\\Xaml\\Tooltip",
    ];

    /// <summary>
    /// Страница «Панель задач»: Tray + Tray\Xaml (и прочие Tray\*), без Tray\Xaml\Popup|… .
    /// </summary>
    public static SettingsCommitScope BarPageScope { get; } =
        SettingsCommitScope.ForGlassPartitionExceptDescendants(BarXamlSub);

    /// <summary>Страница «Смежные окна»: любые ключи под Tray\Xaml\…</summary>
    public static SettingsCommitScope AdjacentPageScope { get; } =
        SettingsCommitScope.ForGlassSubKeyDescendants(BarXamlSub);

    public static SettingsCommitScope AdjacentSurfaceScope(string surfaceSub) =>
        SettingsCommitScope.ForGlassSubKeysExact(surfaceSub);
}
