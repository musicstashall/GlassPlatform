using System.Runtime.InteropServices.WindowsRuntime;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.Storage.Streams;

namespace GlassSettings.Services;

/// <summary>Единый read/decode обоев рабочего стола для всех preview-контролов GS.</summary>
internal static class DesktopWallpaperImageCache
{
    public const int DisplayDecodeWidth = 340;

    private static readonly object Gate = new();

    private static string? _cachedIdentity;
    private static string? _cachedPath;
    private static byte[]? _cachedBytes;
    private static ImageSource? _sharedDisplaySource;
    private static Task? _loadTask;
    private static bool _reloadSuspended;
    private static bool _reloadPending;

    public static bool IsReady
    {
        get
        {
            lock (Gate)
                return _sharedDisplaySource is not null;
        }
    }

    public static Task? LoadTask
    {
        get
        {
            lock (Gate)
                return _loadTask;
        }
    }

    public static string? CachedIdentity
    {
        get
        {
            lock (Gate)
                return _cachedIdentity;
        }
    }

    public static void BeginLoad(DispatcherQueue dispatcherQueue)
    {
        var identity = CurrentWallpaperResolver.TryGetWallpaperImageColorIdentity();
        if (string.IsNullOrWhiteSpace(identity))
        {
            Invalidate();
            return;
        }

        lock (Gate)
        {
            if (_loadTask is { IsCompleted: false }
                && string.Equals(_cachedIdentity, identity, StringComparison.OrdinalIgnoreCase))
            {
                return;
            }

            if (_sharedDisplaySource is not null
                && string.Equals(_cachedIdentity, identity, StringComparison.OrdinalIgnoreCase))
            {
                return;
            }

            _loadTask = LoadCoreAsync(dispatcherQueue, identity);
        }
    }

    public static void NotifyWallpaperChanged(DispatcherQueue dispatcherQueue)
    {
        lock (Gate)
        {
            if (_reloadSuspended)
            {
                _reloadPending = true;
                return;
            }
        }

        Invalidate();
        BeginLoad(dispatcherQueue);
    }

    public static void SuspendReload()
    {
        lock (Gate)
            _reloadSuspended = true;
    }

    public static void ResumeReload(DispatcherQueue dispatcherQueue)
    {
        var run = false;
        lock (Gate)
        {
            _reloadSuspended = false;
            if (_reloadPending)
            {
                _reloadPending = false;
                run = true;
            }
        }

        if (run)
            NotifyWallpaperChanged(dispatcherQueue);
    }

    public static void Invalidate()
    {
        lock (Gate)
        {
            _cachedIdentity = null;
            _cachedPath = null;
            _cachedBytes = null;
            _sharedDisplaySource = null;
            _loadTask = null;
        }
    }

    /// <summary>Публикует decode из уже прочитанных bytes (atomic bundle с blur pipeline).</summary>
    internal static void PublishDecodedWallpaper(
        byte[] bytes,
        string identity,
        string path,
        DispatcherQueue dispatcherQueue)
    {
        if (bytes is not { Length: > 0 }
            || string.IsNullOrWhiteSpace(identity)
            || string.IsNullOrWhiteSpace(path))
        {
            return;
        }

        lock (Gate)
        {
            if (_sharedDisplaySource is not null
                && string.Equals(_cachedIdentity, identity, StringComparison.OrdinalIgnoreCase)
                && string.Equals(_cachedPath, path, StringComparison.OrdinalIgnoreCase)
                && _cachedBytes is not null
                && _cachedBytes.Length == bytes.Length)
            {
                return;
            }

            _cachedIdentity = identity;
            _cachedPath = path;
            _cachedBytes = bytes;
            _sharedDisplaySource = null;
        }

        _ = PublishDecodedWallpaperCoreAsync(dispatcherQueue, bytes, identity, path);
    }

    private static async Task PublishDecodedWallpaperCoreAsync(
        DispatcherQueue dispatcherQueue,
        byte[] bytes,
        string identity,
        string path)
    {
        try
        {
            var image = await DecodeDisplayBitmapAsync(dispatcherQueue, bytes).ConfigureAwait(true);
            if (image is null)
                return;

            lock (Gate)
            {
                if (!string.Equals(_cachedIdentity, identity, StringComparison.OrdinalIgnoreCase)
                    || !string.Equals(_cachedPath, path, StringComparison.OrdinalIgnoreCase))
                {
                    return;
                }

                _sharedDisplaySource = image;
            }
        }
        catch
        {
            // publish не должен ронять host
        }
    }

    public static ImageSource? TryGetSharedDisplaySource()
    {
        lock (Gate)
            return _sharedDisplaySource;
    }

    public static ImageSource? GetDisplaySourceOrLoad(string? path = null)
    {
        var shared = TryGetSharedDisplaySource();
        if (shared is not null)
            return shared;

        var resolved = path;
        if (string.IsNullOrWhiteSpace(resolved))
            resolved = CurrentWallpaperResolver.TryGetDesktopWallpaperPath();

        return ThemeImageUriHelper.TryCreateBitmap(resolved, DisplayDecodeWidth);
    }

    public static byte[]? TryGetCachedBytes()
    {
        lock (Gate)
            return _cachedBytes;
    }

    public static string? TryGetCachedPath()
    {
        lock (Gate)
            return _cachedPath;
    }

    private static async Task LoadCoreAsync(DispatcherQueue dispatcherQueue, string identity)
    {
        try
        {
            var path = CurrentWallpaperResolver.TryGetDesktopWallpaperPath();
            if (string.IsNullOrWhiteSpace(path) || !File.Exists(path))
                return;

            var bytes = await Task.Run(() => ReadWallpaperBytes(path)).ConfigureAwait(false);
            if (bytes is null || bytes.Length == 0)
                return;

            var image = await DecodeDisplayBitmapAsync(dispatcherQueue, bytes).ConfigureAwait(true);
            if (image is null)
                return;

            lock (Gate)
            {
                _cachedIdentity = identity;
                _cachedPath = path;
                _cachedBytes = bytes;
                _sharedDisplaySource = image;
            }
        }
        catch
        {
            // preload не должен ронять host
        }
    }

    private static byte[]? ReadWallpaperBytes(string path)
    {
        try
        {
            var fullPath = Path.GetFullPath(path);
            using var stream = new FileStream(fullPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
            using var memory = new MemoryStream();
            stream.CopyTo(memory);
            return memory.ToArray();
        }
        catch
        {
            return null;
        }
    }

    private static Task<BitmapImage?> DecodeDisplayBitmapAsync(DispatcherQueue dispatcherQueue, byte[] bytes)
    {
        var tcs = new TaskCompletionSource<BitmapImage?>(TaskCreationOptions.RunContinuationsAsynchronously);

        if (!dispatcherQueue.TryEnqueue(DispatcherQueuePriority.Normal, () => _ = DecodeDisplayBitmapCoreAsync(bytes, tcs)))
            tcs.TrySetResult(null);

        return tcs.Task;
    }

    private static async Task DecodeDisplayBitmapCoreAsync(byte[] bytes, TaskCompletionSource<BitmapImage?> tcs)
    {
        try
        {
            var image = new BitmapImage { DecodePixelWidth = DisplayDecodeWidth };
            using var stream = new InMemoryRandomAccessStream();
            await stream.WriteAsync(bytes.AsBuffer());
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
