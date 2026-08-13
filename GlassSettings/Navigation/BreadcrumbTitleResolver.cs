using GlassSettings.Core.Dwm;
using GlassSettings.Core.Registry;
using GlassSettings.Pages;
using GlassSettings.Pages.Dwm;
using GlassSettings.Pages.Glass;
using GlassSettings.Pages.Text;
using GlassSettings.Strings;

namespace GlassSettings.Navigation;

/// <summary>Заголовки breadcrumb по типу страницы и параметру навигации.</summary>
public static class BreadcrumbTitleResolver
{
    public static string Resolve(Type pageType, object? parameter, string? fallback = null)
    {
        if (parameter is string tag && TryResolveRootTag(pageType, tag, out var rootTitle))
            return AppendDwmHiveSuffixIfNeeded(pageType, rootTitle, parameter);

        var title = pageType.Name switch
        {
            nameof(HomePage) => GlassSettingsResources.Get("NavHome"),
            nameof(SystemPage) => GlassSettingsResources.Get("NavSystem"),
            nameof(DwmSettingsHubPage) => GlassSettingsResources.Get("Breadcrumb_DwmSettings"),
            nameof(GlassSettingsHubPage) => GlassSettingsResources.Get("Breadcrumb_GlassSettings"),
            nameof(ProcessesPage) => GlassSettingsResources.Get("NavAttach"),
            nameof(VisualStylesPage) => GlassSettingsResources.Get("NavThemes"),
            nameof(ThemesColorsPage) => GlassSettingsResources.Get("Breadcrumb_ThemesColors"),
            nameof(ThemesDesktopBackgroundPage) => GlassSettingsResources.Get("Breadcrumb_ThemesDesktopBackground"),
            nameof(TextAndFontsPage) => GlassSettingsResources.Get("NavText"),
            nameof(PopupHubPage) => GlassSettingsResources.Get("Breadcrumb_PopupWindows"),
            nameof(TaskbarFlyoutPage) => GlassSettingsResources.Get("Breadcrumb_Taskbar"),
            nameof(TaskbarAdjacentWindowsPage) => GlassSettingsResources.Get("Breadcrumb_AdjacentWindows"),
            nameof(RootWindowsPage) => GlassSettingsResources.Get("Breadcrumb_RootSwca"),
            nameof(DwmColorizationDetailPage) => GlassSettingsResources.Get("Breadcrumb_Colorization"),
            nameof(DwmGlassDetailPage) => GlassSettingsResources.Get("Breadcrumb_DwmGlass"),
            nameof(DwmThemeDetailPage) => GlassSettingsResources.Get("Breadcrumb_DwmTheme"),
            _ => fallback ?? pageType.Name
        };

        return AppendDwmHiveSuffixIfNeeded(pageType, title, parameter);
    }

    private static bool TryResolveRootTag(Type pageType, string tag, out string title)
    {
        var resolved = tag switch
        {
            "home" when pageType == typeof(HomePage) => GlassSettingsResources.Get("NavHome"),
            "system" when pageType == typeof(SystemPage) => GlassSettingsResources.Get("NavSystem"),
            "dwm" or "dwml" when pageType == typeof(DwmSettingsHubPage) => GlassSettingsResources.Get("Breadcrumb_DwmSettings"),
            "glass" when pageType == typeof(GlassSettingsHubPage) => GlassSettingsResources.Get("Breadcrumb_GlassSettings"),
            "attach" when pageType == typeof(ProcessesPage) => GlassSettingsResources.Get("NavAttach"),
            "themes" when pageType == typeof(VisualStylesPage) => GlassSettingsResources.Get("NavThemes"),
            "text" when pageType == typeof(TextAndFontsPage) => GlassSettingsResources.Get("NavText"),
            _ => (string?)null
        };

        if (resolved is null)
        {
            title = string.Empty;
            return false;
        }

        title = resolved;
        return true;
    }

    private static string AppendDwmHiveSuffixIfNeeded(Type pageType, string title, object? parameter)
    {
        if (!IsDwmRoute(pageType))
            return title;

        var context = DwmNavigationContext.FromNavigationParameter(parameter);
        var localSuffix = GlassSettingsResources.Get("Breadcrumb_DwmLocalSuffix");
        var userSuffix = GlassSettingsResources.Get("Breadcrumb_DwmUserSuffix");
        var suffix = context.Hive == DwmRegistryHive.Hklm
            ? localSuffix
            : userSuffix;

        if (string.IsNullOrEmpty(suffix))
            return title;

        if ((!string.IsNullOrEmpty(localSuffix) && title.EndsWith(localSuffix, StringComparison.Ordinal))
            || (!string.IsNullOrEmpty(userSuffix) && title.EndsWith(userSuffix, StringComparison.Ordinal)))
            return title;

        return title + suffix;
    }

    private static bool IsDwmRoute(Type pageType) =>
        pageType == typeof(DwmSettingsHubPage)
        || pageType == typeof(DwmColorizationDetailPage)
        || pageType == typeof(DwmGlassDetailPage)
        || pageType == typeof(DwmThemeDetailPage);
}
