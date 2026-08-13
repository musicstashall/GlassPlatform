using GlassSettings.Core.Dwm;
using GlassSettings.Core.Flyout;
using GlassSettings.Strings;

namespace GlassSettings.Core.Customization;

internal static class CustomizationValueFormatter
{
    public static string Format(RegistryParameterRef parameter, uint value) => parameter.ValueName switch
    {
        "EffectType" => parameter.GlassSubKey?.Contains("Xaml", StringComparison.OrdinalIgnoreCase) == true
            ? FlyoutXamlEffectNames.Get(value)
            : FlyoutEffectNames.Get(value),
        "BorderDrawMode" => value == 0
            ? GlassSettingsResources.Get("Param_ModeDwmSwca")
            : GlassSettingsResources.Get("Param_ModeXaml"),
        "SeparateZones" => value != 0
            ? GlassSettingsResources.Get("Custom_On")
            : GlassSettingsResources.Get("Custom_Off"),
        "NoBackdrop" => value == 0
            ? GlassSettingsResources.Get("Root_FullGlassToggle")
            : GlassSettingsResources.Get("Custom_Off"),
        "BlurDeviation" => value.ToString(),
        "BlurOptimization" => value switch
        {
            0 => GlassSettingsResources.Get("Param_BlurSpeed"),
            1 => GlassSettingsResources.Get("Param_BlurBalance"),
            2 => GlassSettingsResources.Get("Param_BlurQuality"),
            _ => value.ToString()
        },
        "ColorizationOpaqueBlendPriority" =>
            DwmDefaults.IsOpacityAutoValue(value)
                ? GlassSettingsResources.Get("Param_Auto")
                : value.ToString(),
        var name when name.Contains("Opacity", StringComparison.Ordinal) =>
            DwmDefaults.IsOpacityAutoValue(value)
                ? GlassSettingsResources.Get("Param_Auto")
                : value.ToString(),
        "CornerType" => FlyoutCornerNames.Get(
            value,
            parameter.GlassSubKey?.Contains("Xaml", StringComparison.OrdinalIgnoreCase) == true),
        "EnableDropShadow" => value != 0
            ? GlassSettingsResources.Get("Custom_On")
            : GlassSettingsResources.Get("Custom_Off"),
        "NoBorderColor" => value != 0
            ? GlassSettingsResources.Get("Custom_NoBorder")
            : GlassSettingsResources.Get("Custom_WithBorder"),
        _ => value.ToString()
    };
}
