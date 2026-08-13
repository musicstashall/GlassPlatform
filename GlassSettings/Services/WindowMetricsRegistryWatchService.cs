using GlassSettings.Core.Snapshot;
using GlassSettings.Core.Text;
using Microsoft.UI.Dispatching;
using Microsoft.Win32;

namespace GlassSettings.Services;

/// <summary>
/// RegNotify на HKCU\Control Panel\Desktop\WindowMetrics (6 LOGFONT).
/// Debounce + fingerprint; suppress при наших записях.
/// </summary>
public sealed class WindowMetricsRegistryWatchService : IDisposable
{
    private static readonly TimeSpan DebounceInterval = TimeSpan.FromMilliseconds(450);

    public static WindowMetricsRegistryWatchService? Current { get; private set; }

    private readonly DispatcherQueue _dispatcherQueue;
    private readonly RegistryValueChangeWatcher _watcher;
    private readonly DispatcherQueueTimer _debounceTimer;

    private SettingsSnapshot? _snapshotRef;
    private string? _lastFingerprint;
    private DateTime _suppressExternalDirtyUntilUtc;
    private bool _disposed;

    public event EventHandler? ExternalRegistryChanged;

    public WindowMetricsRegistryWatchService(DispatcherQueue dispatcherQueue, SettingsSnapshot snapshot)
    {
        _dispatcherQueue = dispatcherQueue;
        _snapshotRef = snapshot;
        _debounceTimer = dispatcherQueue.CreateTimer();
        _debounceTimer.Interval = DebounceInterval;
        _debounceTimer.IsRepeating = false;
        _debounceTimer.Tick += (_, _) => EvaluateRegistryChange();

        _watcher = new RegistryValueChangeWatcher(
            dispatcherQueue,
            RegistryHive.CurrentUser,
            SystemFontsCatalog.RegistrySubKey,
            OnRegistrySignal,
            SystemFontsCatalog.RegistryValueNames[0],
            SystemFontsCatalog.RegistryValueNames[1],
            SystemFontsCatalog.RegistryValueNames[2],
            SystemFontsCatalog.RegistryValueNames[3],
            SystemFontsCatalog.RegistryValueNames[4],
            SystemFontsCatalog.RegistryValueNames[5]);
    }

    public void Start()
    {
        Current = this;
        _watcher.Start();
        _lastFingerprint = _snapshotRef?.ComputeWindowMetricsFingerprint();
    }

    public void Stop()
    {
        if (Current == this)
            Current = null;
        _watcher.Stop();
        _debounceTimer.Stop();
    }

    public void Dispose()
    {
        if (_disposed)
            return;
        _disposed = true;
        Stop();
        _watcher.Dispose();
        _snapshotRef = null;
    }

    public void SuppressExternalDirty(TimeSpan duration)
    {
        _suppressExternalDirtyUntilUtc = DateTime.UtcNow.Add(duration);
    }

    public void SyncFingerprintFromSnapshot(SettingsSnapshot snapshot)
    {
        _snapshotRef = snapshot;
        _lastFingerprint = snapshot.ComputeWindowMetricsFingerprint();
    }

    internal void RequestEvaluateAfterPageOperation()
    {
        if (_disposed)
            return;
        _dispatcherQueue.TryEnqueue(DispatcherQueuePriority.Low, EvaluateRegistryChange);
    }

    private void OnRegistrySignal()
    {
        if (_disposed)
            return;
        if (DateTime.UtcNow < _suppressExternalDirtyUntilUtc)
            return;
        if (ShouldDeferForPageOperation())
            return;

        _debounceTimer.Stop();
        _debounceTimer.Start();
    }

    private void EvaluateRegistryChange()
    {
        if (_disposed || _snapshotRef is null)
            return;
        if (DateTime.UtcNow < _suppressExternalDirtyUntilUtc)
            return;
        if (ShouldDeferForPageOperation())
        {
            RequestEvaluateAfterPageOperation();
            return;
        }

        var fingerprint = _snapshotRef.ComputeWindowMetricsFingerprint();
        if (string.Equals(fingerprint, _lastFingerprint, StringComparison.Ordinal))
            return;

        _lastFingerprint = fingerprint;
        if (!_snapshotRef.HasUnsavedInScope(TextScopeCatalog.SystemFontsScope))
            ExternalRegistryChanged?.Invoke(this, EventArgs.Empty);
    }

    private static bool ShouldDeferForPageOperation()
    {
        var gate = SettingsPageOperationGate.Current;
        return gate is not null && gate.ShouldDeferRegistryWatch;
    }
}
