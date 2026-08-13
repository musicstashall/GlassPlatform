using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml.Media;

namespace GlassSettings.Services;

/// <summary>
/// Thumbnail recent-обоин (BackgroundHistoryPath0..4) для страницы «Фон».
/// Прогрев под заставкой GS; фоновое обновление при внешнем WallpaperChanged.
/// </summary>
internal static class RecentWallpaperThumbCache
{
    public const int DecodeEdgePx = 100;

    private static readonly object Gate = new();

    private static string? _fingerprint;
    private static IReadOnlyList<CachedThumb>? _items;
    private static Task? _loadTask;
    private static bool _reloadSuspended;
    private static bool _reloadPending;

    public readonly record struct CachedThumb(string Path, ImageSource Source);

    public static Task? LoadTask
    {
        get
        {
            lock (Gate)
                return _loadTask;
        }
    }

    public static bool IsReady
    {
        get
        {
            lock (Gate)
                return _items is not null
                    && string.Equals(_fingerprint, BuildFingerprint(), StringComparison.Ordinal);
        }
    }

    public static void BeginLoad(DispatcherQueue dispatcherQueue)
    {
        var fingerprint = BuildFingerprint();
        lock (Gate)
        {
            if (_loadTask is { IsCompleted: false }
                && string.Equals(_fingerprint, fingerprint, StringComparison.Ordinal))
            {
                return;
            }

            if (_items is not null
                && string.Equals(_fingerprint, fingerprint, StringComparison.Ordinal))
            {
                return;
            }

            _loadTask = LoadCoreAsync(dispatcherQueue, fingerprint);
        }
    }

    public static void NotifyHistoryChanged(DispatcherQueue dispatcherQueue)
    {
        lock (Gate)
        {
            // Mode→Фото LTR: не Invalidate/BeginLoad — UI-decode ворует кадры
            // (log: loadPending:true при usedPreferred во время reveal).
            if (_reloadSuspended)
            {
                _reloadPending = true;
                return;
            }
        }

        Invalidate();
        BeginLoad(dispatcherQueue);
    }

    /// <summary>Пауза фонового reload на время L→R reveal (см. NotifyHistoryChanged).</summary>
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
            NotifyHistoryChanged(dispatcherQueue);
    }

    public static void Invalidate()
    {
        lock (Gate)
        {
            _fingerprint = null;
            _items = null;
            _loadTask = null;
        }
    }

    public static bool TryGet(string path, out ImageSource? source)
    {
        source = null;
        if (string.IsNullOrWhiteSpace(path))
            return false;

        lock (Gate)
        {
            if (_items is null
                || !string.Equals(_fingerprint, BuildFingerprint(), StringComparison.Ordinal))
            {
                return false;
            }

            foreach (var item in _items)
            {
                if (!string.Equals(item.Path, path, StringComparison.OrdinalIgnoreCase))
                    continue;

                source = item.Source;
                return true;
            }
        }

        return false;
    }

    public static IReadOnlyList<CachedThumb>? TryGetSnapshot()
    {
        lock (Gate)
        {
            if (_items is null
                || !string.Equals(_fingerprint, BuildFingerprint(), StringComparison.Ordinal))
            {
                return null;
            }

            return _items;
        }
    }

    private static async Task LoadCoreAsync(DispatcherQueue dispatcherQueue, string fingerprint)
    {
        var paths = DesktopBackgroundService.GetRecentHistoryPaths();
        var built = new List<CachedThumb>(paths.Count);

        foreach (var path in paths)
        {
            var tcs = new TaskCompletionSource<ImageSource?>(
                TaskCreationOptions.RunContinuationsAsynchronously);

            if (!dispatcherQueue.TryEnqueue(
                    DispatcherQueuePriority.Normal,
                    () => _ = DecodeOnUiAsync(path, tcs)))
            {
                tcs.TrySetResult(null);
            }

            var source = await tcs.Task.ConfigureAwait(false);
            if (source is not null)
                built.Add(new CachedThumb(path, source));
        }

        lock (Gate)
        {
            var now = BuildFingerprint();
            // Пока грузили — история могла смениться; не публикуем устаревшее.
            if (!string.Equals(now, fingerprint, StringComparison.Ordinal))
            {
                _loadTask = null;
            }
            else
            {
                _fingerprint = fingerprint;
                _items = built;
            }
        }

        // История уехала во время decode — догнать актуальную.
        if (!string.Equals(BuildFingerprint(), fingerprint, StringComparison.Ordinal))
            BeginLoad(dispatcherQueue);
    }

    private static async Task DecodeOnUiAsync(string path, TaskCompletionSource<ImageSource?> tcs)
    {
        try
        {
            var source = await ThemeImageUriHelper.TryCreateThumbnailAsync(path, DecodeEdgePx)
                .ConfigureAwait(true);
            tcs.TrySetResult(source);
        }
        catch
        {
            tcs.TrySetResult(null);
        }
    }

    private static string BuildFingerprint()
    {
        var paths = DesktopBackgroundService.GetRecentHistoryPaths();
        if (paths.Count == 0)
            return string.Empty;

        var parts = new string[paths.Count];
        for (var i = 0; i < paths.Count; i++)
        {
            var path = paths[i];
            long ticks = 0;
            try
            {
                if (File.Exists(path))
                    ticks = File.GetLastWriteTimeUtc(path).Ticks;
            }
            catch
            {
                // ignore
            }

            parts[i] = path + "|" + ticks.ToString(System.Globalization.CultureInfo.InvariantCulture);
        }

        return string.Join(";", parts);
    }
}
