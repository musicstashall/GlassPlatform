using System.Collections.Concurrent;
using System.Runtime.InteropServices.WindowsRuntime;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.Storage.Streams;

namespace GlassSettings.Services;

/// <summary>Shell-иконки строк списка в hosted file picker.</summary>
internal static class ExecutableFilePickerListIconResolver
{
    private static readonly ConcurrentDictionary<string, byte[]> PngCache = new(StringComparer.OrdinalIgnoreCase);
    private static readonly ConcurrentDictionary<string, ImageSource> BitmapCache = new(StringComparer.OrdinalIgnoreCase);
    private static readonly ConcurrentDictionary<string, Task<byte[]?>> PngInFlight = new(StringComparer.OrdinalIgnoreCase);

    public static void ApplyFolderIcons(IEnumerable<ExecutableFilePickerItem> items)
    {
        var folderIcon = ShellFolderIconResolver.TryGetFolderIcon();
        if (folderIcon is null)
            return;

        foreach (var item in items)
        {
            if (item.IsFolder)
                item.Icon = folderIcon;
        }
    }

    /// <param name="preferImageThumbnails">
    /// Large icons: для изображений — превью файла (как в системном диалоге), иначе shell-иконка.
    /// </param>
    public static async Task ApplyIconsAsync(
        IReadOnlyList<ExecutableFilePickerItem> items,
        DispatcherQueue dispatcherQueue,
        int generation,
        Func<int, bool> isCurrentGeneration,
        bool preferImageThumbnails = false)
    {
        var files = items.Where(static item => !item.IsFolder).ToList();
        if (files.Count == 0)
            return;

        foreach (var item in files)
        {
            if (!isCurrentGeneration(generation))
                return;

            var useThumbnail = preferImageThumbnails
                && ExecutableFilePickerCatalog.IsImageExtension(item.FullPath);
            var cacheKey = useThumbnail ? "thumb:" + item.FullPath : item.FullPath;

            if (BitmapCache.TryGetValue(cacheKey, out var cached))
            {
                await AssignIconOnUiThreadAsync(dispatcherQueue, item, cached).ConfigureAwait(true);
                continue;
            }

            ImageSource? image;
            if (useThumbnail)
            {
                image = await CreateImageThumbnailAsync(dispatcherQueue, item.FullPath).ConfigureAwait(true);
            }
            else
            {
                var pngBytes = await GetOrLoadPngBytesAsync(item.FullPath).ConfigureAwait(false);
                if (!isCurrentGeneration(generation) || pngBytes is null)
                    continue;

                image = await CreateBitmapImageAsync(dispatcherQueue, pngBytes).ConfigureAwait(true);
            }

            if (!isCurrentGeneration(generation) || image is null)
                continue;

            BitmapCache[cacheKey] = image;
            await AssignIconOnUiThreadAsync(dispatcherQueue, item, image).ConfigureAwait(true);
        }
    }

    private static Task<ImageSource?> CreateImageThumbnailAsync(
        DispatcherQueue dispatcherQueue,
        string fullPath)
    {
        var tcs = new TaskCompletionSource<ImageSource?>(TaskCreationOptions.RunContinuationsAsynchronously);

        if (!dispatcherQueue.TryEnqueue(DispatcherQueuePriority.Normal, () =>
            {
                try
                {
                    // DecodePixelWidth — как крупные значки проводника (~96 px).
                    tcs.TrySetResult(ThemeImageUriHelper.TryCreateBitmap(fullPath, decodePixelWidth: 96));
                }
                catch (Exception ex)
                {
                    tcs.TrySetException(ex);
                }
            }))
        {
            tcs.TrySetResult(null);
        }

        return tcs.Task;
    }

    private static Task AssignIconOnUiThreadAsync(
        DispatcherQueue dispatcherQueue,
        ExecutableFilePickerItem item,
        ImageSource? icon)
    {
        var tcs = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

        if (!dispatcherQueue.TryEnqueue(DispatcherQueuePriority.Normal, () =>
            {
                try
                {
                    item.Icon = icon;
                    tcs.TrySetResult();
                }
                catch (Exception ex)
                {
                    tcs.TrySetException(ex);
                }
            }))
        {
            tcs.TrySetResult();
        }

        return tcs.Task;
    }

    private static async Task<byte[]?> GetOrLoadPngBytesAsync(string fullPath)
    {
        if (PngCache.TryGetValue(fullPath, out var cached))
            return cached;

        if (PngInFlight.TryGetValue(fullPath, out var existing))
            return await existing.ConfigureAwait(false);

        var loadTask = StaThreadRunner.RunAsync(() => LoadIconPngBytes(fullPath));
        if (!PngInFlight.TryAdd(fullPath, loadTask))
            return await PngInFlight[fullPath].ConfigureAwait(false);

        try
        {
            var pngBytes = await loadTask.ConfigureAwait(false);
            if (pngBytes is not null)
                PngCache[fullPath] = pngBytes;
            return pngBytes;
        }
        finally
        {
            PngInFlight.TryRemove(fullPath, out _);
        }
    }

    private static byte[]? LoadIconPngBytes(string fullPath)
    {
        try
        {
            var hIcon = NativeShellIconInterop.GetFileIconHandle(fullPath, out var mustDestroy);
            try
            {
                if (hIcon == IntPtr.Zero)
                    return null;

                return IconHandleToPngBytes(hIcon);
            }
            finally
            {
                NativeShellIconInterop.ReleaseIcon(hIcon, mustDestroy);
            }
        }
        catch
        {
            return null;
        }
    }

    private static byte[]? IconHandleToPngBytes(IntPtr hIcon)
    {
        using var icon = (System.Drawing.Icon)System.Drawing.Icon.FromHandle(hIcon).Clone();
        using var bitmap = icon.ToBitmap();
        using var stream = new MemoryStream();
        bitmap.Save(stream, System.Drawing.Imaging.ImageFormat.Png);
        return stream.ToArray();
    }

    private static Task<ImageSource?> CreateBitmapImageAsync(DispatcherQueue dispatcherQueue, byte[] pngBytes)
    {
        var tcs = new TaskCompletionSource<ImageSource?>(TaskCreationOptions.RunContinuationsAsynchronously);

        if (!dispatcherQueue.TryEnqueue(DispatcherQueuePriority.Normal, () => _ = CreateBitmapImageCoreAsync(pngBytes, tcs)))
            tcs.TrySetResult(null);

        return tcs.Task;
    }

    private static async Task CreateBitmapImageCoreAsync(byte[] pngBytes, TaskCompletionSource<ImageSource?> tcs)
    {
        try
        {
            var image = new BitmapImage();
            using var stream = new InMemoryRandomAccessStream();
            await stream.WriteAsync(pngBytes.AsBuffer());
            stream.Seek(0);
            await image.SetSourceAsync(stream);
            tcs.TrySetResult(image);
        }
        catch
        {
            tcs.TrySetResult(null);
        }
    }
}
