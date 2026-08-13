using GlassSettings.Core.Customization;
using GlassSettings.Core.Registry;
using GlassSettings.Core.Settings;
using GlassSettings.Services;

namespace GlassSettings.Core.Menu;

/// <summary>Menu root DWORD/Special: NoSystemDropShadow, EnableImmersiveStyle, icon background (BWM Menu CONFIG).</summary>
public static class MenuRootRegistry
{
    public const string MenuScopeSubKey = "Menu";

    public const string NoSystemDropShadowValueName = "NoSystemDropShadow";
    public const string EnableImmersiveStyleValueName = "EnableImmersiveStyle";
    public const string NoModernAppBackgroundColorValueName = "NoModernAppBackgroundColor";
    public const string ColorTreatAsTransparentValueName = "ColorTreatAsTransparent";
    public const string ColorTreatAsTransparentThresholdValueName = "ColorTreatAsTransparentThreshold";

    private static readonly string[] ToggleValueNames =
    [
        NoSystemDropShadowValueName,
        EnableImmersiveStyleValueName,
        NoModernAppBackgroundColorValueName,
    ];

    private static readonly string[] IconBackgroundValueNames =
    [
        ColorTreatAsTransparentValueName,
        ColorTreatAsTransparentThresholdValueName,
    ];

    public static IReadOnlyList<RegistryParameterRef> AllParameterRefs()
    {
        var list = ToggleValueNames
            .Select(name => RegistryParameterRef.Glass(MenuScopeSubKey, name))
            .ToList();
        list.AddRange(IconBackgroundValueNames
            .Select(name => RegistryParameterRef.Glass(MenuScopeSubKey, name)));
        return list;
    }

    public static bool IsIconBackgroundPresent() =>
        GlassRegistry.ReadDword(MenuScopeSubKey, ColorTreatAsTransparentValueName) is not null;

    public static bool IsEnableImmersiveStylePresent() =>
        GlassRegistry.ReadDword(MenuScopeSubKey, EnableImmersiveStyleValueName) is not null;

    public static bool IsAnyTogglePresent() =>
        GlassSectionGate.IsAnyPresent(MenuScopeSubKey, ToggleValueNames);

    public static uint ReadEffectiveDword(string valueName) =>
        GlassRegistry.ReadDwordFb(MenuScopeSubKey, valueName, DefaultDword(valueName));

    public static void WriteDword(string valueName, uint value, bool liveFlyout = false)
    {
        if (ShouldDeleteInsteadOfWrite(valueName, value))
            SettingsSession.Current.DeleteGlassValue(MenuScopeSubKey, valueName, liveFlyout);
        else
            SettingsSession.Current.WriteGlassDword(MenuScopeSubKey, valueName, value, liveFlyout);
    }

    public static void DeleteIconBackground(bool liveFlyout = false)
    {
        foreach (var name in IconBackgroundValueNames)
            SettingsSession.Current.DeleteGlassValue(MenuScopeSubKey, name, liveFlyout);
    }

    public static void WriteIconBackground(uint argb, uint threshold, bool liveFlyout = false)
    {
        SettingsSession.Current.WriteGlassDword(
            MenuScopeSubKey,
            ColorTreatAsTransparentValueName,
            argb,
            liveFlyout);
        if (threshold == DefaultDword(ColorTreatAsTransparentThresholdValueName))
            SettingsSession.Current.DeleteGlassValue(
                MenuScopeSubKey,
                ColorTreatAsTransparentThresholdValueName,
                liveFlyout);
        else
            SettingsSession.Current.WriteGlassDword(
                MenuScopeSubKey,
                ColorTreatAsTransparentThresholdValueName,
                threshold,
                liveFlyout);
    }

    public static uint ReadEffectiveIconColor() =>
        GlassRegistry.ReadDwordFb(MenuScopeSubKey, ColorTreatAsTransparentValueName, 0xFFFFFFFF);

    public static uint ReadEffectiveIconThreshold() =>
        ReadEffectiveDword(ColorTreatAsTransparentThresholdValueName);

    public static bool ShouldDeleteInsteadOfWrite(string valueName, uint value) => valueName switch
    {
        NoSystemDropShadowValueName => value == 0,
        EnableImmersiveStyleValueName => value == 1,
        NoModernAppBackgroundColorValueName => value == 1,
        _ => false,
    };

    public static uint DefaultDword(string valueName) => valueName switch
    {
        NoSystemDropShadowValueName => 0,
        EnableImmersiveStyleValueName => 1,
        NoModernAppBackgroundColorValueName => 1,
        ColorTreatAsTransparentThresholdValueName => 50,
        _ => 0,
    };
}
