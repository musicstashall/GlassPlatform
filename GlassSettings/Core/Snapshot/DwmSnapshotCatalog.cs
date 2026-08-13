namespace GlassSettings.Core.Snapshot;

/// <summary>DWM-ключи для UI-партиций commit scope. Capture HKCU/HKLM\DWM — full enum минус <see cref="DwmSnapshotExclusions"/>.</summary>
public enum DwmSnapshotPartition
{
    Colorization,
    Glass,
    Theme,
    System
}

public static class DwmSnapshotCatalog
{
    private static readonly string[] ColorizationKeys =
    [
        "ColorizationColor",
        "ColorizationColorOverride", "ColorizationColorInactive",
        "ColorizationAfterglow", "ColorizationAfterglowOverride",
        "GlassType",
        "GlassOverrideAccent",
        "BlurDeviation",
        "GlassOpacity", "GlassOpacityInactive",
        "ColorizationOpacity", "ColorizationOpacityMaximized",
        "ColorizationOpacityInactive", "ColorizationOpacityInactiveMaximized",
        "ColorizationOpaqueBlend", "ColorizationOpaqueBlendPriority",
        "ColorizationBlurBalance", "ColorizationBlurBalanceOverride",
        "ColorizationAfterglowBalance", "ColorizationAfterglowBalanceOverride",
        "ColorizationColorBalance", "ColorizationColorBalanceOverride",
        "ColorizationBaseTransparent", "ColorizationBaseMaximized", "ColorizationBaseOpaque"
    ];

    private static readonly string[] GlassKeys =
    [
        "BlurOptimization", "RoundRectRadius", "UseDirect3DRendering",
        "CaptionButtons", "CenterCaption", "TextGlowMode", "DisableModernBorders",
        "ColorizationColorCaption", "ColorizationColorCaptionMaximized",
        "ColorizationColorCaptionInactive", "ColorizationColorCaptionInactiveMaximized"
    ];

    private static readonly string[] ThemeKeys =
    [
        "ColorizationGlassReflectionOpacity", "ColorizationGlassReflectionOpacityMaximized",
        "ColorizationGlassReflectionOpacityInactive", "ColorizationGlassReflectionOpacityInactiveMaximized",
        "ColorizationGlassReflectionIntensity", "ColorizationGlassReflectionParallaxIntensity",
        "ColorizationGlassReflectionPolicy", "CustomThemeReflection", "CustomThemeMaterial",
        "MaterialOpacity", "CustomThemeAtlas", "AtlasThemeImage"
    ];

    private static readonly string[] SystemKeys = DwmSystemScopeCatalog.AllKeys;

    public static IReadOnlyList<string> GetKeys(DwmSnapshotPartition partition) => partition switch
    {
        DwmSnapshotPartition.Colorization => ColorizationKeys,
        DwmSnapshotPartition.Glass => GlassKeys,
        DwmSnapshotPartition.Theme => ThemeKeys,
        DwmSnapshotPartition.System => SystemKeys,
        _ => []
    };

    public static IReadOnlyList<string> AllKeys { get; } =
        ColorizationKeys.Concat(GlassKeys).Concat(ThemeKeys).Concat(SystemKeys).ToArray();

    public static IEnumerable<DwmSnapshotPartition> AllPartitions =>
        [DwmSnapshotPartition.Colorization, DwmSnapshotPartition.Glass, DwmSnapshotPartition.Theme, DwmSnapshotPartition.System];
}
