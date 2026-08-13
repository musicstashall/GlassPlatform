using GlassSettings.Core.Dwm;
using GlassSettings.Core.Registry;

namespace GlassSettings.Navigation;

public enum HomeNavKind
{
    SectionRoot,
    GlassDrill,
    DwmDrill,
    RootWindows
}

public sealed class HomeNavTarget
{
    public required HomeNavKind Kind { get; init; }
    public string? SectionTag { get; init; }
    public Type? PageType { get; init; }
    public object? Parameter { get; init; }
    public string? BreadcrumbTitle { get; init; }
    public string? SidebarTag { get; init; }

    public static HomeNavTarget Section(string tag) => new()
    {
        Kind = HomeNavKind.SectionRoot,
        SectionTag = tag,
        SidebarTag = tag
    };

    public static HomeNavTarget GlassPage(Type pageType, object? parameter, string breadcrumbTitle) => new()
    {
        Kind = HomeNavKind.GlassDrill,
        PageType = pageType,
        Parameter = parameter,
        BreadcrumbTitle = breadcrumbTitle,
        SidebarTag = "glass"
    };

    public static HomeNavTarget DwmPage(Type pageType, object? parameter, string breadcrumbTitle) => new()
    {
        Kind = HomeNavKind.DwmDrill,
        PageType = pageType,
        Parameter = parameter,
        BreadcrumbTitle = breadcrumbTitle,
        SidebarTag = "dwm"
    };

    public static HomeNavTarget DwmPage(
        Type pageType,
        DwmRegistryHive hive,
        string baseRouteId,
        string breadcrumbTitle) => new()
    {
        Kind = HomeNavKind.DwmDrill,
        PageType = pageType,
        Parameter = DwmNavigationContext.ForDetail(hive, baseRouteId),
        BreadcrumbTitle = breadcrumbTitle,
        SidebarTag = "dwm"
    };

    public static HomeNavTarget RootWindows() => new()
    {
        Kind = HomeNavKind.RootWindows,
        SidebarTag = "glass"
    };
}
