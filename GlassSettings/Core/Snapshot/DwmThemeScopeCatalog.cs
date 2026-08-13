namespace GlassSettings.Core.Snapshot;

using GlassSettings.Core.Registry;

/// <summary>Группы DWM-ключей Theme для точечных индикаторов на detail-страницах.</summary>
public static class DwmThemeScopeCatalog
{
    public static readonly string[] AtlasKeys = ["CustomThemeAtlas"];

    public static readonly string[] ReflectionCollectionKeys = ["CustomThemeReflection"];

    public static readonly string[] MaterialCollectionKeys = ["CustomThemeMaterial"];

    public static readonly string[] ReflectionOpacityKeys =
    [
        "ColorizationGlassReflectionOpacity",
        "ColorizationGlassReflectionOpacityInactive"
    ];

    public static readonly string[] ReflectionMaxOpacityKeys =
    [
        "ColorizationGlassReflectionOpacityMaximized",
        "ColorizationGlassReflectionOpacityInactiveMaximized"
    ];

    /// <summary>Ключи страницы «Текстуры» — для строки «Параметры текстуры стекла» на hub Theme.</summary>
    public static readonly string[] IntensityKeys = ["ColorizationGlassReflectionIntensity"];

    public static readonly string[] MaterialOpacityKeys = ["MaterialOpacity"];

    public static readonly string[] ParallaxKeys = ["ColorizationGlassReflectionParallaxIntensity"];

    public static readonly string[] PolicyKeys = ["ColorizationGlassReflectionPolicy"];

    public static readonly string[] TexturesNavKeys =
    [
        "CustomThemeReflection",
        "CustomThemeMaterial",
        "MaterialOpacity",
        "ColorizationGlassReflectionOpacity",
        "ColorizationGlassReflectionOpacityMaximized",
        "ColorizationGlassReflectionOpacityInactive",
        "ColorizationGlassReflectionOpacityInactiveMaximized",
        "ColorizationGlassReflectionIntensity",
        "ColorizationGlassReflectionParallaxIntensity",
        "ColorizationGlassReflectionPolicy"
    ];

    public static SettingsCommitScope AtlasScope(DwmRegistryHive? hive = null) =>
        hive is { } selectedHive
            ? SettingsCommitScope.ForDwmKeys(selectedHive, AtlasKeys)
            : SettingsCommitScope.ForDwmKeys(AtlasKeys);

    public static SettingsCommitScope ReflectionCollectionScope(DwmRegistryHive? hive = null) =>
        hive is { } selectedHive
            ? SettingsCommitScope.ForDwmKeys(selectedHive, ReflectionCollectionKeys)
            : SettingsCommitScope.ForDwmKeys(ReflectionCollectionKeys);

    public static SettingsCommitScope MaterialCollectionScope(DwmRegistryHive? hive = null) =>
        hive is { } selectedHive
            ? SettingsCommitScope.ForDwmKeys(selectedHive, MaterialCollectionKeys)
            : SettingsCommitScope.ForDwmKeys(MaterialCollectionKeys);

    public static SettingsCommitScope ReflectionOpacityScope(DwmRegistryHive? hive = null) =>
        hive is { } selectedHive
            ? SettingsCommitScope.ForDwmKeys(selectedHive, ReflectionOpacityKeys)
            : SettingsCommitScope.ForDwmKeys(ReflectionOpacityKeys);

    public static SettingsCommitScope ReflectionMaxOpacityScope(DwmRegistryHive? hive = null) =>
        hive is { } selectedHive
            ? SettingsCommitScope.ForDwmKeys(selectedHive, ReflectionMaxOpacityKeys)
            : SettingsCommitScope.ForDwmKeys(ReflectionMaxOpacityKeys);

    public static SettingsCommitScope IntensityScope(DwmRegistryHive? hive = null) =>
        hive is { } selectedHive
            ? SettingsCommitScope.ForDwmKeys(selectedHive, IntensityKeys)
            : SettingsCommitScope.ForDwmKeys(IntensityKeys);

    public static SettingsCommitScope MaterialOpacityScope(DwmRegistryHive? hive = null) =>
        hive is { } selectedHive
            ? SettingsCommitScope.ForDwmKeys(selectedHive, MaterialOpacityKeys)
            : SettingsCommitScope.ForDwmKeys(MaterialOpacityKeys);

    public static SettingsCommitScope ParallaxScope(DwmRegistryHive? hive = null) =>
        hive is { } selectedHive
            ? SettingsCommitScope.ForDwmKeys(selectedHive, ParallaxKeys)
            : SettingsCommitScope.ForDwmKeys(ParallaxKeys);

    public static SettingsCommitScope PolicyScope(DwmRegistryHive? hive = null) =>
        hive is { } selectedHive
            ? SettingsCommitScope.ForDwmKeys(selectedHive, PolicyKeys)
            : SettingsCommitScope.ForDwmKeys(PolicyKeys);

    public static SettingsCommitScope TexturesNavScope(DwmRegistryHive? hive = null) =>
        hive is { } selectedHive
            ? SettingsCommitScope.ForDwmKeys(selectedHive, TexturesNavKeys)
            : SettingsCommitScope.ForDwmKeys(TexturesNavKeys);

    /// <summary>Dirty внутри секции «Дополнительно» на detail Theme (opacity reflection).</summary>
    public static SettingsCommitScope AdvancedScope(DwmRegistryHive? hive = null)
    {
        var keys = new string[ReflectionOpacityKeys.Length + ReflectionMaxOpacityKeys.Length];
        ReflectionOpacityKeys.CopyTo(keys, 0);
        ReflectionMaxOpacityKeys.CopyTo(keys, ReflectionOpacityKeys.Length);
        return hive is { } selectedHive
            ? SettingsCommitScope.ForDwmKeys(selectedHive, keys)
            : SettingsCommitScope.ForDwmKeys(keys);
    }
}
