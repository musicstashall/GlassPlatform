using GlassSettings.Controls;
using GlassSettings.Core.Platform;
using GlassSettings.Services;
using Microsoft.UI.Dispatching;
using System.Runtime.InteropServices;

namespace GlassSettings.Services;

/// <summary>
/// Event-driven presence watcher: maintains a subscriber list and invokes all subscribers
/// when platform state changes. Single source of truth for task/service state.
///
/// When platform is RUNNING: reads from shared memory (native PlatformPresenceMonitor writes there).
/// When platform is STOPPED: performs direct COM/SCM queries (fallback) on STA thread.
/// </summary>
public sealed class PlatformPresenceService : IDisposable
{
    public static PlatformPresenceService Instance { get; } = new();

    private readonly object _gate = new();
    private readonly List<Action<NativeHostInterop.PresenceSnapshot>> _subscribers = new();
    private DispatcherQueue? _dispatcher;
    private NativeHostInterop.PresenceSnapshot _snapshot;
    private uint _lastRevision;
    private CancellationTokenSource? _presenceWaitCts;

    // Watcher STA thread — always active while subscribers exist.
    private Thread? _watcherThread;
    private CancellationTokenSource? _watcherCts;
    private ManualResetEventSlim? _watcherSignal;
    private NativeHostInterop.PresenceSnapshot _lastWatcherSnapshot;
    private static int s_watcherRevision;

    private const int WatcherPollIntervalMs = 500;

    /// <summary>Fired on UI thread when snapshot changes (backward compat).</summary>
    public event EventHandler? PresenceChanged;

    public NativeHostInterop.PresenceSnapshot CurrentSnapshot
    {
        get
        {
            lock (_gate)
                return _snapshot;
        }
    }

    public void Attach(DispatcherQueue dispatcher)
    {
        _dispatcher = dispatcher;
        RefreshSnapshotFromSharedMemory(forceNotify: true, raiseEvent: false);
        SyncWatcher();
    }

    public void StartPresenceWatchAfterShellReady()
    {
        EnsurePresenceWait();
        RefreshSnapshotFromSharedMemory(forceNotify: true, raiseEvent: true);
    }

    public void NotifyLifecycleChanged()
    {
        NativeHostInterop.InvalidateOGCache();
        SignalWatcherRefresh();
        // Watcher молчит, если снимок не изменился (неудачный старт). Тумблер всё равно
        // нужно снять с busy — иначе он остаётся выключенным.
        var dq = _dispatcher;
        if (dq is null)
        {
            RaisePresenceTimed();
            return;
        }

        if (dq.HasThreadAccess)
            RaisePresenceTimed();
        else
            dq.TryEnqueue(RaisePresenceTimed);
    }

    public void Subscribe(Action<NativeHostInterop.PresenceSnapshot> callback)
    {
        lock (_gate)
            _subscribers.Add(callback);
        SyncWatcher();
        var snap = CurrentSnapshot;
        if (snap.Revision > 0)
        {
            var dq = _dispatcher;
            if (dq != null)
                dq.TryEnqueue(() => callback(snap));
        }
    }

    public void Unsubscribe(Action<NativeHostInterop.PresenceSnapshot> callback)
    {
        lock (_gate)
            _subscribers.Remove(callback);
        SyncWatcher();
    }

    public void AddSubscriber()
    {
        var first = Interlocked.Increment(ref _subscriberCount) == 1;
        SyncWatcher();
        if (first)
        {
            // Always fire PresenceChanged on first subscriber — ensures initial state sync.
            var dq = _dispatcher;
            if (dq != null)
            {
                dq.TryEnqueue(() =>
                {
                    PresenceChanged?.Invoke(this, EventArgs.Empty);
                });
            }
            return;
        }
    }

    public void RemoveSubscriber()
    {
        if (Interlocked.Decrement(ref _subscriberCount) < 0)
            Interlocked.Exchange(ref _subscriberCount, 0);
        SyncWatcher();
    }

    private int _subscriberCount;

    public async Task WaitForOpenGlassOrchestrationAsync(
        NativeHostInterop.NativeOpenGlassHostUiState expected,
        TimeSpan timeout,
        CancellationToken cancellationToken = default)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (DateTime.UtcNow < deadline)
        {
            cancellationToken.ThrowIfCancellationRequested();
            RefreshSnapshotFromSharedMemory(forceNotify: false);
            if ((NativeHostInterop.NativeOpenGlassHostUiState)CurrentSnapshot.OpenGlassOrchestration == expected)
                return;
            await Task.Delay(250, cancellationToken).ConfigureAwait(false);
        }
    }

    /// <summary>
    /// Единый источник истины для состояния задачи платформы.
    /// </summary>
    public PlatformTaskState QueryTaskState(PlatformSchedulerKind kind, GlassPlatformArch arch)
    {
        var snap = CurrentSnapshot;
        if (snap.Revision > 0)
        {
            var result = kind switch
            {
                PlatformSchedulerKind.Glass => new PlatformTaskState(
                    TaskExists: arch == GlassPlatformArch.X64
                        ? snap.GlassTaskExistsX64 != 0
                        : snap.GlassTaskExistsX86 != 0,
                    Lifecycle: (NativeHostInterop.InjectLifecycleUi)(arch == GlassPlatformArch.X64
                        ? snap.InjectLifecycleUiX64 : snap.InjectLifecycleUiX86),
                    Autorun: (GlassAutorunUiState)(arch == GlassPlatformArch.X64
                        ? snap.GlassAutorunStateX64 : snap.GlassAutorunStateX86)),
                PlatformSchedulerKind.OpenGlass => new PlatformTaskState(
                    TaskExists: snap.OgLegacyTaskExists != 0 || snap.OgCurrentTaskExists != 0,
                    Lifecycle: snap.OpenGlassOrchestration == (int)NativeHostInterop.NativeOpenGlassHostUiState.Running
                        ? NativeHostInterop.InjectLifecycleUi.Running
                        : NativeHostInterop.InjectLifecycleUi.Idle,
                    Autorun: (GlassAutorunUiState)snap.OpenGlassOrchestration),
                _ => PlatformTaskState.Default
            };
            return result;
        }

        var direct = QueryTaskStateDirect(kind, arch);
        return direct;
    }

    public PlatformTaskState QueryTaskStateDirect(PlatformSchedulerKind kind, GlassPlatformArch arch)
    {
        return kind switch
        {
            PlatformSchedulerKind.Glass => new PlatformTaskState(
                TaskExists: NativeHostInterop.GlassAutorunTaskExistsNative(arch),
                Lifecycle: NativeHostInterop.QueryInjectLifecycleUi(arch),
                Autorun: GlassPlatformService.QueryGlassAutorunUiState(arch)),
            PlatformSchedulerKind.OpenGlass => new PlatformTaskState(
                TaskExists: NativeHostInterop.LegacyHostTaskExists(),
                Lifecycle: NativeHostInterop.QueryInjectLifecycleUi(GlassPlatformNames.NativeArch),
                Autorun: MapOgOrchestrationToAutorun(NativeHostInterop.QueryOpenGlassHostUiState())),
            _ => PlatformTaskState.Default
        };
    }

    private static GlassAutorunUiState MapOgOrchestrationToAutorun(NativeHostInterop.NativeOpenGlassHostUiState st) =>
        st switch
        {
            NativeHostInterop.NativeOpenGlassHostUiState.Running => GlassAutorunUiState.Running,
            NativeHostInterop.NativeOpenGlassHostUiState.Ready => GlassAutorunUiState.Ready,
            NativeHostInterop.NativeOpenGlassHostUiState.Disabled => GlassAutorunUiState.Disabled,
            _ => GlassAutorunUiState.NoTask
        };

    public void Dispose()
    {
        _presenceWaitCts?.Cancel();
        _presenceWaitCts?.Dispose();
        _presenceWaitCts = null;

        _watcherCts?.Cancel();
        _watcherSignal?.Set();
        _watcherThread?.Join(2000);
        _watcherCts?.Dispose();
        _watcherCts = null;
        _watcherSignal?.Dispose();
        _watcherSignal = null;
        _watcherThread = null;
    }

    private void EnsurePresenceWait()
    {
        if (_presenceWaitCts != null)
            return;

        var h = NativeHostInterop.OpenPresenceChangedEvent();
        if (h == IntPtr.Zero)
            return;

        _presenceWaitCts = new CancellationTokenSource();
        var token = _presenceWaitCts.Token;
        _ = Task.Run(() =>
        {
            try
            {
                while (!token.IsCancellationRequested)
                {
                    _ = WaitForSingleObject(h, 5000);
                    if (token.IsCancellationRequested)
                        break;
                    SignalWatcherRefresh();
                }
            }
            catch (OperationCanceledException) { }
        }, token);
    }

    [DllImport("kernel32.dll")]
    private static extern uint WaitForSingleObject(IntPtr hHandle, uint dwMilliseconds);

    private void SyncWatcher()
    {
        // Watcher always runs while GS is open — ensures snapshot is always current
        // even when SystemPage is active and HomePage is unsubscribed.
        if (_watcherThread != null)
            return;

        EnsureWatcherThread();
    }

    private void EnsureWatcherThread()
    {
        if (_watcherThread != null)
            return;

        _lastWatcherSnapshot = default;
        _watcherCts = new CancellationTokenSource();
        _watcherSignal = new ManualResetEventSlim(false);
        _watcherThread = new Thread(WatcherThreadProc)
        {
            IsBackground = true,
            Name = "PlatformPresence.Watcher"
        };
        _watcherThread.SetApartmentState(ApartmentState.STA);
        _watcherThread.Start();
    }

    private void StopWatcherThread()
    {
        _watcherCts?.Cancel();
        _watcherSignal?.Set();
        _watcherThread?.Join(2000);
        _watcherCts?.Dispose();
        _watcherCts = null;
        _watcherSignal?.Dispose();
        _watcherSignal = null;
        _watcherThread = null;
    }

    private void WatcherThreadProc()
    {
        var token = _watcherCts!.Token;
        try
        {
            while (!token.IsCancellationRequested)
            {
                _watcherSignal!.Wait(WatcherPollIntervalMs);
                if (token.IsCancellationRequested)
                    break;
                _watcherSignal.Reset();

                // Always build via direct COM/SCM queries.
                // Shared memory from native monitor (DWM process) cannot see dual-arch Glass fields
                // due to session isolation — they are always 0. The GS process CAN query everything
                // correctly via P/Invoke, so we always use the direct-query path.
                var snap = BuildFallbackSnapshot();

                if (SnapshotEquals(snap, _lastWatcherSnapshot))
                {
                    continue;
                }

                _lastWatcherSnapshot = snap;

                lock (_gate)
                {
                    _snapshot = snap;
                }

                NotifySubscribers(snap);
            }
        }
        catch (OperationCanceledException) { }
    }

    private static bool SnapshotEquals(
        NativeHostInterop.PresenceSnapshot a,
        NativeHostInterop.PresenceSnapshot b)
    {
        return a.Revision != 0
            && a.GlassState == b.GlassState
            && a.OpenGlassOrchestration == b.OpenGlassOrchestration
            && a.OpenGlassHostProcessRunning == b.OpenGlassHostProcessRunning
            && a.GlassAutorunState == b.GlassAutorunState
            && a.InjectLifecycleUi == b.InjectLifecycleUi
            && a.StopEventSignaled == b.StopEventSignaled
            && a.GlassTaskExistsX64 == b.GlassTaskExistsX64
            && a.GlassTaskExistsX86 == b.GlassTaskExistsX86
            && a.InjectLifecycleUiX64 == b.InjectLifecycleUiX64
            && a.InjectLifecycleUiX86 == b.InjectLifecycleUiX86
            && a.GlassAutorunStateX64 == b.GlassAutorunStateX64
            && a.GlassAutorunStateX86 == b.GlassAutorunStateX86
            && a.OgLegacyTaskExists == b.OgLegacyTaskExists
            && a.OgCurrentTaskExists == b.OgCurrentTaskExists;
    }

    private void NotifySubscribers(NativeHostInterop.PresenceSnapshot snap)
    {
        Action<NativeHostInterop.PresenceSnapshot>[] snapshot;
        lock (_gate)
            snapshot = _subscribers.ToArray();

        var dq = _dispatcher;
        if (dq != null)
        {
            dq.TryEnqueue(() =>
            {
                foreach (var cb in snapshot)
                {
                    try { cb(snap); }
                    catch { }
                }
                // Always fire PresenceChanged — event-based subscribers (HomePage) rely on this.
                RaisePresenceTimed();
            });
        }
    }

    private void SignalWatcherRefresh()
    {
        _watcherSignal?.Set();
    }

    private void RefreshSnapshotFromSharedMemory(bool forceNotify, bool raiseEvent = true)
    {
        // Always use direct queries — shared memory from DWM cannot see dual-arch Glass fields.
        var snap = BuildFallbackSnapshot();

        var changed = false;
        lock (_gate)
        {
            if (snap.Revision != _lastRevision || forceNotify)
            {
                _lastRevision = snap.Revision;
                _snapshot = snap;
                changed = true;
            }
        }

        if (changed && raiseEvent)
            RaisePresenceTimed();
    }

    private void RaisePresenceTimed()
    {
        var handlers = PresenceChanged?.GetInvocationList();
        if (handlers is null)
            return;

        foreach (var handler in handlers)
        {
            try
            {
                ((EventHandler)handler)(this, EventArgs.Empty);
            }
            catch
            {
                // Подписчик не должен срывать остальных.
            }
        }
    }

    private static NativeHostInterop.PresenceSnapshot BuildFallbackSnapshot()
    {
        var noBackdrop = RootNoBackdropPolicy.ReadEffectiveNoBackdrop();
        var pipeline = NativeHostInterop.IsOpenGlassBackdropPipelineActive();

        uint effective;
        if (noBackdrop != 0)
            effective = 0;
        else if (pipeline)
            effective = 1;
        else
            effective = 2;

        var glassState = (int)NativeHostInterop.QueryGlassAutorunUiState();
        var ogOrch = (int)NativeHostInterop.QueryOpenGlassHostUiState();
        var hostRunning = NativeHostInterop.IsOpenGlassHostProcessRunning();
        var injectLifecycle = (int)NativeHostInterop.QueryInjectLifecycleUi(GlassPlatformNames.NativeArch);
        var stopSignaled = HomeStatusSummary.IsStopEventSignaled(GlassPlatformNames.NativeArch);

        var taskX64 = NativeHostInterop.GlassAutorunTaskExistsNative(GlassPlatformArch.X64);
        var taskX86 = NativeHostInterop.GlassAutorunTaskExistsNative(GlassPlatformArch.X86);
        var lifeX64 = (int)NativeHostInterop.QueryInjectLifecycleUi(GlassPlatformArch.X64);
        var lifeX86 = (int)NativeHostInterop.QueryInjectLifecycleUi(GlassPlatformArch.X86);
        var autoX64 = (int)NativeHostInterop.QueryGlassAutorunUiState(GlassPlatformArch.X64);
        var autoX86 = (int)NativeHostInterop.QueryGlassAutorunUiState(GlassPlatformArch.X86);

        var ogLegacy = NativeHostInterop.LegacyHostTaskExists() ? 1u : 0u;
        var ogCurrent = ogOrch != 0 ? 1u : 0u;

        var revision = (uint)Interlocked.Increment(ref s_watcherRevision);
        if (revision == 0)
            revision = (uint)Interlocked.Increment(ref s_watcherRevision);

        return new NativeHostInterop.PresenceSnapshot
        {
            Revision = revision,
            GlassState = glassState,
            OpenGlassOrchestration = ogOrch,
            OpenGlassPipelineActive = pipeline ? 1u : 0u,
            OpenGlassHostProcessRunning = hostRunning ? 1u : 0u,
            RootBackdropMode = effective,
            GlassAutorunState = glassState,
            InjectLifecycleUi = injectLifecycle,
            StopEventSignaled = stopSignaled ? 1u : 0u,
            GlassTaskExistsX64 = taskX64 ? 1u : 0u,
            GlassTaskExistsX86 = taskX86 ? 1u : 0u,
            InjectLifecycleUiX64 = lifeX64,
            InjectLifecycleUiX86 = lifeX86,
            GlassAutorunStateX64 = autoX64,
            GlassAutorunStateX86 = autoX86,
            OgLegacyTaskExists = ogLegacy,
            OgCurrentTaskExists = ogCurrent,
        };
    }
}
