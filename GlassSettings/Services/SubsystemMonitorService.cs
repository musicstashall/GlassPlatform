using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.UI.Dispatching;
using GlassSettings.Core.Platform;

namespace GlassSettings.Services;

/// <summary>
/// Сервис мониторинга lifecycle-состояний подсистем.
/// Активен только во время переходных процессов (Starting/Stopping).
/// Опрашивает shared memory каждые 250мс, шлёт обновления в UI при изменении.
/// </summary>
public sealed class SubsystemMonitorService
{
    public static SubsystemMonitorService Instance { get; } = new();

    private const int PollIntervalMs = 250;
    private const int SettleTimeoutMs = 30000;

    private readonly object _gate = new();
    private readonly HashSet<object> _subscribers = new();
    private DispatcherQueue? _dispatcher;

    private CancellationTokenSource? _cts;
    private Task? _loopTask;
    private Dictionary<uint, SubsystemSnapshot> _lastSnapshots = new();
    private bool _active;

    private SubsystemMonitorService() { }

    /// <summary>
    /// Подписка на обновления lifecycle-состояний подсистем.
    /// Вызывать из UI-потока. Сервис стартует при первом подписчике.
    /// </summary>
    public void Subscribe(object subscriber, DispatcherQueue queue)
    {
        lock (_gate)
        {
            _dispatcher = queue;
            if (_subscribers.Add(subscriber) && _subscribers.Count == 1)
                StartLoop();
        }
    }

    /// <summary>
    /// Отписка. Сервис останавливается при последнем отписавшемся.
    /// </summary>
    public void Unsubscribe(object subscriber)
    {
        lock (_gate)
        {
            if (_subscribers.Remove(subscriber) && _subscribers.Count == 0)
                StopLoop();
        }
    }

    /// <summary>
    /// Принудительный запуск мониторинга (вызывать при обнаружении Starting).
    /// </summary>
    public void EnsureActive()
    {
        lock (_gate)
        {
            if (!_active && _subscribers.Count > 0)
                StartLoop();
        }
    }

    /// <summary>
    /// Последний снимок (thread-safe).
    /// </summary>
    public IReadOnlyList<SubsystemSnapshot> Latest { get; private set; } = Array.Empty<SubsystemSnapshot>();

    /// <summary>
    /// Событие обновления lifecycle-состояний (вызывается в UI-потоке).
    /// </summary>
    public event Action<IReadOnlyList<SubsystemSnapshot>>? SubsystemChanged;

    private void StartLoop()
    {
        if (_active) return;
        _active = true;
        _cts = new CancellationTokenSource();
        var ct = _cts.Token;
        _loopTask = Task.Run(() => PollLoop(ct), ct);
    }

    private void StopLoop()
    {
        if (!_active) return;
        _active = false;
        _cts?.Cancel();
        _cts = null;
        _loopTask = null;
        _lastSnapshots.Clear();
    }

    private void PollLoop(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            try
            {
                Thread.Sleep(PollIntervalMs);
                if (ct.IsCancellationRequested) break;

                var snapshots = CaptureSnapshots();
                if (snapshots.Count == 0)
                {
                    // Нет активных PID — все выгружены, сервис может остановиться.
                    if (_lastSnapshots.Count > 0)
                    {
                        _lastSnapshots.Clear();
                        Latest = Array.Empty<SubsystemSnapshot>();
                        NotifySubscribers(Latest);
                    }
                    break;
                }

                if (!HasChanges(snapshots))
                    continue;

                _lastSnapshots = snapshots;
                var result = snapshots.Values.ToArray();
                Latest = result;
                NotifySubscribers(result);
            }
            catch (OperationCanceledException)
            {
                break;
            }
            catch
            {
                // Игнорируем ошибки чтения shared memory.
            }
        }

        lock (_gate)
        {
            if (_active && _subscribers.Count == 0)
                _active = false;
        }
    }

    private static Dictionary<uint, SubsystemSnapshot> CaptureSnapshots()
    {
        var result = new Dictionary<uint, SubsystemSnapshot>();

        var lifeX64 = NativeHostInterop.QueryInjectLifecycleUi(Core.Platform.GlassPlatformArch.X64);
        var lifeX86 = NativeHostInterop.QueryInjectLifecycleUi(Core.Platform.GlassPlatformArch.X86);
        if (lifeX64 == NativeHostInterop.InjectLifecycleUi.Idle
            && lifeX86 == NativeHostInterop.InjectLifecycleUi.Idle)
            return result;

        if (!NativeHostInterop.TryReadInjectMapSlots(out var slots))
            return result;

        foreach (var slot in slots)
        {
            if (slot.Pid == 0) continue;
            if (slot.Kind != NativeHostInterop.InjectKind.ActiveGui) continue;

            result[slot.Pid] = new SubsystemSnapshot
            {
                Pid = slot.Pid,
                DiagFlags = slot.DiagFlags,
                DiagLifecycle = slot.DiagLifecycle,
                ModuleBits = slot.ModuleBits,
                SubclassWindowTotal = slot.SubclassWindowTotal,
                State = slot.State,
            };
        }

        return result;
    }

    private bool HasChanges(Dictionary<uint, SubsystemSnapshot> current)
    {
        if (current.Count != _lastSnapshots.Count) return true;

        foreach (var kvp in current)
        {
            if (!_lastSnapshots.TryGetValue(kvp.Key, out var prev)) return true;
            if (!SnapshotEquals(kvp.Value, prev)) return true;
        }

        return false;
    }

    private static bool SnapshotEquals(SubsystemSnapshot a, SubsystemSnapshot b)
        => a.Pid == b.Pid
        && a.DiagFlags == b.DiagFlags
        && a.DiagLifecycle == b.DiagLifecycle
        && a.ModuleBits == b.ModuleBits
        && a.SubclassWindowTotal == b.SubclassWindowTotal
        && a.State == b.State;

    private void NotifySubscribers(IReadOnlyList<SubsystemSnapshot> snapshots)
    {
        var dq = _dispatcher;
        if (dq is null) return;

        dq.TryEnqueue(() => SubsystemChanged?.Invoke(snapshots));
    }
}

/// <summary>
/// Иммутабельный снимок lifecycle-состояния подсистем для одного процесса.
/// </summary>
public sealed class SubsystemSnapshot
{
    public uint Pid { get; init; }
    public uint DiagFlags { get; init; }
    public uint DiagLifecycle { get; init; }
    public uint ModuleBits { get; init; }
    public uint SubclassWindowTotal { get; init; }
    public NativeHostInterop.InjectState State { get; init; }
}
