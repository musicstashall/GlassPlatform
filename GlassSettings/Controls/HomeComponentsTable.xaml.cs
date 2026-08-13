using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace GlassSettings.Controls;

public sealed partial class HomeComponentsTable : UserControl
{
    public HomeComponentsTable()
    {
        InitializeComponent();
        ApplyLocalizedStrings();
    }

    private void ApplyLocalizedStrings()
    {
        ModuleHeader.Text = GlassSettingsResources.Get("Home_TableModule");
        VersionHeader.Text = GlassSettingsResources.Get("Home_TableVersion");
    }

    public void SetModuleHeader(string text) => ModuleHeader.Text = text;

    public void SetRows(
        IReadOnlyList<(string Name, string Version)> rows,
        string? updateLinkText = null,
        Action? updateClick = null)
    {
        RowsHost.Children.Clear();
        foreach (var (name, version) in rows)
        {
            var isOpenGlass = string.Equals(name, "OpenGlass", StringComparison.OrdinalIgnoreCase);
            RowsHost.Children.Add(CreateRow(
                name,
                version,
                isOpenGlass ? updateLinkText : null,
                isOpenGlass ? updateClick : null));
        }
    }

    private static Grid CreateRow(string name, string version, string? updateLinkText, Action? updateClick)
    {
        var grid = new Grid
        {
            Margin = new Thickness(12, 8, 12, 8),
            ColumnSpacing = 12
        };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

        var nameText = new TextBlock
        {
            Text = name,
            Style = Application.Current.Resources["BodyTextBlockStyle"] as Style,
            TextTrimming = TextTrimming.CharacterEllipsis
        };
        var versionHost = new StackPanel
        {
            HorizontalAlignment = HorizontalAlignment.Right,
            Spacing = 2
        };
        var versionText = new TextBlock
        {
            Text = version,
            MinWidth = 120,
            HorizontalAlignment = HorizontalAlignment.Right,
            Foreground = AppThemeVisualResolver.ResolveThemeBrush("TextFillColorSecondaryBrush"),
            Style = Application.Current.Resources["BodyTextBlockStyle"] as Style,
            TextTrimming = TextTrimming.CharacterEllipsis
        };
        versionHost.Children.Add(versionText);

        if (!string.IsNullOrWhiteSpace(updateLinkText) && updateClick is not null)
        {
            var updateLink = new HyperlinkButton
            {
                Content = updateLinkText,
                FontSize = 12,
                Padding = new Thickness(0),
                HorizontalAlignment = HorizontalAlignment.Right
            };
            updateLink.Click += (_, _) => updateClick();
            versionHost.Children.Add(updateLink);
        }
        Grid.SetColumn(versionHost, 1);

        grid.Children.Add(nameText);
        grid.Children.Add(versionHost);
        return grid;
    }
}
