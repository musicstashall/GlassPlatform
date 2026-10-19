using GlassSettings.Core.Snapshot;
using Microsoft.UI.Dispatching;
using Microsoft.Win32;

namespace GlassSettings.Services;

/// <summary>
/// RegNotify на HKCU\DWM и HKLM\DWM: debounce + fingerprint, suppress при наших записях и baseline reload.
/// External-sync commit — после settle 1 с тишины (один таймер на весь DWM).
/// </summary>
public sealed class RegistrySnapshotWatchService : IDisposable
{
    private static readonly TimeSpan DebounceInterval = TimeSpan.FromMilliseconds(450);
    private static readonly TimeSpan ExternalSyncSettleInterval = TimeSpan.FromMilliseconds(1000);

    public static RegistrySnapshotWatchService? Current { get; private set; }

    /// <summary>Settle external-sync commit в очереди — session notice не открывается.</summary>
    public bool IsExternalSyncCommitPending => _externalSyncSettleTimer.IsRunning;

    private readonly DispatcherQueue _dispatcherQueue;
    private readonly RegistryValueChangeWatcher _dwmWatcherHkcu;
    private readonly RegistryValueChangeWatcher _dwmWatcherHklm;
    private readonly DispatcherQueueTimer _debounceTimer;
    private readonly DispatcherQueueTimer _externalSyncSettleTimer;

    private SettingsSnapshot? _snapshotRef;
    private string? _lastFingerprint;
    private DateTime _suppressExternalDirtyUntilUtc;
    private bool _disposed;

    public RegistrySnapshotWatchService(DispatcherQueue dispatcherQueue, SettingsSnapshot snapshot)
    {
        _dispatcherQueue = dispatcherQueue;
        _snapshotRef = snapshot;
        _debounceTimer = dispatcherQueue.CreateTimer();
        _debounceTimer.Interval = DebounceInterval;
        _debounceTimer.IsRepeating = false;
        _debounceTimer.Tick += OnDebounceTick;

        _externalSyncSettleTimer = dispatcherQueue.CreateTimer();
        _externalSyncSettleTimer.Interval = ExternalSyncSettleInterval;
        _externalSyncSettleTimer.IsRepeating = false;
        _externalSyncSettleTimer.Tick += OnExternalSyncSettleTick;

        _dwmWatcherHkcu = new RegistryValueChangeWatcher(
            dispatcherQueue,
            RegistryHive.CurrentUser,
            @"Software\Microsoft\Windows\DWM",
            OnDwmRegistrySignal);
        _dwmWatcherHklm = new RegistryValueChangeWatcher(
            dispatcherQueue,
            RegistryHive.LocalMachine,
            @"Software\Microsoft\Windows\DWM",
            OnDwmRegistrySignal);
    }

    public void Attach()
    {
        Current = this;
        _dwmWatcherHkcu.Start();
        _dwmWatcherHklm.Start();
        _lastFingerprint = _snapshotRef?.ComputeDwmRegistryFingerprint();
        RegistryWatchTrace.Write($"Attach: fingerprintLen={_lastFingerprint?.Length ?? 0}");
    }

    public void Detach()
    {
        if (Current == this)
            Current = null;

        _dwmWatcherHkcu.Stop();
        _dwmWatcherHklm.Stop();
        _debounceTimer.Stop();
        CancelExternalSyncSettle();
    }

    public void Dispose()
    {
        if (_disposed)
            return;

        _disposed = true;
        Detach();
        _debounceTimer.Tick -= OnDebounceTick;
        _externalSyncSettleTimer.Tick -= OnExternalSyncSettleTick;
        _dwmWatcherHkcu.Dispose();
        _dwmWatcherHklm.Dispose();
        _snapshotRef = null;
    }

    public void SuppressExternalDirty(TimeSpan duration)
    {
        _suppressExternalDirtyUntilUtc = DateTime.UtcNow.Add(duration);
        CancelExternalSyncSettle();
        RegistryWatchTrace.Write($"SuppressExternalDirty: {duration.TotalMilliseconds:0}ms");
    }

    public void SyncFingerprintFromSnapshot(SettingsSnapshot snapshot)
    {
        _snapshotRef = snapshot;
        _lastFingerprint = snapshot.ComputeDwmRegistryFingerprint();
        CancelExternalSyncSettle();
        RegistryWatchTrace.Write("SyncFingerprintFromSnapshot: cancel settle");
    }

    /// <summary>Тот же контракт, что SyncFingerprintFromSnapshot, но отпечаток уже посчитан вне UI.</summary>
    public void AdoptDwmFingerprint(SettingsSnapshot snapshot, string fingerprint)
    {
        _snapshotRef = snapshot;
        _lastFingerprint = fingerprint;
        CancelExternalSyncSettle();
    }

    /// <summary>После page operation sequence — один evaluate, если debounce был отложен.</summary>
    internal void RequestEvaluateAfterPageOperation()
    {
        if (_disposed)
            return;

        _dispatcherQueue.TryEnqueue(DispatcherQueuePriority.Low, EvaluateRegistryChange);
    }

    private static bool ShouldDeferForPageOperation()
    {
        var gate = SettingsPageOperationGate.Current;
        if (gate is null || !gate.ShouldDeferRegistryWatch)
            return false;

        gate.MarkRegistryEvaluateDeferred();
        RegistryWatchTrace.Write($"Evaluate: skip — page operation phase {gate.Phase}");
        return true;
    }

    private void OnDwmRegistrySignal() =>
        _dispatcherQueue.TryEnqueue(ScheduleDebounce);

    private void ScheduleDebounce()
    {
        if (_disposed)
            return;

        RegistryWatchTrace.Write("ScheduleDebounce");
        _debounceTimer.Stop();
        _debounceTimer.Start();
    }

    private void OnDebounceTick(DispatcherQueueTimer sender, object args)
    {
        sender.Stop();
        if (_disposed)
            return;

        EvaluateRegistryChange();
    }

    private void OnExternalSyncSettleTick(DispatcherQueueTimer sender, object args)
    {
        sender.Stop();
        if (_disposed)
            return;

        RegistryWatchTrace.Write("ExternalSyncSettleTick");
        CommitExternalSyncIfStable();
    }

    private void EvaluateRegistryChange()
    {
        var session = SettingsSession.Current;
        if (!session.IsBaselineCaptured || session.IsShuttingDown || _snapshotRef is null)
        {
            RegistryWatchTrace.Write(
                $"Evaluate: skip baseline={session.IsBaselineCaptured}, shutdown={session.IsShuttingDown}, snapNull={_snapshotRef is null}");
            return;
        }

        if (ShouldDeferForPageOperation())
            return;

        var fingerprint = _snapshotRef.ComputeDwmRegistryFingerprint();
        if (IsSuppressed())
        {
            _lastFingerprint = fingerprint;
            CancelExternalSyncSettle();
            RegistryWatchTrace.Write("Evaluate: suppressed — fingerprint synced, settle cancelled");
            return;
        }

        if (string.Equals(_lastFingerprint, fingerprint, StringComparison.Ordinal))
        {
            RegistryWatchTrace.Write("Evaluate: skip — fingerprint unchanged");
            return;
        }

        var hasUnsaved = session.HasRegistryWatchSemanticDirty();

        if (!hasUnsaved)
        {
            _lastFingerprint = fingerprint;
            CancelExternalSyncSettle();
            RegistryWatchTrace.Write("Evaluate: fingerprint changed, semantic clean — no settle");
            return;
        }

        RegistryWatchTrace.WriteSemanticDirtyDetails(session, "Evaluate");
        _lastFingerprint = fingerprint;

        if (SettingsInteractionDeferral.IsSliderGestureActive)
        {
            CancelExternalSyncSettle();
            RegistryWatchTrace.Write("Evaluate: semantic dirty, skip settle — slider gesture active");
            return;
        }

        if (session.ShouldBlockExternalRegistrySync())
        {
            CancelExternalSyncSettle();
            RegistryWatchTrace.Write("Evaluate: semantic dirty, skip settle — user-origin DWM dirty");
            return;
        }

        RegistryWatchTrace.Write("Evaluate: semantic dirty — ArmExternalSyncSettle");
        ArmExternalSyncSettle();
    }

    private void CommitExternalSyncIfStable()
    {
        var session = SettingsSession.Current;
        if (!session.IsBaselineCaptured || session.IsShuttingDown || _snapshotRef is null)
        {
            RegistryWatchTrace.Write("ExternalSyncSettleTick: skip — session not ready");
            return;
        }

        if (SettingsPageOperationGate.Current is { ShouldDeferRegistryWatch: true } gate)
        {
            gate.MarkRegistryEvaluateDeferred();
            RegistryWatchTrace.Write($"ExternalSyncSettleTick: skip — page operation phase {gate.Phase}");
            return;
        }

        if (IsSuppressed())
        {
            RegistryWatchTrace.Write("ExternalSyncSettleTick: skip — suppressed");
            return;
        }

        if (SettingsInteractionDeferral.IsSliderGestureActive)
        {
            RegistryWatchTrace.Write("ExternalSyncSettleTick: skip — slider gesture active");
            return;
        }

        if (!session.HasRegistryWatchSemanticDirty())
        {
            _lastFingerprint = _snapshotRef.ComputeDwmRegistryFingerprint();
            RegistryWatchTrace.Write("ExternalSyncSettleTick: skip — semantic clean");
            return;
        }

        if (session.ShouldBlockExternalRegistrySync())
        {
            RegistryWatchTrace.Write("ExternalSyncSettleTick: skip — user-origin DWM dirty");
            return;
        }

        RegistryWatchTrace.WriteSemanticDirtyDetails(session, "ExternalSyncSettleTick");
        RegistryWatchTrace.Write("ExternalSyncSettleTick: calling ApplyExternalRegistrySyncFromWatch");
        session.ApplyExternalRegistrySyncFromWatch();
    }

    private void ArmExternalSyncSettle()
    {
        _externalSyncSettleTimer.Stop();
        _externalSyncSettleTimer.Start();
        RegistryWatchTrace.Write($"ArmExternalSyncSettle: {_externalSyncSettleTimer.Interval.TotalMilliseconds:0}ms");
    }

    private void CancelExternalSyncSettle()
    {
        if (!_externalSyncSettleTimer.IsRunning)
            return;

        _externalSyncSettleTimer.Stop();
        RegistryWatchTrace.Write("CancelExternalSyncSettle");
    }

    private bool IsSuppressed() => DateTime.UtcNow < _suppressExternalDirtyUntilUtc;
}
