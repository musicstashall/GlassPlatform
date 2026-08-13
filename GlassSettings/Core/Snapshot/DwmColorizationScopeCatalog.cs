namespace GlassSettings.Core.Snapshot;

using GlassSettings.Core.Registry;

/// <summary>Группы DWM-ключей Colorization для точечных индикаторов на detail-странице.</summary>
public static class DwmColorizationScopeCatalog
{
    /// <summary>Ключи верхней карточки «Композиция» (accent / opaque). BlurDeviation — в ActiveColor*.</summary>
    public static readonly string[] CompositionCardKeys =
    [
        "GlassOverrideAccent",
        "ColorizationOpaqueBlend"
    ];

    public static readonly string[] ActiveColorVistaKeys =
    [
        "ColorizationColor",
        "ColorizationColorOverride",
        "GlassOpacity",
        "BlurDeviation"
    ];

    public static readonly string[] ActiveColorWin7Keys =
    [
        "ColorizationColor",
        "ColorizationColorOverride",
        "ColorizationColorBalance",
        "ColorizationColorBalanceOverride",
        "ColorizationBlurBalance",
        "ColorizationBlurBalanceOverride",
        "BlurDeviation"
    ];

    public static readonly string[] SecondaryColorVistaKeys =
    [
        "ColorizationColorInactive",
        "GlassOpacityInactive"
    ];

    public static readonly string[] SecondaryColorWin7Keys =
    [
        "ColorizationAfterglow",
        "ColorizationAfterglowOverride",
        "ColorizationAfterglowBalance",
        "ColorizationAfterglowBalanceOverride"
    ];

    private static readonly string[] BaseColorTabKeys =
    [
        "ColorizationBaseTransparent",
        "ColorizationBaseMaximized",
        "ColorizationBaseOpaque"
    ];

    public static readonly string[] PriorityKeys = ["ColorizationOpaqueBlendPriority"];

    public static readonly string[] BaseColorKeys =
    [
        "ColorizationBaseTransparent",
        "ColorizationBaseMaximized",
        "ColorizationBaseOpaque"
    ];

    public static readonly string[] BaseOpacityKeys =
    [
        "ColorizationOpacity",
        "ColorizationOpacityInactive",
        "ColorizationOpacityMaximized",
        "ColorizationOpacityInactiveMaximized"
    ];

    public static SettingsCommitScope GlassTypeScope(DwmRegistryHive? hive = null) =>
        hive is { } selectedHive
            ? SettingsCommitScope.ForDwmKeys(selectedHive, "GlassType")
            : SettingsCommitScope.ForDwmKeys("GlassType");

    public static SettingsCommitScope CompositionCardScope(DwmRegistryHive? hive = null) =>
        hive is { } selectedHive
            ? SettingsCommitScope.ForDwmKeys(selectedHive, CompositionCardKeys)
            : SettingsCommitScope.ForDwmKeys(CompositionCardKeys);

    public static SettingsCommitScope ActiveColorVistaScope(DwmRegistryHive? hive = null) =>
        hive is { } selectedHive
            ? SettingsCommitScope.ForDwmKeys(selectedHive, ActiveColorVistaKeys)
            : SettingsCommitScope.ForDwmKeys(ActiveColorVistaKeys);

    public static SettingsCommitScope ActiveColorWin7Scope(DwmRegistryHive? hive = null) =>
        hive is { } selectedHive
            ? SettingsCommitScope.ForDwmKeys(selectedHive, ActiveColorWin7Keys)
            : SettingsCommitScope.ForDwmKeys(ActiveColorWin7Keys);

    public static SettingsCommitScope SecondaryColorVistaScope(DwmRegistryHive? hive = null) =>
        hive is { } selectedHive
            ? SettingsCommitScope.ForDwmKeys(selectedHive, SecondaryColorVistaKeys)
            : SettingsCommitScope.ForDwmKeys(SecondaryColorVistaKeys);

    public static SettingsCommitScope SecondaryColorWin7Scope(DwmRegistryHive? hive = null) =>
        hive is { } selectedHive
            ? SettingsCommitScope.ForDwmKeys(selectedHive, SecondaryColorWin7Keys)
            : SettingsCommitScope.ForDwmKeys(SecondaryColorWin7Keys);

    public static SettingsCommitScope BaseColorTabScope(int tabIndex, DwmRegistryHive? hive = null)
    {
        if (tabIndex < 0 || tabIndex >= BaseColorTabKeys.Length)
            return hive is { } selectedHive
                ? SettingsCommitScope.ForDwmKeys(selectedHive, Array.Empty<string>())
                : SettingsCommitScope.ForDwmKeys(Array.Empty<string>());

        return hive is { } selected
            ? SettingsCommitScope.ForDwmKeys(selected, BaseColorTabKeys[tabIndex])
            : SettingsCommitScope.ForDwmKeys(BaseColorTabKeys[tabIndex]);
    }

    public static SettingsCommitScope BaseOpacityTabScope(int tabIndex, DwmRegistryHive? hive = null) => tabIndex switch
    {
        0 => hive is { } selectedHive
            ? SettingsCommitScope.ForDwmKeys(
                selectedHive,
                "ColorizationOpacity",
                "ColorizationOpacityInactive")
            : SettingsCommitScope.ForDwmKeys(
            "ColorizationOpacity",
            "ColorizationOpacityInactive"),
        1 => hive is { } selected
            ? SettingsCommitScope.ForDwmKeys(
                selected,
                "ColorizationOpacityMaximized",
                "ColorizationOpacityInactiveMaximized")
            : SettingsCommitScope.ForDwmKeys(
            "ColorizationOpacityMaximized",
            "ColorizationOpacityInactiveMaximized"),
        _ => hive is { } selected
            ? SettingsCommitScope.ForDwmKeys(selected, Array.Empty<string>())
            : SettingsCommitScope.ForDwmKeys(Array.Empty<string>())
    };

    public static SettingsCommitScope PriorityScope(DwmRegistryHive? hive = null) =>
        hive is { } selectedHive
            ? SettingsCommitScope.ForDwmKeys(selectedHive, PriorityKeys)
            : SettingsCommitScope.ForDwmKeys(PriorityKeys);

    public static SettingsCommitScope BaseColorScope(DwmRegistryHive? hive = null) =>
        hive is { } selectedHive
            ? SettingsCommitScope.ForDwmKeys(selectedHive, BaseColorKeys)
            : SettingsCommitScope.ForDwmKeys(BaseColorKeys);

    public static SettingsCommitScope BaseOpacityScope(DwmRegistryHive? hive = null) =>
        hive is { } selectedHive
            ? SettingsCommitScope.ForDwmKeys(selectedHive, BaseOpacityKeys)
            : SettingsCommitScope.ForDwmKeys(BaseOpacityKeys);

    /// <summary>Dirty внутри секции «Дополнительно» на detail Colorization.</summary>
    public static SettingsCommitScope AdvancedScope(DwmRegistryHive? hive = null)
    {
        var keys = new string[PriorityKeys.Length + BaseColorKeys.Length + BaseOpacityKeys.Length];
        PriorityKeys.CopyTo(keys, 0);
        BaseColorKeys.CopyTo(keys, PriorityKeys.Length);
        BaseOpacityKeys.CopyTo(keys, PriorityKeys.Length + BaseColorKeys.Length);
        return hive is { } selectedHive
            ? SettingsCommitScope.ForDwmKeys(selectedHive, keys)
            : SettingsCommitScope.ForDwmKeys(keys);
    }
}
