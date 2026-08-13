using GlassSettings.Core.Customization;
using GlassSettings.Core.Snapshot;
using GlassSettings.Pages;
using GlassSettings.Pages.Dwm;
using GlassSettings.Pages.Glass;
using GlassSettings.Pages.Text;

namespace GlassSettings.Services;

/// <summary>Scope commit по типу страницы (статический fallback).</summary>
internal static class CommitScopeRegistry
{
    public static SettingsCommitScope? TryGetForPageType(Type pageType) => pageType switch
    {
        _ when pageType == typeof(SystemPage) => null,
        _ when pageType == typeof(ProcessesPage) => null,
        _ when pageType == typeof(RootWindowsPage) => SettingsCommitScope.ForArea(CustomizationAreaId.Root),
        _ when pageType == typeof(PopupHubPage) => SettingsCommitScope.ForArea(CustomizationAreaId.Popup),
        _ when pageType == typeof(TaskbarFlyoutPage) => TaskbarScopeCatalog.BarPageScope,
        _ when pageType == typeof(TaskbarAdjacentWindowsPage) => TaskbarScopeCatalog.AdjacentPageScope,
        // Hub — навигация + точки на строках; жёлтый session notice — на detail (Root/Popup/Taskbar/Text).
        _ when pageType == typeof(GlassSettingsHubPage) => null,
        _ when pageType == typeof(DwmColorizationDetailPage) => SettingsCommitScope.ForArea(CustomizationAreaId.DwmColorization),
        _ when pageType == typeof(DwmGlassDetailPage) => SettingsCommitScope.ForArea(CustomizationAreaId.DwmGlass),
        _ when pageType == typeof(DwmThemeDetailPage) => SettingsCommitScope.ForArea(CustomizationAreaId.DwmTheme),
        _ when pageType == typeof(DwmSettingsHubPage) => SettingsCommitScope.DwmPlatform,
        _ when pageType == typeof(TextAndFontsPage) => SettingsCommitScope.ForArea(CustomizationAreaId.Text),
        _ => null
    };
}
