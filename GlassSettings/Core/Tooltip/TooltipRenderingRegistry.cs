using GlassSettings.Core.Customization;
using GlassSettings.Core.Registry;
using GlassSettings.Core.Settings;
using GlassSettings.Services;

namespace GlassSettings.Core.Tooltip;

/// <summary>Custom rendering tooltip: Text Color и margins (BWM\ToolTip\DarkMode_Color — Core: Tooltip).</summary>
public static class TooltipRenderingRegistry
{
    public const string RegSub = "ToolTip";

    public const string DarkModeColorValueName = "DarkMode_Color";
    public const string LightModeColorValueName = "LightMode_Color";
    public const string MarginsTypeValueName = "MarginsType";
    public const string MarginsLeftValueName = "Margins_cxLeftWidth";
    public const string MarginsRightValueName = "Margins_cxRightWidth";
    public const string MarginsTopValueName = "Margins_cyTopHeight";
    public const string MarginsBottomValueName = "Margins_cyBottomHeight";

    private static readonly string[] AllValueNames =
    [
        DarkModeColorValueName,
        LightModeColorValueName,
        MarginsTypeValueName,
        MarginsLeftValueName,
        MarginsRightValueName,
        MarginsTopValueName,
        MarginsBottomValueName,
    ];

    public static IReadOnlyList<RegistryParameterRef> AllParameterRefs() =>
        AllValueNames.Select(name => RegistryParameterRef.Glass(RegSub, name)).ToList();

    public static bool IsAnyPresent() =>
        GlassSectionGate.IsAnyPresent(RegSub, AllValueNames);

    public static void DeleteAll(bool liveFlyout = false)
    {
        if (!liveFlyout)
        {
            GlassSectionGate.DeleteAll(RegSub, AllValueNames);
            return;
        }

        foreach (var name in AllValueNames)
            SettingsSession.Current.DeleteGlassValue(RegSub, name, liveFlyout: true);
    }

    public static uint ReadEffectiveDword(string valueName) =>
        GlassRegistry.ReadDwordFb(RegSub, valueName, DefaultDword(valueName));

    public static uint ReadEffectiveColor(bool darkMode) =>
        ReadEffectiveDword(darkMode ? DarkModeColorValueName : LightModeColorValueName);

    public static bool ShouldDisableDefaultButton() => !IsAnyPresent();

    public static uint DefaultDword(string valueName) => valueName switch
    {
        DarkModeColorValueName => 0xFFFFFFFF,
        LightModeColorValueName => 0xFF1A1A1A,
        MarginsTypeValueName => 0,
        MarginsLeftValueName => 6,
        MarginsRightValueName => 6,
        MarginsTopValueName => 6,
        MarginsBottomValueName => 6,
        _ => 0,
    };
}
