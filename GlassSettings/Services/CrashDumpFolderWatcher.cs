using Microsoft.UI.Dispatching;

namespace GlassSettings.Services;

/// <summary>
/// Наблюдение за каталогом *.dmp: Created/Deleted/Changed/Renamed → debounced Changed.
/// </summary>
public sealed class CrashDumpFolderWatcher : IDisposable
{
    private readonly DispatcherQueue _dispatcherQueue;
    private readonly object _gate = new();
    private FileSystemWatcher? _watcher;
    private DispatcherQueueTimer? _debounceTimer;
    private string? _folder;
    private bool _disposed;
    private bool _pending;

    public event EventHandler? Changed;

    public CrashDumpFolderWatcher(DispatcherQueue dispatcherQueue)
    {
        _dispatcherQueue = dispatcherQueue ?? throw new ArgumentNullException(nameof(dispatcherQueue));
    }

    /// <summary>Старт с папкой платформы (совместимость).</summary>
    public void Start() => Start(CrashDumpCatalog.ResolvePlatformFolder());

    public void Start(string folder)
    {
        lock (_gate)
        {
            if (_disposed)
                return;

            StopCore();

            if (string.IsNullOrWhiteSpace(folder))
                return;

            _folder = folder;

            try
            {
                Directory.CreateDirectory(folder);
            }
            catch (Exception)
            {
                return;
            }

            FileSystemWatcher watcher;
            try
            {
                watcher = new FileSystemWatcher(folder, "*.dmp")
                {
                    NotifyFilter = NotifyFilters.FileName
                        | NotifyFilters.LastWrite
                        | NotifyFilters.Size
                        | NotifyFilters.CreationTime,
                    IncludeSubdirectories = false,
                    EnableRaisingEvents = false,
                };
            }
            catch (Exception)
            {
                return;
            }

            watcher.Created += OnFsEvent;
            watcher.Deleted += OnFsEvent;
            watcher.Changed += OnFsEvent;
            watcher.Renamed += OnFsRenamed;
            watcher.Error += OnFsError;

            try
            {
                watcher.EnableRaisingEvents = true;
            }
            catch (Exception)
            {
                watcher.Dispose();
                return;
            }

            _watcher = watcher;
            _debounceTimer = _dispatcherQueue.CreateTimer();
            _debounceTimer.Interval = TimeSpan.FromMilliseconds(300);
            _debounceTimer.IsRepeating = false;
            _debounceTimer.Tick += OnDebounceTick;
        }
    }

    public void Stop()
    {
        lock (_gate)
        {
            StopCore();
        }
    }

    public void Dispose()
    {
        lock (_gate)
        {
            if (_disposed)
                return;
            _disposed = true;
            StopCore();
        }
    }

    private void StopCore()
    {
        _pending = false;

        if (_debounceTimer is not null)
        {
            _debounceTimer.Stop();
            _debounceTimer.Tick -= OnDebounceTick;
            _debounceTimer = null;
        }

        if (_watcher is not null)
        {
            try
            {
                _watcher.EnableRaisingEvents = false;
            }
            catch (Exception)
            {
                // ignore
            }

            _watcher.Created -= OnFsEvent;
            _watcher.Deleted -= OnFsEvent;
            _watcher.Changed -= OnFsEvent;
            _watcher.Renamed -= OnFsRenamed;
            _watcher.Error -= OnFsError;
            _watcher.Dispose();
            _watcher = null;
        }
    }

    private void OnFsEvent(object sender, FileSystemEventArgs e) => ScheduleNotify();

    private void OnFsRenamed(object sender, RenamedEventArgs e) => ScheduleNotify();

    private void OnFsError(object sender, ErrorEventArgs e)
    {
        // Пересоздаём watcher на UI-потоке после сбоя буфера.
        var folder = _folder;
        _ = _dispatcherQueue.TryEnqueue(DispatcherQueuePriority.Low, () =>
        {
            if (_disposed || string.IsNullOrWhiteSpace(folder))
                return;
            Start(folder);
            ScheduleNotify();
        });
    }

    private void ScheduleNotify()
    {
        if (_disposed)
            return;

        _pending = true;
        _ = _dispatcherQueue.TryEnqueue(DispatcherQueuePriority.Normal, () =>
        {
            if (_disposed || _debounceTimer is null)
                return;

            _debounceTimer.Stop();
            _debounceTimer.Start();
        });
    }

    private void OnDebounceTick(DispatcherQueueTimer sender, object args)
    {
        if (_disposed || !_pending)
            return;

        _pending = false;
        Changed?.Invoke(this, EventArgs.Empty);
    }
}
