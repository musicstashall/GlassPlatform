using Microsoft.UI.Dispatching;

namespace GlassSettings.Services;

/// <summary>Фаза serial-конвейера операций detail-страницы (см. docs/guides/GlassSettings-page-pipeline.md).</summary>
public enum SettingsPageOperationPhase
{
    Idle,
    Animating,
    RegistryMutation,
    Cosmetic,
    NoticeSync
}

/// <summary>
/// Serial-очередь фаз на <see cref="Controls.SettingsPageShell"/>:
/// Animating → RegistryMutation → Cosmetic → NoticeSync.
/// Политика и порядок — docs/guides/GlassSettings-page-pipeline.md.
/// </summary>
public sealed class SettingsPageOperationGate
{
    private readonly SemaphoreSlim _serial = new(1, 1);
    private readonly DispatcherQueue _dispatcherQueue;
    private bool _registryEvaluateDeferred;

    public static SettingsPageOperationGate? Current { get; private set; }

    public SettingsPageOperationPhase Phase { get; private set; } = SettingsPageOperationPhase.Idle;

    public bool IsBusy => Phase != SettingsPageOperationPhase.Idle;

    public bool ShouldDeferNotices =>
        Phase is SettingsPageOperationPhase.Animating
            or SettingsPageOperationPhase.RegistryMutation
            or SettingsPageOperationPhase.Cosmetic;

    public bool ShouldDeferRegistryWatch =>
        Phase is SettingsPageOperationPhase.Animating
            or SettingsPageOperationPhase.RegistryMutation
            or SettingsPageOperationPhase.Cosmetic
            or SettingsPageOperationPhase.NoticeSync;

    public bool ShouldBlockPageMutation =>
        Phase is SettingsPageOperationPhase.Animating
            or SettingsPageOperationPhase.RegistryMutation;

    public SettingsPageOperationGate(DispatcherQueue dispatcherQueue) =>
        _dispatcherQueue = dispatcherQueue;

    /// <summary>Выполнить цепочку фаз; параллельные вызовы ждут в очереди.</summary>
    public async Task RunSequenceAsync(Func<SettingsPageOperationSequence, Task> build)
    {
        await _serial.WaitAsync().ConfigureAwait(true);
        var previous = Current;
        Current = this;
        var sequence = new SettingsPageOperationSequence(this, _dispatcherQueue);
        try
        {
            await build(sequence).ConfigureAwait(true);
        }
        finally
        {
            Phase = SettingsPageOperationPhase.Idle;
            Current = previous;
            _serial.Release();
            TryScheduleDeferredRegistryEvaluate();
        }
    }

    internal void SetPhase(SettingsPageOperationPhase phase)
    {
        if (Phase == phase)
            return;

        NoticeMirrorTrace.WriteSession($"OperationGate: {Phase} → {phase}");
        Phase = phase;
    }

    internal void MarkRegistryEvaluateDeferred() => _registryEvaluateDeferred = true;

    private void TryScheduleDeferredRegistryEvaluate()
    {
        if (!_registryEvaluateDeferred)
            return;

        _registryEvaluateDeferred = false;
        RegistrySnapshotWatchService.Current?.RequestEvaluateAfterPageOperation();
        WindowMetricsRegistryWatchService.Current?.RequestEvaluateAfterPageOperation();
    }
}

/// <summary>Шаги одной транзакции внутри <see cref="SettingsPageOperationGate.RunSequenceAsync"/>.</summary>
public sealed class SettingsPageOperationSequence
{
    private readonly SettingsPageOperationGate _gate;
    private readonly DispatcherQueue _dispatcherQueue;

    internal SettingsPageOperationSequence(SettingsPageOperationGate gate, DispatcherQueue dispatcherQueue)
    {
        _gate = gate;
        _dispatcherQueue = dispatcherQueue;
    }

    public async Task AnimatingAsync(Func<Task> work)
    {
        _gate.SetPhase(SettingsPageOperationPhase.Animating);
        await work().ConfigureAwait(true);
    }

    public async Task RegistryMutationAsync(Func<Task> work)
    {
        _gate.SetPhase(SettingsPageOperationPhase.RegistryMutation);
        await work().ConfigureAwait(true);
        await SettingsInteractionDeferral.RunAfterUiSettledAsync(_dispatcherQueue).ConfigureAwait(true);
        await SettingsInteractionDeferral.RunAfterUiSettledAsync(_dispatcherQueue).ConfigureAwait(true);
        if (SettingsSession.Current.IsBaselineCaptured)
            RegistrySnapshotWatchService.Current?.SyncFingerprintFromSnapshot(SettingsSession.Current.RegistrySnapshot);
    }

    public async Task CosmeticAsync(Action work)
    {
        _gate.SetPhase(SettingsPageOperationPhase.Cosmetic);
        work();
        await Task.CompletedTask.ConfigureAwait(false);
    }

    public async Task CosmeticAsync(Func<Task> work)
    {
        _gate.SetPhase(SettingsPageOperationPhase.Cosmetic);
        await work().ConfigureAwait(true);
    }

    public async Task NoticeSyncAsync(Action work)
    {
        _gate.SetPhase(SettingsPageOperationPhase.NoticeSync);
        work();
        await Task.CompletedTask.ConfigureAwait(false);
    }
}
