namespace GlassSettings.Core.Platform;

/// <summary>Маршрутизация live-reload по типу DWM-ключа. См. GlassSettings/docs/GlassSettings-host-glass.md.</summary>
internal static class DwmLiveReloadKeys
{
    public const string CustomThemeReflection = "CustomThemeReflection";
    public const string CustomThemeMaterial = "CustomThemeMaterial";
    public const string CustomThemeAtlas = "CustomThemeAtlas";

    public static bool IsThemeImage(string valueName) =>
        string.Equals(valueName, CustomThemeReflection, StringComparison.OrdinalIgnoreCase)
        || string.Equals(valueName, CustomThemeMaterial, StringComparison.OrdinalIgnoreCase)
        || string.Equals(valueName, CustomThemeAtlas, StringComparison.OrdinalIgnoreCase);
}
