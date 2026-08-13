namespace GlassSettings.Core.Snapshot;

using GlassSettings.Core.Registry;

/// <summary>HKLM-only системные ключи OpenGlass (вкладка System донора).</summary>
public static class DwmSystemScopeCatalog
{
    public static readonly string[] HookKeys = ["DisabledHooks"];

    public static readonly string[] HeaderKeys =
    [
        "DisableGlassOnBattery",
        "GlassSafetyZoneMode"
    ];

    public static readonly string[] AllKeys =
    [
        ..HeaderKeys,
        ..HookKeys
    ];

    public static SettingsCommitScope HookScope(DwmRegistryHive hive = DwmRegistryHive.Hklm) =>
        SettingsCommitScope.ForDwmKeys(hive, HookKeys);

    public static SettingsCommitScope AllScope(DwmRegistryHive hive = DwmRegistryHive.Hklm) =>
        SettingsCommitScope.ForDwmKeys(hive, AllKeys);
}
