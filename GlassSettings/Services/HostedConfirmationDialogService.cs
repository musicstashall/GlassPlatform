using GlassSettings.Strings;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Services;

/// <summary>Общий паттерн hosted-диалогов подтверждения (штора + панель + footer).</summary>
public static class HostedConfirmationDialogService
{
    public static async Task<bool> ConfirmAsync(HostedConfirmationSpec spec)
    {
        var result = await HostedDialogService.ShowAsync(ToDialogSpec(spec)).ConfigureAwait(true);
        return result == HostedDialogResult.Primary;
    }

    public static Task<bool> ConfirmThenAsync(HostedConfirmationSpec spec, Func<Task> procedure) =>
        HostedDialogService.ConfirmThenAsync(ToDialogSpec(spec), procedure);

    public static TextBlock CreateBody(string text) =>
        new()
        {
            Text = text,
            TextWrapping = TextWrapping.WrapWholeWords,
            Style = Application.Current.Resources["BodyTextBlockStyle"] as Style
        };

    public static HostedDialogFooterSpec CreateFooter(
        string primaryButtonText,
        string? secondaryButtonText = null,
        double primaryButtonWidth = 108,
        double secondaryButtonWidth = 108,
        double buttonSpacing = 16) =>
        new()
        {
            SecondaryButtonText = secondaryButtonText,
            SecondaryButtonWidth = secondaryButtonWidth,
            PrimaryButtonText = primaryButtonText,
            PrimaryButtonWidth = primaryButtonWidth,
            ButtonSpacing = buttonSpacing
        };

    public static HostedDialogFooterSpec CreateCancelConfirmFooter(
        string confirmButtonText,
        double confirmButtonWidth = 108) =>
        CreateFooter(
            confirmButtonText,
            GlassSettingsResources.Get("Dialog_Cancel"),
            confirmButtonWidth);

    private static HostedDialogSpec ToDialogSpec(HostedConfirmationSpec spec) =>
        new()
        {
            Title = spec.Title,
            Body = HostedDialogLayout.WrapBodyWithContentPadding(spec.Body),
            MinWidth = spec.MinWidth,
            MaxWidth = spec.MaxWidth,
            ManageOverlay = spec.ManageOverlay,
            Footer = spec.Footer
        };
}

/// <summary>Спецификация hosted-диалога подтверждения.</summary>
public sealed class HostedConfirmationSpec
{
    public required string Title { get; init; }

    public required UIElement Body { get; init; }

    public required HostedDialogFooterSpec Footer { get; init; }

    public double MinWidth { get; init; } = 420;

    public double MaxWidth { get; init; } = 480;

    public bool ManageOverlay { get; init; } = true;
}
