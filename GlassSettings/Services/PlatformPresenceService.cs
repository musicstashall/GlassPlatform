using GlassSettings.Core.Platform;
using GlassSettings.Services;
using Microsoft.UI.Dispatching;
using System.Runtime.InteropServices;

namespace GlassSettings.Services;

/// <summary>
/// Подписка на snapshot платформы (mapping + event) с fallback poll без хоста.
/// </summary>
public sealed class PlatformPresenceService : IDisposable
{
    public static PlatformPresenceService Instance { get; } = new();

    private readonly object _gate = new();
    private DispatcherQueue? _dispatcher;
    private NativeHostInterop.PresenceSnapshot _snapshot;
    private uint _lastRevision;
    private int _subscriberCount;
    private bool _fallbackPollActive;
    private RegisteredWaitHandle? _presenceWait;
    private CancellationTokenSource? _presenceWaitCts;
    private CancellationTokenSource? _fallbackCts;
    private Timer? _fallbackTimer;

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
        // forceNotify без события: Home уже Refresh() в Loaded.
        RefreshSnapshot(forceNotify: true, raiseEvent: false);
        // EnsurePresenceWait — только после shell ready (см. StartPresenceWatchAfterShellReady).
        // Иначе WaitForSingleObject сразу сигналит → DispatchRefresh(raiseEvent) →
        // SchedulerRow.Refresh → AV на x86 до Low Ensure (лог: pump_normal_after, нет pump_low).
        SyncFallbackPoll();
    }

    /// <summary>После dismiss/Ensure: named wait + один PresenceChanged для карточек.</summary>
    public void StartPresenceWatchAfterShellReady()
    {
        EnsurePresenceWait();
        RefreshSnapshot(forceNotify: true, raiseEvent: true);
    }

    /// <summary>После start/stop: snapshot + fallback-poll (хост мог уйти без Presence event).</summary>
    public void NotifyLifecycleChanged()
    {
        SyncFallbackPoll();
        DispatchRefresh();
    }

    public void AddSubscriber()
    {
        Interlocked.Increment(ref _subscriberCount);
        SyncFallbackPoll();
    }

    public void RemoveSubscriber()
    {
        if (Interlocked.Decrement(ref _subscriberCount) < 0)
            Interlocked.Exchange(ref _subscriberCount, 0);
        SyncFallbackPoll();
    }

    public async Task WaitForOpenGlassOrchestrationAsync(
        NativeHostInterop.NativeOpenGlassHostUiState expected,
        TimeSpan timeout,
        CancellationToken cancellationToken = default)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (DateTime.UtcNow < deadline)
        {
            cancellationToken.ThrowIfCancellationRequested();
            RefreshSnapshot(forceNotify: false);
            if ((NativeHostInterop.NativeOpenGlassHostUiState)CurrentSnapshot.OpenGlassOrchestration == expected)
                return;
            await Task.Delay(250, cancellationToken).ConfigureAwait(false);
        }
    }

    public void Dispose()
    {
        _presenceWaitCts?.Cancel();
        _presenceWaitCts?.Dispose();
        _presenceWaitCts = null;
        _presenceWait?.Unregister(null);
        _presenceWait = null;
        _fallbackCts?.Cancel();
        _fallbackCts?.Dispose();
        _fallbackCts = null;
        _fallbackTimer?.Dispose();
        _fallbackTimer = null;
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
            while (!token.IsCancellationRequested)
            {
                _ = WaitForSingleObject(h, 5000);
                if (token.IsCancellationRequested)
                    break;
                DispatchRefresh();
            }
        }, token);
    }

    [DllImport("kernel32.dll")]
    private static extern uint WaitForSingleObject(IntPtr hHandle, uint dwMilliseconds);

    private void SyncFallbackPoll()
    {
        // Fallback-poll нужен, когда нет ни одного хоста (не только NativeArch).
        var needFallback = _subscriberCount > 0 && !ReloadRenderService.IsAnyGlassPlatformRunning();
        if (needFallback == _fallbackPollActive)
            return;

        _fallbackPollActive = needFallback;
        if (!needFallback)
        {
            _fallbackCts?.Cancel();
            _fallbackTimer?.Dispose();
            _fallbackTimer = null;
            return;
        }

        _fallbackCts?.Cancel();
        _fallbackCts = new CancellationTokenSource();
        _fallbackTimer?.Dispose();
        // Только через dispatcher: BuildFallback → Task Scheduler/COM; Timer TP (MTA) на x86 даёт AV.
        // dueTime != Zero: Attach/PostNav уже сделали Refresh; мгновенный повторный QueryGlassAutorun на x86 AV.
        _fallbackTimer = new Timer(
            _ => DispatchRefresh(),
            null,
            TimeSpan.FromSeconds(2),
            TimeSpan.FromSeconds(2));
    }

    private void DispatchRefresh()
    {
        var dq = _dispatcher;
        if (dq == null)
        {
            RefreshSnapshot(forceNotify: true);
            return;
        }

        dq.TryEnqueue(DispatcherQueuePriority.Normal, () => RefreshSnapshot(forceNotify: true));
    }

    private void RefreshSnapshot(bool forceNotify, bool raiseEvent = true)
    {
        NativeHostInterop.PresenceSnapshot snap;
        var fromMapping = NativeHostInterop.TryReadPresenceSnapshot(out snap);

        if (!fromMapping)
        {
            snap = BuildFallbackSnapshot();
        }

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
            PresenceChanged?.Invoke(this, EventArgs.Empty);
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

        return new NativeHostInterop.PresenceSnapshot
        {
            GlassState = glassState,
            OpenGlassOrchestration = ogOrch,
            OpenGlassPipelineActive = pipeline ? 1u : 0u,
            OpenGlassHostProcessRunning = hostRunning ? 1u : 0u,
            RootBackdropMode = effective,
        };
    }
}
