using GlassSettings.Strings;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Services;

/// <summary>Диалог с информацией о поддержке проекта Glass.</summary>
public static class DonationDialogService
{
    public static async Task ShowAsync(XamlRoot xamlRoot)
    {
        var body = new StackPanel { Spacing = 12 };

        body.Children.Add(MakeParagraph(GlassSettingsResources.Get("Dialog_DonationBody1")));
        body.Children.Add(MakeParagraph(GlassSettingsResources.Get("Dialog_DonationBody2")));
        body.Children.Add(MakeParagraph(GlassSettingsResources.Get("Dialog_DonationBody3")));
        body.Children.Add(MakeParagraph(GlassSettingsResources.Get("Dialog_DonationThanks")));

        var scroll = new ScrollViewer
        {
            MaxHeight = 360,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            Content = body
        };

        var dialog = new ContentDialog
        {
            Title = GlassSettingsResources.Get("Dialog_DonationTitle"),
            Content = scroll,
            PrimaryButtonText = GlassSettingsResources.Get("Dialog_Understood"),
            DefaultButton = ContentDialogButton.Primary,
            XamlRoot = xamlRoot
        };

        _ = await dialog.ShowAsync();
    }

    private static TextBlock MakeParagraph(string text) => new()
    {
        Text = text,
        TextWrapping = TextWrapping.Wrap,
        Style = Application.Current.Resources["BodyTextBlockStyle"] as Style
    };
}
