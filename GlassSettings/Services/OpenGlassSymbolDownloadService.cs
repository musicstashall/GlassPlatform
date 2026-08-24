using GlassSettings.Core.Platform;

namespace GlassSettings.Services;

public enum OpenGlassSymbolDownloadOutcomeKind
{
    Success,
    Cancelled,
    Failed,
}

public enum OpenGlassSymbolDownloadFailureKind
{
    None,
    ConnectionLost,
    TimedOut,
    Other,
}

public readonly record struct OpenGlassSymbolDownloadCompletion(
    OpenGlassSymbolDownloadOutcomeKind Kind,
    int HResult,
    string Message,
    OpenGlassSymbolDownloadFailureKind FailureKind = OpenGlassSymbolDownloadFailureKind.None);

public readonly record struct SymbolDownloadProgress(int? Percent, string? PdbFileName);

public sealed class OpenGlassSymbolDownloadService
{
    public static OpenGlassSymbolDownloadService Current { get; } = new();

    private readonly object _gate = new();
    private readonly OpenGlassSymbolDownloadWorker _worker = new();
    private CancellationTokenSource? _downloadCts;
    private Task<OpenGlassSymbolDownloadCompletion>? _activeTask;

    public bool IsRunning
    {
        get
        {
            lock (_gate)
                return _activeTask is { IsCompleted: false };
        }
    }

    public string GetCacheDirectoryAbsolute()
    {
        var dllPath = PlatformPaths.OpenGlassDllPath
                      ?? throw new InvalidOperationException("OpenGlass.dll path is unavailable.");

        var dllDir = Path.GetDirectoryName(dllPath)
                     ?? throw new InvalidOperationException("OpenGlass.dll directory is unavailable.");

        return Path.Combine(dllDir, "symbols");
    }

    public string GetCacheDirectoryDisplayPath()
    {
        var absolute = GetCacheDirectoryAbsolute();
        try
        {
            return Path.GetRelativePath(AppContext.BaseDirectory, absolute);
        }
        catch
        {
            return absolute;
        }
    }

    public bool HasExistingCacheContent()
    {
        var cacheRoot = GetCacheDirectoryAbsolute();
        return Directory.Exists(cacheRoot)
               && Directory.EnumerateFileSystemEntries(cacheRoot).Any();
    }

    public Task<OpenGlassSymbolDownloadCompletion> StartDownloadAsync(
        IProgress<SymbolDownloadProgress> progress,
        CancellationToken cancellationToken = default)
    {
        lock (_gate)
        {
            if (_activeTask is { IsCompleted: false })
                throw new InvalidOperationException("Symbol download is already running.");

            _downloadCts?.Dispose();
            _downloadCts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);

            var layout = OpenGlassSymbolCacheLayoutPolicy.ResolveLayout(
                OpenGlassVersionService.TryGetInstalledVersion(out var version, out _)
                    ? version
                    : null);

            _activeTask = RunDownloadAndCleanupAsync(
                GetCacheDirectoryAbsolute(),
                layout,
                progress,
                _downloadCts.Token);

            return _activeTask;
        }
    }

    private async Task<OpenGlassSymbolDownloadCompletion> RunDownloadAndCleanupAsync(
        string cacheRoot,
        OpenGlassSymbolCacheLayoutKind layout,
        IProgress<SymbolDownloadProgress> progress,
        CancellationToken cancellationToken)
    {
        try
        {
            return await _worker.DownloadAsync(cacheRoot, layout, progress, cancellationToken)
                .ConfigureAwait(false);
        }
        finally
        {
            lock (_gate)
            {
                _downloadCts?.Dispose();
                _downloadCts = null;
            }
        }
    }

    public void CancelDownload()
    {
        lock (_gate)
            _downloadCts?.Cancel();
    }

    public async Task CancelAndWaitAsync(TimeSpan timeout)
    {
        Task<OpenGlassSymbolDownloadCompletion>? task;
        lock (_gate)
        {
            _downloadCts?.Cancel();
            task = _activeTask;
        }

        if (task is null)
            return;

        try
        {
            await task.WaitAsync(timeout).ConfigureAwait(false);
        }
        catch (TimeoutException)
        {
        }
        catch (OperationCanceledException)
        {
        }
        finally
        {
            lock (_gate)
            {
                if (ReferenceEquals(_activeTask, task) && task.IsCompleted)
                {
                    _downloadCts?.Dispose();
                    _downloadCts = null;
                    _activeTask = null;
                }
            }
        }
    }
}
