using GlassSettings.Core.Registry;

namespace GlassSettings.Core.Dwm;

public static class DwmRouteIds
{
    public const string Hub = "dwm";
    public const string Colorization = "colorization";
    public const string Glass = "glass";
    public const string Theme = "theme";

    public static string ForHive(string baseRouteId, DwmRegistryHive hive) =>
        hive == DwmRegistryHive.Hklm
            ? baseRouteId + "L"
            : baseRouteId;

    public static bool IsLocal(string? routeId) =>
        !string.IsNullOrWhiteSpace(routeId)
        && routeId.EndsWith("L", StringComparison.OrdinalIgnoreCase);
}
