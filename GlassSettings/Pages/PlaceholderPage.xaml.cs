using GlassSettings.Core.Layout;
using GlassSettings.Strings;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Navigation;

namespace GlassSettings.Pages;

public sealed partial class PlaceholderPage : Page
{
    public PlaceholderPage()
    {
        InitializeComponent();
        SettingsContentColumnLayout.AttachColumn(ContentColumn, ContentScroll);
    }

    protected override void OnNavigatedTo(NavigationEventArgs e)
    {
        base.OnNavigatedTo(e);
        if (e.Parameter is string title)
            SetHeader(title, GlassSettingsResources.Get("Placeholder_Subtitle"));
    }

    public void SetHeader(string title, string subtitle)
    {
        Header.Title = title;
        Header.Subtitle = subtitle;
    }
}
