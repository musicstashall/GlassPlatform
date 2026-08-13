using GlassSettings.Core.Customization;
using GlassSettings.Core.Flyout;
using GlassSettings.Core.Menu;
using GlassSettings.Core.Registry;
using GlassSettings.Core.Root;
using GlassSettings.Core.Tooltip;

namespace GlassSettings.Core.Snapshot;

/// <summary>Область commit: весь реестр, раздел Обзора, поддерево BWM или внешний флаг (System).</summary>
public readonly record struct SettingsCommitScope
{
    public SettingsCommitScopeKind Kind { get; init; }
    public CustomizationAreaId? Area { get; init; }
    public string? GlassSubKey { get; init; }
    /// <summary>Несколько exact-subkey (бар Tray + Tray\Xaml); иначе используется GlassSubKey.</summary>
    public string[]? GlassSubKeys { get; init; }
    public GlassSubKeyMatchMode GlassSubKeyMatch { get; init; }
    public FlyoutParameterGroup? FlyoutGroup { get; init; }
    public string[]? DwmValueNames { get; init; }
    public string[]? GlassValueNames { get; init; }
    public string[]? WindowMetricsValueNames { get; init; }
    public DwmRegistryHive? DwmHive { get; init; }

    public static SettingsCommitScope All { get; } = new() { Kind = SettingsCommitScopeKind.All };

    public static SettingsCommitScope System { get; } = new() { Kind = SettingsCommitScopeKind.System };

    public static SettingsCommitScope Processes { get; } = new() { Kind = SettingsCommitScopeKind.Processes };

    public static SettingsCommitScope DwmPlatform { get; } = new() { Kind = SettingsCommitScopeKind.DwmPlatform };

    public static SettingsCommitScope GlassPlatform { get; } = new() { Kind = SettingsCommitScopeKind.GlassPlatform };

    public static SettingsCommitScope ForArea(CustomizationAreaId area) =>
        new() { Kind = SettingsCommitScopeKind.Area, Area = area };

    public static SettingsCommitScope ForArea(CustomizationAreaId area, DwmRegistryHive hive) =>
        new() { Kind = SettingsCommitScopeKind.Area, Area = area, DwmHive = hive };

    public static SettingsCommitScope ForGlassSubKey(string relativeSubKey) =>
        new()
        {
            Kind = SettingsCommitScopeKind.GlassSubKey,
            GlassSubKey = relativeSubKey,
            GlassSubKeyMatch = GlassSubKeyMatchMode.Partition
        };

    /// <summary>Dirty/commit только values в указанных subkey (без дочерних ключей).</summary>
    public static SettingsCommitScope ForGlassSubKeysExact(params string[] relativeSubKeys) =>
        new()
        {
            Kind = SettingsCommitScopeKind.GlassSubKey,
            GlassSubKey = relativeSubKeys is { Length: 1 } ? relativeSubKeys[0] : null,
            GlassSubKeys = relativeSubKeys,
            GlassSubKeyMatch = GlassSubKeyMatchMode.Exact
        };

    /// <summary>Dirty/commit только потомки parent\… (сам parent не входит).</summary>
    public static SettingsCommitScope ForGlassSubKeyDescendants(string parentRelativeSubKey) =>
        new()
        {
            Kind = SettingsCommitScopeKind.GlassSubKey,
            GlassSubKey = parentRelativeSubKey,
            GlassSubKeyMatch = GlassSubKeyMatchMode.Descendants
        };

    /// <summary>
    /// Dirty/commit partition исключённого ключа без его потомков
    /// (Tray без Tray\Xaml\* — страница «Панель задач»).
    /// </summary>
    public static SettingsCommitScope ForGlassPartitionExceptDescendants(string excludeDescendantsOfRelativeSubKey) =>
        new()
        {
            Kind = SettingsCommitScopeKind.GlassSubKey,
            GlassSubKey = excludeDescendantsOfRelativeSubKey,
            GlassSubKeyMatch = GlassSubKeyMatchMode.PartitionExceptDescendants
        };

    public static SettingsCommitScope ForFlyoutParameterGroup(string regSub, FlyoutParameterGroup group) =>
        new()
        {
            Kind = SettingsCommitScopeKind.FlyoutParameterGroup,
            GlassSubKey = regSub,
            FlyoutGroup = group
        };

    public static SettingsCommitScope ForMenuItemKind(MenuItemKind kind) =>
        ForFlyoutParameterGroup(MenuItemRegistry.RegSub(kind), FlyoutParameterGroup.MenuItem);

    public static SettingsCommitScope ForMenuCustomRenderingMaster() =>
        ForFlyoutParameterGroup(
            MenuCustomRenderingCoordinator.MenuScopeSubKey,
            FlyoutParameterGroup.MenuCustomRenderingMaster);

    public static SettingsCommitScope ForMenuRoot() =>
        ForFlyoutParameterGroup(MenuRootRegistry.MenuScopeSubKey, FlyoutParameterGroup.MenuRoot);

    public static SettingsCommitScope ForTooltipRendering() =>
        ForFlyoutParameterGroup(TooltipRenderingRegistry.RegSub, FlyoutParameterGroup.TooltipRendering);

    public static SettingsCommitScope ForRootControlIcon(RootControlIconKind kind) =>
        ForFlyoutParameterGroup(RootControlIconRegistry.RegSub(kind), FlyoutParameterGroup.RootControlIcon);

    /// <summary>Dirty по иконкам всех контролов Root (Ribbon + ListView + TreeView).</summary>
    public static SettingsCommitScope ForRootControlIcons() =>
        ForFlyoutParameterGroup(RootControlIconRegistry.ControlScopeSubKey, FlyoutParameterGroup.RootControlIcon);

    /// <summary>Dirty только по указанным HKCU\DWM value (точечные индикаторы на detail).</summary>
    public static SettingsCommitScope ForDwmKeys(params string[] valueNames) =>
        new() { Kind = SettingsCommitScopeKind.DwmKeySubset, DwmValueNames = valueNames };

    public static SettingsCommitScope ForDwmKeys(DwmRegistryHive hive, params string[] valueNames) =>
        new() { Kind = SettingsCommitScopeKind.DwmKeySubset, DwmValueNames = valueNames, DwmHive = hive };

    public static SettingsCommitScope ForDwmHive(DwmRegistryHive hive) =>
        new() { Kind = SettingsCommitScopeKind.DwmPlatform, DwmHive = hive };

    public static SettingsCommitScope ForDwmHiveArea(CustomizationAreaId area, DwmRegistryHive hive) =>
        new() { Kind = SettingsCommitScopeKind.Area, Area = area, DwmHive = hive };

    /// <summary>Dirty только по указанным HKCU\BWM value (точечные индикаторы Text и др.).</summary>
    public static SettingsCommitScope ForGlassValues(params string[] valueNames) =>
        new() { Kind = SettingsCommitScopeKind.GlassValueSubset, GlassValueNames = valueNames };

    /// <summary>Dirty только по указанным HKCU\WindowMetrics LOGFONT value.</summary>
    public static SettingsCommitScope ForWindowMetricsKeys(params string[] valueNames) =>
        new() { Kind = SettingsCommitScopeKind.WindowMetricsKeySubset, WindowMetricsValueNames = valueNames };
}
