using GlassSettings.Controls.Processes;
using GlassSettings.Core.Registry;
using GlassSettings.Strings;
using Microsoft.UI.Dispatching;

namespace GlassSettings.Services;

/// <summary>Hosted-диалог выбора файла/папки (TreeView + BreadcrumbBar, без IFileOpenDialog).</summary>
public static class ExecutableFilePickerDialogService
{
    private static readonly HashSet<string> SupportedExecutableExtensions =
        new(ExecutableFilePickerCatalog.SupportedExecutableExtensions, StringComparer.OrdinalIgnoreCase);

    private static readonly HashSet<string> SupportedImageExtensions =
        new(ExecutableFilePickerCatalog.SupportedImageExtensions, StringComparer.OrdinalIgnoreCase);

    /// <summary>Attach-list: штора сразу; при выборе может остаться открытой.</summary>
    public static async Task<string?> PickAttachListAsync(
        DispatcherQueue dispatcherQueue,
        bool retainOverlayAfterSelection = false)
    {
        var retainOverlay = false;
        WindowBusyOverlayService.Push(showProgress: true);
        try
        {
            var picked = await PickExecutableAsync(dispatcherQueue).ConfigureAwait(true);
            retainOverlay = retainOverlayAfterSelection && picked is not null;
            return picked;
        }
        finally
        {
            if (!retainOverlay)
                WindowBusyOverlayService.Pop();
        }
    }

    /// <summary>Block-list: штора на время диалога.</summary>
    public static async Task<string?> PickBlockListAsync(DispatcherQueue dispatcherQueue)
    {
        WindowBusyOverlayService.Push(showProgress: true);
        try
        {
            return await PickExecutableAsync(dispatcherQueue).ConfigureAwait(true);
        }
        finally
        {
            WindowBusyOverlayService.Pop();
        }
    }

    /// <summary>Фон рабочего стола: полный путь к изображению (jpg/png/…).</summary>
    public static async Task<string?> PickImageAsync(DispatcherQueue dispatcherQueue)
    {
        WindowBusyOverlayService.Push(showProgress: true);
        try
        {
            while (true)
            {
                var panel = new ExecutableFilePickerPanel(
                    dispatcherQueue,
                    ExecutableFilePickerCatalog.GetImageExtensionFilters());

                try
                {
                    var result = await HostedDialogService.ShowAsync(
                        new HostedDialogSpec
                        {
                            Title = GlassSettingsResources.Get("Themes_Bg_FilePickerTitle"),
                            Body = panel,
                            MinWidth = ExecutableFilePickerLayoutMetrics.CompactDialogMinWidth,
                            MaxWidth = 900,
                            MaxHeight = 560,
                            ManageOverlay = false,
                            BodyHandlesScroll = true,
                            DeferPrimaryCloseToHost = true,
                            Footer = new HostedDialogFooterSpec
                            {
                                PrimaryButtonText = GlassSettingsResources.Get("Attach_FilePickerSelect"),
                                PrimaryButtonWidth = 108,
                                IsPrimaryEnabled = false
                            }
                        },
                        panel.AttachDialogContext).ConfigureAwait(true);

                    if (result != HostedDialogResult.Primary)
                        return null;

                    var path = panel.GetSelectedFilePath();
                    if (string.IsNullOrWhiteSpace(path))
                        return null;

                    if (!IsSupportedImagePath(path))
                    {
                        await ShowInvalidImageTypeDialogAsync().ConfigureAwait(true);
                        continue;
                    }

                    return path;
                }
                finally
                {
                    panel.PersistLastDirectory();
                }
            }
        }
        finally
        {
            WindowBusyOverlayService.Pop();
        }
    }

    /// <summary>Слайд-шоу: полный путь к папке альбома (только каталоги, без фильтров расширений).</summary>
    public static async Task<string?> PickFolderAsync(
        DispatcherQueue dispatcherQueue,
        string? initialDirectory = null)
    {
        WindowBusyOverlayService.Push(showProgress: true);
        try
        {
            var panel = new ExecutableFilePickerPanel(
                dispatcherQueue,
                foldersOnly: true,
                initialDirectory);

            var result = await HostedDialogService.ShowAsync(
                new HostedDialogSpec
                {
                    Title = GlassSettingsResources.Get("Themes_Bg_FolderPickerTitle"),
                    Body = panel,
                    MinWidth = ExecutableFilePickerLayoutMetrics.CompactDialogMinWidth,
                    MaxWidth = 900,
                    MaxHeight = 560,
                    ManageOverlay = false,
                    BodyHandlesScroll = true,
                    DeferPrimaryCloseToHost = true,
                    Footer = new HostedDialogFooterSpec
                    {
                        PrimaryButtonText = GlassSettingsResources.Get("Attach_FilePickerSelect"),
                        PrimaryButtonWidth = 108,
                        IsPrimaryEnabled = false
                    }
                },
                panel.AttachDialogContext).ConfigureAwait(true);

            if (result != HostedDialogResult.Primary)
                return null;

            var path = panel.GetSelectedFolderPath();
            return string.IsNullOrWhiteSpace(path) ? null : path;
        }
        finally
        {
            WindowBusyOverlayService.Pop();
        }
    }

    private static async Task<string?> PickExecutableAsync(DispatcherQueue dispatcherQueue)
    {
        while (true)
        {
            var panel = new ExecutableFilePickerPanel(dispatcherQueue);

            try
            {
                var result = await HostedDialogService.ShowAsync(
                    new HostedDialogSpec
                    {
                        Title = GlassSettingsResources.Get("Attach_FilePickerTitle"),
                        Body = panel,
                        MinWidth = ExecutableFilePickerLayoutMetrics.CompactDialogMinWidth,
                        MaxWidth = 900,
                        MaxHeight = 560,
                        ManageOverlay = false,
                        BodyHandlesScroll = true,
                        DeferPrimaryCloseToHost = true,
                        Footer = new HostedDialogFooterSpec
                        {
                            PrimaryButtonText = GlassSettingsResources.Get("Attach_FilePickerSelect"),
                            PrimaryButtonWidth = 108,
                            IsPrimaryEnabled = false
                        }
                    },
                    panel.AttachDialogContext).ConfigureAwait(true);

                if (result != HostedDialogResult.Primary)
                    return null;

                var path = panel.GetSelectedFilePath();
                if (string.IsNullOrWhiteSpace(path))
                    return null;

                if (!IsSupportedExecutablePath(path))
                {
                    await ShowInvalidFileTypeDialogAsync().ConfigureAwait(true);
                    continue;
                }

                var exeName = AttachListRegistry.NormalizeExeName(Path.GetFileName(path));
                await Task.Run(() => ProcessIconResolver.RegisterKnownExecutablePath(path)).ConfigureAwait(true);
                await ProcessIconResolver.PreloadExeAssetsAsync(exeName).ConfigureAwait(true);
                return exeName;
            }
            finally
            {
                panel.PersistLastDirectory();
            }
        }
    }

    private static bool IsSupportedExecutablePath(string path)
    {
        var extension = Path.GetExtension(path);
        return SupportedExecutableExtensions.Contains(extension);
    }

    private static bool IsSupportedImagePath(string path)
    {
        var extension = Path.GetExtension(path);
        return SupportedImageExtensions.Contains(extension);
    }

    private static Task ShowInvalidFileTypeDialogAsync() =>
        HostedConfirmationDialogService.ConfirmAsync(new HostedConfirmationSpec
        {
            Title = GlassSettingsResources.Get("Attach_FilePickerInvalidTypeTitle"),
            Body = HostedConfirmationDialogService.CreateBody(
                GlassSettingsResources.Get("Attach_FilePickerInvalidTypeBody")),
            Footer = HostedConfirmationDialogService.CreateFooter(
                GlassSettingsResources.Get("Dialog_Understood"),
                primaryButtonWidth: 108),
            ManageOverlay = false,
            MinWidth = 420,
            MaxWidth = 480
        });

    private static Task ShowInvalidImageTypeDialogAsync() =>
        HostedConfirmationDialogService.ConfirmAsync(new HostedConfirmationSpec
        {
            Title = GlassSettingsResources.Get("Themes_Bg_FilePickerInvalidTypeTitle"),
            Body = HostedConfirmationDialogService.CreateBody(
                GlassSettingsResources.Get("Themes_Bg_FilePickerInvalidTypeBody")),
            Footer = HostedConfirmationDialogService.CreateFooter(
                GlassSettingsResources.Get("Dialog_Understood"),
                primaryButtonWidth: 108),
            ManageOverlay = false,
            MinWidth = 420,
            MaxWidth = 480
        });
}
