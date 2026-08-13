using GlassSettings.Controls;
using GlassSettings.Core.Customization;
using GlassSettings.Core.Snapshot;
using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Pages.Glass;

public sealed partial class GlassSettingsHubPage : Page
{
    private RootFullGlassRow? _fullGlassRow;
    private SettingsNavigateRow? _rootRow;
    private SettingsNavigateRow? _popupRow;
    private SettingsNavigateRow? _taskbarRow;
    private SettingsNavigateRow? _textRow;

    public GlassSettingsHubPage()
    {
        InitializeComponent();

        _fullGlassRow = new RootFullGlassRow();
        _rootRow = NavHost.AddNavigateSection(
            "root",
            GlassSettingsResources.Get("Glass_HubRoot"),
            GlassSettingsResources.Get("Glass_HubRootDesc"),
            onNavigate: _ => OnRootNavigate());
        _rootRow.HeaderActions = _fullGlassRow.FullGlassToggle;
        _rootRow.UnsavedScope = SettingsCommitScope.ForArea(CustomizationAreaId.Root);

        _popupRow = NavHost.AddNavigateSection(
            "popup",
            GlassSettingsResources.Get("Glass_HubPopup"),
            GlassSettingsResources.Get("Glass_HubPopupDesc"),
            onNavigate: h => h.PushPage(typeof(PopupHubPage), null, GlassSettingsResources.Get("Breadcrumb_PopupWindows")));
        _popupRow.UnsavedScope = SettingsCommitScope.ForArea(CustomizationAreaId.Popup);

        _taskbarRow = NavHost.AddNavigateSection(
            "taskbar",
            GlassSettingsResources.Get("Glass_HubTaskbar"),
            GlassSettingsResources.Get("Glass_HubTaskbarDesc"),
            onNavigate: h => h.PushPage(typeof(TaskbarFlyoutPage), null, GlassSettingsResources.Get("Breadcrumb_Taskbar")));
        _taskbarRow.UnsavedScope = SettingsCommitScope.ForArea(CustomizationAreaId.Taskbar);

        _textRow = NavHost.AddNavigateSection(
            "text",
            GlassSettingsResources.Get("Glass_HubText"),
            GlassSettingsResources.Get("Glass_HubTextDesc"),
            onNavigate: _ => App.ShellWindow?.NavigateToTextPage());
        _textRow.UnsavedScope = SettingsCommitScope.ForArea(CustomizationAreaId.Text);

        Loaded += (_, _) => _fullGlassRow!.SyncFromRegistry();
    }

    private void OnRootNavigate()
    {
        NavHost.PushPage(typeof(RootWindowsPage), null, GlassSettingsResources.Get("Breadcrumb_RootSwca"));
    }
}
