using GlassSettings.Core.Registry;

namespace GlassSettings.Core.Dwm;

public readonly record struct DwmNavigationContext(DwmRegistryHive Hive, string? RouteId = null)
{
    public static DwmNavigationContext DefaultUser { get; } =
        new(DwmRegistryHive.Hkcu, DwmRouteIds.ForHive(DwmRouteIds.Hub, DwmRegistryHive.Hkcu));

    public static DwmNavigationContext ForHub(DwmRegistryHive hive) =>
        new(hive, DwmRouteIds.ForHive(DwmRouteIds.Hub, hive));

    public static DwmNavigationContext ForDetail(DwmRegistryHive hive, string baseRouteId) =>
        new(hive, DwmRouteIds.ForHive(baseRouteId, hive));

    public static DwmNavigationContext FromNavigationParameter(object? parameter) =>
        parameter switch
        {
            DwmNavigationContext context => context,
            string routeId when routeId.StartsWith(DwmRouteIds.Hub, StringComparison.OrdinalIgnoreCase) =>
                new(
                    DwmRouteIds.IsLocal(routeId) ? DwmRegistryHive.Hklm : DwmRegistryHive.Hkcu,
                    routeId),
            _ => DefaultUser
        };
}
