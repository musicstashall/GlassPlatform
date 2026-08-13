namespace GlassSettings.Core.Snapshot;

using GlassSettings.Core.Registry;

/// <summary>Группы DWM-ключей Glass («Внешний вид») для точечных индикаторов на detail-странице.</summary>
public static class DwmGlassScopeCatalog
{
    public static readonly string[] GeneralKeys =
    [
        "BlurOptimization",
        "UseDirect3DRendering"
    ];

    public static readonly string[] WindowKeys =
    [
        "RoundRectRadius",
        "CaptionButtons",
        "DisableModernBorders"
    ];

    public static readonly string[] CaptionStyleKeys =
    [
        "CenterCaption",
        "TextGlowMode"
    ];

    public static readonly string[] CaptionColorNormalKeys =
    [
        "ColorizationColorCaption",
        "ColorizationColorCaptionInactive"
    ];

    public static readonly string[] CaptionColorMaximizedKeys =
    [
        "ColorizationColorCaptionMaximized",
        "ColorizationColorCaptionInactiveMaximized"
    ];

    public static readonly string[] CaptionColorActiveKeys = ["ColorizationColorCaption"];

    public static readonly string[] CaptionColorInactiveKeys = ["ColorizationColorCaptionInactive"];

    public static readonly string[] CaptionColorActiveMaximizedKeys = ["ColorizationColorCaptionMaximized"];

    public static readonly string[] CaptionColorInactiveMaximizedKeys = ["ColorizationColorCaptionInactiveMaximized"];

    public static SettingsCommitScope GeneralScope(DwmRegistryHive? hive = null) =>
        ScopeFor(GeneralKeys, hive);

    public static SettingsCommitScope WindowScope(DwmRegistryHive? hive = null) =>
        ScopeFor(WindowKeys, hive);

    public static SettingsCommitScope CaptionStyleScope(DwmRegistryHive? hive = null) =>
        ScopeFor(CaptionStyleKeys, hive);

    public static SettingsCommitScope CaptionColorNormalScope(DwmRegistryHive? hive = null) =>
        ScopeFor(CaptionColorNormalKeys, hive);

    public static SettingsCommitScope CaptionColorMaximizedScope(DwmRegistryHive? hive = null) =>
        ScopeFor(CaptionColorMaximizedKeys, hive);

    public static SettingsCommitScope ForKeys(string[] keys, DwmRegistryHive? hive = null) =>
        ScopeFor(keys, hive);

    private static SettingsCommitScope ScopeFor(string[] keys, DwmRegistryHive? hive) =>
        hive is { } selectedHive
            ? SettingsCommitScope.ForDwmKeys(selectedHive, keys)
            : SettingsCommitScope.ForDwmKeys(keys);
}
