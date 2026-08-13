using GlassSettings.Controls.Themes;
using GlassSettings.Strings;
using Microsoft.UI.Dispatching;

namespace GlassSettings.Services;

/// <summary>Hosted-диалог выбора картинок альбома слайдшоу (Save / отмена со снимком).</summary>
public static class SlideshowAlbumPickerDialogService
{
    public static async Task ShowAsync(
        DispatcherQueue dispatcherQueue,
        SlideshowAlbumKind albumKind,
        string albumRoot,
        string albumDisplayName)
    {
        if (dispatcherQueue is null
            || albumKind == SlideshowAlbumKind.None
            || string.IsNullOrWhiteSpace(albumRoot))
        {
            return;
        }

        var hasSnapshot = DesktopBackgroundService.TryCaptureSlideshowEditSnapshot(out var snapshot);

        // Как ExecutableFilePickerDialogService: внешняя штора + ManageOverlay=false.
        WindowBusyOverlayService.Push(showProgress: true);
        try
        {
            var panel = new SlideshowAlbumPickerPanel(albumKind, albumRoot, albumDisplayName);
            var result = await HostedDialogService.ShowAsync(
                new HostedDialogSpec
                {
                    Title = GlassSettingsResources.Get("Themes_Bg_AlbumPickerTitle"),
                    Body = panel,
                    MinWidth = 820,
                    MaxWidth = 1040,
                    // Потолок; Height = min(MaxHeight, DialogBoundsHost) при каждом resize.
                    MaxHeight = 746,
                    ManageOverlay = false,
                    BodyHandlesScroll = true,
                    FooterDividerFullWidth = true,
                    Footer = new HostedDialogFooterSpec
                    {
                        PrimaryButtonText = GlassSettingsResources.Get("Themes_Bg_AlbumPickerSave"),
                        PrimaryButtonWidth = 108,
                        IsPrimaryEnabled = false
                    }
                },
                configure: ctx => panel.AttachHostContext(ctx)).ConfigureAwait(true);

            if (result != HostedDialogResult.Primary && hasSnapshot)
            {
                _ = await Task.Run(() =>
                        DesktopBackgroundService.TryRestoreSlideshowEditSnapshot(snapshot))
                    .ConfigureAwait(true);
            }
        }
        finally
        {
            WindowBusyOverlayService.Pop();
        }
    }
}
