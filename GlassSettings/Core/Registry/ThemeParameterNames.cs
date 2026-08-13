namespace GlassSettings.Core.Registry;

/// <summary>Имена theme-параметров BWM (оба режима ОС + XAML).</summary>
public static class ThemeParameterNames
{
    public static readonly string[] GradientNames =
    [
        "DarkMode_GradientColor",
        "LightMode_GradientColor"
    ];

    public static readonly string[] BorderNames =
    [
        "DarkMode_BorderColor",
        "LightMode_BorderColor",
        "NoBorderColor"
    ];

    public static readonly string[] GlobalTextGlowNames =
    [
        "DarkMode_GlobalTextGlow",
        "LightMode_GlobalTextGlow"
    ];
}
