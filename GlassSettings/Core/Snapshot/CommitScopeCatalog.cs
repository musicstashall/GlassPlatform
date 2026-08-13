using GlassSettings.Core.Customization;
using GlassSettings.Core.Flyout;
using GlassSettings.Core.Menu;
using GlassSettings.Core.Registry;

namespace GlassSettings.Core.Snapshot;

internal static class CommitScopeCatalog
{
    private static readonly GlassRegistryPartition[] PopupPartitions =
        [GlassRegistryPartition.Menu, GlassRegistryPartition.DropDown, GlassRegistryPartition.ToolTip];

    private static readonly GlassRegistryPartition[] GlassPlatformPartitions =
        Enum.GetValues<GlassRegistryPartition>();

    private static readonly DwmSnapshotPartition[] AllDwmPartitions =
        [DwmSnapshotPartition.Colorization, DwmSnapshotPartition.Glass, DwmSnapshotPartition.Theme, DwmSnapshotPartition.System];

    public static IReadOnlyList<GlassRegistryPartition> GetGlassPartitions(SettingsCommitScope scope) => scope.Kind switch
    {
        SettingsCommitScopeKind.All =>
            Enum.GetValues<GlassRegistryPartition>(),
        SettingsCommitScopeKind.Area when scope.Area == CustomizationAreaId.General =>
            [GlassRegistryPartition.GeneralRoot],
        SettingsCommitScopeKind.Area when scope.Area == CustomizationAreaId.Root =>
            [GlassRegistryPartition.Root],
        SettingsCommitScopeKind.Area when scope.Area == CustomizationAreaId.Popup =>
            PopupPartitions,
        SettingsCommitScopeKind.Area when scope.Area == CustomizationAreaId.Taskbar =>
            [GlassRegistryPartition.Tray],
        SettingsCommitScopeKind.Area when scope.Area == CustomizationAreaId.Text =>
            [GlassRegistryPartition.GeneralRoot],
        SettingsCommitScopeKind.GlassSubKey when scope.GlassSubKey is { } subKey =>
            TryMapGlassSubKeyToPartition(subKey) is { } p ? [p] : [],
        SettingsCommitScopeKind.DwmPlatform => [],
        SettingsCommitScopeKind.GlassPlatform => GlassPlatformPartitions,
        SettingsCommitScopeKind.Processes or SettingsCommitScopeKind.System => [],
        _ => []
    };

    public static IReadOnlyList<DwmSnapshotPartition> GetDwmPartitions(SettingsCommitScope scope) => scope.Kind switch
    {
        SettingsCommitScopeKind.All or SettingsCommitScopeKind.DwmPlatform => AllDwmPartitions,
        SettingsCommitScopeKind.Area when scope.Area == CustomizationAreaId.DwmColorization =>
            [DwmSnapshotPartition.Colorization],
        SettingsCommitScopeKind.Area when scope.Area == CustomizationAreaId.DwmGlass =>
            [DwmSnapshotPartition.Glass],
        SettingsCommitScopeKind.Area when scope.Area == CustomizationAreaId.DwmTheme =>
            [DwmSnapshotPartition.Theme],
        _ => []
    };

    public static IReadOnlyList<RegistryParameterRef> GetTrackedGlassParameters(SettingsCommitScope scope) =>
        scope.Kind switch
        {
            SettingsCommitScopeKind.FlyoutParameterGroup
                when scope.GlassSubKey is { } regSub && scope.FlyoutGroup is { } group =>
                FlyoutScopeCatalog.GetParameters(regSub, group),
            SettingsCommitScopeKind.GlassSubKey when scope.GlassSubKey is { } subKey =>
                CustomizationCatalog.GetFlyoutSectionTrackedParameters(subKey),
            SettingsCommitScopeKind.GlassValueSubset when scope.GlassValueNames is { Length: > 0 } names =>
                names.Select(static name => RegistryParameterRef.Glass("", name)).ToList(),
            SettingsCommitScopeKind.Area when scope.Area is { } area =>
                CustomizationCatalog.GetParameters(area)
                    .Where(static p => p.Store == RegistryStoreKind.Glass)
                    .ToList(),
            SettingsCommitScopeKind.All =>
                CustomizationCatalog.HomeSummaryAreas
                    .SelectMany(CustomizationCatalog.GetParameters)
                    .Where(static p => p.Store == RegistryStoreKind.Glass)
                    .ToList(),
            SettingsCommitScopeKind.GlassPlatform =>
                Enum.GetValues<CustomizationAreaId>()
                    .Where(static id => id is not (
                        CustomizationAreaId.DwmColorization
                        or CustomizationAreaId.DwmGlass
                        or CustomizationAreaId.DwmTheme))
                    .SelectMany(CustomizationCatalog.GetParameters)
                    .Where(static p => p.Store == RegistryStoreKind.Glass)
                    .ToList(),
            _ => []
        };

    public static IReadOnlyList<string> GetDwmValueNames(SettingsCommitScope scope)
    {
        if (scope.Kind == SettingsCommitScopeKind.DwmKeySubset
            && scope.DwmValueNames is { Length: > 0 } names)
        {
            return names;
        }

        var keys = new List<string>();
        foreach (var partition in GetDwmPartitions(scope))
            keys.AddRange(DwmSnapshotCatalog.GetKeys(partition));
        return keys;
    }

    private static GlassRegistryPartition? TryMapGlassSubKeyToPartition(string relativeSubKey)
    {
        var normalized = relativeSubKey.Replace('/', '\\').Trim('\\');
        if (MenuItemRegistry.IsMenuItemSubKey(normalized))
            return GlassRegistryPartition.Menu;

        var first = normalized.Split('\\')[0];
        return first.ToUpperInvariant() switch
        {
            "MENU" => GlassRegistryPartition.Menu,
            "DROPDOWN" => GlassRegistryPartition.DropDown,
            "TOOLTIP" => GlassRegistryPartition.ToolTip,
            "ROOT" => GlassRegistryPartition.Root,
            "TRAY" => GlassRegistryPartition.Tray,
            _ => null
        };
    }
}
