using System.Runtime.InteropServices;
using System.Text;
using GlassSettings.Core.Platform;
using GlassSettings.Strings;
using Microsoft.UI.Dispatching;

namespace GlassSettings.Services;

public enum InjectRuntimeBeacon
{
    Active = 0,
    Rejected = 1,
    Residual = 2,
}

public sealed class InjectRuntimeEntry
{
    public uint Pid { get; init; }
    public string ExeName { get; init; } = string.Empty;
    public InjectRuntimeBeacon Beacon { get; init; }
    public string ModulesText { get; init; } = string.Empty;
    public uint ModuleBits { get; init; }
    public byte Path { get; init; }
    public NativeHostInterop.InjectArch Arch { get; init; }
    public uint DiagFlags { get; init; }
    public uint DiagLifecycle { get; init; }
    public uint SubclassWindowTotal { get; init; }
    public ulong SubclassKindMask { get; init; }
    public NativeHostInterop.InjectState State { get; init; }

    public string ArchLabel => Arch == NativeHostInterop.InjectArch.X86 ? "x86" : "x64";

    /// <summary>
    /// Суффикс архитектуры в UI: только на GS x64 для Wow64-процессов — « (x86)».
    /// На GS x86 и для нативных x64 — пусто.
    /// </summary>
    public string ArchDisplaySuffix =>
        Environment.Is64BitProcess && Arch == NativeHostInterop.InjectArch.X86
            ? " (x86)"
            : string.Empty;

    public string FormatProcessTitle() => ExeName + ArchDisplaySuffix;

    /// <summary>Строка meta в detail-диалоге: «PID N» или «PID N · (x86)».</summary>
    public string FormatPidMeta() =>
        ArchDisplaySuffix.Length == 0
            ? $"PID {Pid}"
            : $"PID {Pid} · (x86)";

    /// <summary>Soft unload (событие) или Force FreeLibrary — при любом ненулевом ModuleBits.</summary>
    public bool CanRequestUnload =>
        ModuleBits != 0 && !NeedsX86ForceAgentWithoutPackage;

    /// <summary>
    /// GS x64 + residual/off: Force через SysWOW64 rundll32 требует x86 InstallPath.
    /// Soft UnloadRequest для живых injectee работает без x86-пакета.
    /// </summary>
    public bool NeedsX86ForceAgentWithoutPackage =>
        Environment.Is64BitProcess
        && Arch == NativeHostInterop.InjectArch.X86
        && RequiresForceUnloadPath
        && !GlassModulePath.IsPlatformInstalled(GlassPlatformArch.X86);

    private bool RequiresForceUnloadPath =>
        Beacon == InjectRuntimeBeacon.Residual
        || !ReloadRenderService.IsAnyGlassPlatformRunning();

    public string? UnloadDisabledReason =>
        NeedsX86ForceAgentWithoutPackage
            ? GlassSettingsResources.Get("Runtime_UnloadNeedsX86Package")
            : null;
}

public sealed class InjectRuntimeSnapshot
{
    public static InjectRuntimeSnapshot Empty { get; } = new();

    public IReadOnlyList<InjectRuntimeEntry> Entries { get; init; } = Array.Empty<InjectRuntimeEntry>();
    public int ActiveCount { get; init; }
    public int RejectedCount { get; init; }
    public int ResidualCount { get; init; }
    public bool PlatformRunning { get; init; }
    public bool HasResiduals => ResidualCount > 0;
}

/// <summary>
/// Общий фоновый монитор InjectMap + Toolhelp. Подписчики (Home / Processes) через refcount.
/// </summary>
public sealed class InjectRuntimeMonitor
{
    public static InjectRuntimeMonitor Instance { get; } = new();

    private readonly object _gate = new();
    private readonly HashSet<object> _subscribers = [];
    private DispatcherQueueTimer? _timer;
    private DispatcherQueue? _queue;
    private int _generation;
    private bool _inFlight;
    private InjectRuntimeSnapshot _latest = InjectRuntimeSnapshot.Empty;
    private bool _lastHadResiduals;
    private bool _wasPlatformRunning;
    private bool _residualSweepActive;
    /// <summary>Пик количества slots при живой платформе (для детекта shutdown race — резкое уменьшение slots).</summary>
    private int _peakSlots;
    /// <summary>Один раз при старте монитора (платформа off): полный Toolhelp-scan residual.</summary>
    private bool _idlePlatformProbePending = true;
    private bool _pendingColdProbeCompletion;
    /// <summary>Первый CaptureSnapshot после StartPolling — без cheap-idle (читать карту / cold-probe).</summary>
    private bool _forceFullCapturePending;
    /// <summary>PID из последней живой карты — bridge, пока map закрыта / residual-sweep.</summary>
    private readonly HashSet<uint> _bridgePids = [];
    /// <summary>
    /// PID с ModuleBits≠0 из предыдущих снимков — держим в scan/UI, пока Module32
    /// не подтвердит выгрузку (дыра: PID вышел из карты → строка пропадала).
    /// </summary>
    private readonly HashSet<uint> _stickyPids = [];
    /// <summary>Пустые residual-сканы подряд; не рвём sweep на первом miss (дыра map→scan).</summary>
    private int _residualEmptyStreak;

    public event Action<InjectRuntimeSnapshot>? SnapshotChanged;

    public InjectRuntimeSnapshot Latest
    {
        get
        {
            lock (_gate)
                return _latest;
        }
    }

    public bool LastHadResiduals
    {
        get
        {
            lock (_gate)
                return _lastHadResiduals;
        }
    }

    /// <summary>
    /// Cold-start под заставкой: дешёвый idle-снимок без Toolhelp.
    /// <see cref="_idlePlatformProbePending"/> не сбрасываем — полный Process32/Module32
    /// делает первый <see cref="CaptureSnapshot"/> после Subscribe (платформа off).
    /// </summary>
    public void PublishColdStartIdleSnapshot()
    {
        // Без OpenMutex/IsGlassPlatformRunning: на x86 вызов сразу после PostNav убивал процесс
        // (after_PostNav есть, idle done нет). Реальный mutex — в polling после dismiss.
        const bool platformRunning = false;
        InjectRuntimeSnapshot snapshot;
        lock (_gate)
        {
            // Было false → cold-probe никогда не выполнялся (пустой список при residual DLL).
            _idlePlatformProbePending = true;
            _pendingColdProbeCompletion = false;
            _wasPlatformRunning = platformRunning;
            _residualSweepActive = false;

            snapshot = new InjectRuntimeSnapshot { PlatformRunning = platformRunning };
            _latest = snapshot;
            _lastHadResiduals = false;
            _bridgePids.Clear();
            _stickyPids.Clear();
            _residualEmptyStreak = 0;
        }

        // Cold-start: только _latest, без SnapshotChanged.
    }

    public void Subscribe(object subscriber, DispatcherQueue queue)
    {
        ArgumentNullException.ThrowIfNull(subscriber);
        ArgumentNullException.ThrowIfNull(queue);

        bool start;
        lock (_gate)
        {
            _queue ??= queue;
            start = _subscribers.Add(subscriber) && _subscribers.Count == 1;
        }

        if (start)
            StartPolling(queue);
        else
            RequestRefresh();
    }

    public void Unsubscribe(object subscriber)
    {
        bool stop;
        lock (_gate)
        {
            if (!_subscribers.Remove(subscriber))
                return;
            stop = _subscribers.Count == 0;
            if (stop)
                ++_generation;
        }

        if (stop)
            StopPolling();
    }

    public void RequestRefresh()
    {
        DispatcherQueue? queue;
        lock (_gate)
            queue = _queue;
        if (queue is null)
            return;
        queue.TryEnqueue(() => ScheduleBackgroundScan());
    }

    public bool RequestProcessUnload(uint pid)
    {
        if (pid == 0)
            return false;

        // Хост rundll32 (GlassCompat Main) — не Remote FreeLibrary: вешает GS и платформу.
        if (IsGlassPlatformHostPid(pid))
        {
            return false;
        }

        InjectRuntimeEntry? entry = null;
        lock (_gate)
            entry = _latest.Entries.FirstOrDefault(e => e.Pid == pid);

        // Residual / платформа off — только Force (нет слушателя UnloadRequest).
        // Active — soft; при отсутствии события native сам перейдёт к Force.
        // Любой хост (x86|x64): soft UnloadRequest слушает InjectMap той же разрядности.
        var anyHost = ReloadRenderService.IsAnyGlassPlatformRunning();
        bool force;
        if (entry is null)
            force = !anyHost;
        else
            force = entry.Beacon == InjectRuntimeBeacon.Residual || !anyHost;

        var ok = force
            ? NativeHostInterop.ForceUnloadProcess(pid)
            : NativeHostInterop.RequestProcessUnload(pid);

        if (ok)
            RequestRefresh();
        return ok;
    }

    private void StartPolling(DispatcherQueue queue)
    {
        queue.TryEnqueue(() =>
        {
            lock (_gate)
            {
                _queue = queue;
                _forceFullCapturePending = true;
                if (_timer is null)
                {
                    _timer = queue.CreateTimer();
                    _timer.Interval = TimeSpan.FromSeconds(1);
                    _timer.IsRepeating = true;
                    _timer.Tick += (_, _) => ScheduleBackgroundScan();
                }
                _timer.Start();
            }
            ScheduleBackgroundScan();
        });
    }

    private void StopPolling()
    {
        DispatcherQueueTimer? timer;
        lock (_gate)
        {
            timer = _timer;
            _timer = null;
            // Следующая подписка снова сделает cold-probe, если платформа off.
            _idlePlatformProbePending = true;
            _pendingColdProbeCompletion = false;
            _forceFullCapturePending = false;
        }
        timer?.Stop();
    }

    private void ScheduleBackgroundScan()
    {
        int generation;
        lock (_gate)
        {
            if (_inFlight)
                return;
            if (_subscribers.Count == 0)
                return;
            _inFlight = true;
            generation = _generation;
        }

        _ = Task.Run(() =>
        {
            try
            {
                var snapshot = CaptureSnapshot();
                var queue = _queue;
                if (queue is null)
                {
                    lock (_gate)
                        _inFlight = false;
                    return;
                }

                // inFlight держим до Apply на UI — иначе тик запускает второй скан.
                queue.TryEnqueue(() =>
                {
                    try
                    {
                        ApplySnapshot(generation, snapshot);
                        App.ShellWindow?.RunDeferredShellReadyWorkAfterFirstProbe();
                    }
                    finally
                    {
                        lock (_gate)
                            _inFlight = false;
                    }
                });
            }
            catch
            {
                lock (_gate)
                    _inFlight = false;
            }
        });
    }

    private InjectRuntimeSnapshot CaptureSnapshot()
    {
        // GS x64 + только хост x86: NativeArch-mutex = false → карта .x86 игнорировалась (нет Active).
        var platformRunning = ReloadRenderService.IsAnyGlassPlatformRunning();
        var selfPid = (uint)Environment.ProcessId;

        // Читаем карту ДО lock, чтобы sweep trigger мог проверить slots.Length.
        var mapOk = NativeHostInterop.TryReadInjectMapSlots(out var slots);
        if (!mapOk)
            slots = Array.Empty<NativeHostInterop.InjectMapSlot>();
        slots = PruneDeadInjectMapSlots(slots);

        bool residualSweep;
        bool runColdProbe;
        bool forceFull;
        uint[] bridgePids;
        uint[] stickyPids;
        lock (_gate)
        {
            if (_wasPlatformRunning && !platformRunning)
            {
                _residualSweepActive = true;
                _residualEmptyStreak = 0;
                _peakSlots = 0;
            }
            // Shutdown race: slots declined significantly from peak while mutex still alive.
            // Trigger sweep immediately — scan-only processes must be Residual, not Active.
            else if (platformRunning && _peakSlots > 0 && slots.Length < _peakSlots - 1
                     && !_residualSweepActive)
            {
                _residualSweepActive = true;
                _residualEmptyStreak = 0;
            }
            if (platformRunning && !_residualSweepActive)
            {
                _idlePlatformProbePending = false;
                _pendingColdProbeCompletion = false;
                _residualEmptyStreak = 0;
                // Track peak slot count for shutdown race detection.
                if (slots.Length > _peakSlots)
                {
                    _peakSlots = slots.Length;
                }
            }
            // Recovery: sweep was from shutdown race, slots fully recovered to peak — deactivate.
            // Do NOT deactivate during drain (slots > 0 but < peak) — sweep must stay ON.
            if (platformRunning && _residualSweepActive && slots.Length >= _peakSlots && _peakSlots > 0)
            {
                _residualSweepActive = false;
                _idlePlatformProbePending = false;
                _pendingColdProbeCompletion = false;
                _residualEmptyStreak = 0;
                _peakSlots = slots.Length;
            }

            _wasPlatformRunning = platformRunning;
            residualSweep = _residualSweepActive;
            forceFull = _forceFullCapturePending;
            if (forceFull)
                _forceFullCapturePending = false;
            runColdProbe = !platformRunning && !residualSweep && (_idlePlatformProbePending || forceFull);
            if (runColdProbe)
                _pendingColdProbeCompletion = true;
            bridgePids = _bridgePids.Count == 0 ? [] : _bridgePids.ToArray();
            stickyPids = _stickyPids.Count == 0 ? [] : _stickyPids.ToArray();
        }

        // Платформа off: без residual-sweep и без cold-probe — только mutex (дешёвый idle).
        // Первый poll после StartPolling (_forceFullCapturePending) cheap-idle не использует.
        if (!platformRunning && !residualSweep && !runColdProbe)
            return new InjectRuntimeSnapshot { PlatformRunning = false };

        // Дыра: HostPublish CW off (H47) + map Close до !mutex → slots пусты при ещё «running»
        // или сразу после stop. Bridge PID + CW-host exe дополняют light-scan.
        if (slots.Length > 0)
        {
            lock (_gate)
            {
                foreach (var s in slots)
                {
                    if (s.Pid != 0 && s.Pid != selfPid)
                    {
                        _bridgePids.Add(s.Pid);
                        if (s.ModuleBits != 0)
                            _stickyPids.Add(s.Pid);
                    }
                }
            }
        }

        NativeHostInterop.InjectScanResult[] scan;
        if (platformRunning)
        {
            // slots ∪ sticky ∪ bridge — не терять PID, выпавший из карты до Module32.
            var pidSet = new HashSet<uint>();
            foreach (var s in slots)
            {
                if (s.Pid != 0 && s.Pid != selfPid)
                    pidSet.Add(s.Pid);
            }
            foreach (var pid in stickyPids)
            {
                if (pid != 0 && pid != selfPid)
                    pidSet.Add(pid);
            }
            foreach (var pid in bridgePids)
            {
                if (pid != 0 && pid != selfPid)
                    pidSet.Add(pid);
            }

            var pids = pidSet.Count == 0 ? Array.Empty<uint>() : pidSet.ToArray();
            // Пустой pids раньше вызывал Scan(null) = полный обход → residual rundll32-хост.
            var targetedScanOk = true;
            if (pids.Length == 0)
            {
                scan = Array.Empty<NativeHostInterop.InjectScanResult>();
            }
            else if (!NativeHostInterop.ScanGlassModules(pids, out scan))
            {
                scan = Array.Empty<NativeHostInterop.InjectScanResult>();
                targetedScanOk = false;
            }

            // CW hosts (ShellHost/SEH/…) часто вне карты — добрать Module32 по exe.
            scan = MergeScanResults(scan, ScanCwHostProcesses(selfPid));
            // Не чистить sticky при сбое Scan — иначе ложный drop и «воскрешение» на sweep.
            if (targetedScanOk)
                _ = PruneStickyAfterScan(scan, selfPid);
        }
        else if (!platformRunning && (residualSweep || runColdProbe))
        {
            var fullScanOk = NativeHostInterop.ScanGlassModules(null, out scan);
            if (!fullScanOk)
                scan = Array.Empty<NativeHostInterop.InjectScanResult>();
            // Полный scan + bridge/sticky seed (если full miss на AC).
            var seedSet = new HashSet<uint>();
            foreach (var pid in bridgePids)
                seedSet.Add(pid);
            foreach (var pid in stickyPids)
                seedSet.Add(pid);
            if (seedSet.Count > 0
                && !scan.Any(s => seedSet.Contains(s.Pid)))
            {
                var seed = seedSet.ToArray();
                if (NativeHostInterop.ScanGlassModules(seed, out var bridgeScan))
                    scan = MergeScanResults(scan, bridgeScan);
            }
            scan = MergeScanResults(scan, ScanCwHostProcesses(selfPid));
            if (fullScanOk)
                _ = PruneStickyAfterScan(scan, selfPid);
        }
        else
        {
            scan = Array.Empty<NativeHostInterop.InjectScanResult>();
        }

        // Native sticky SoT — меню/HasShellInjectResiduals читают ту же map.
        _ = NativeHostInterop.RefreshInjectResidualSticky(GlassPlatformArch.X64);
        _ = NativeHostInterop.RefreshInjectResidualSticky(GlassPlatformArch.X86);

        return Merge(platformRunning, residualSweep, slots, scan, selfPid);
    }

    /// <summary>
    /// Убрать sticky PID, у которых Module32 не нашёл Glass-модулей (или процесс мёртв).
    /// </summary>
    private int PruneStickyAfterScan(
        NativeHostInterop.InjectScanResult[] scan,
        uint selfPid)
    {
        var withBits = new HashSet<uint>();
        foreach (var s in scan)
        {
            if (s.Pid != 0 && s.ModuleBits != 0)
                withBits.Add(s.Pid);
        }

        var dropped = 0;
        lock (_gate)
        {
            if (_stickyPids.Count == 0)
                return 0;
            var remove = new List<uint>();
            foreach (var pid in _stickyPids)
            {
                if (pid == 0 || pid == selfPid || !withBits.Contains(pid))
                    remove.Add(pid);
            }
            foreach (var pid in remove)
            {
                if (_stickyPids.Remove(pid))
                    ++dropped;
            }
        }
        return dropped;
    }

    private static bool IsGlassSettingsProcess(uint pid, string? exe, uint selfPid) =>
        pid == selfPid
        || (!string.IsNullOrEmpty(exe)
            && exe.Equals("GlassSettings.exe", StringComparison.OrdinalIgnoreCase));

    private static bool IsGlassPlatformHostPid(uint pid)
    {
        if (pid == 0)
            return false;
        return pid == ReadHostPidMapping(@"Local\GlassHostPid.x64")
            || pid == ReadHostPidMapping(@"Local\GlassHostPid.x86");
    }

    /// <summary>Точечный Module32 по CW-host exe (ShellHost часто вне InjectMap — H47).</summary>
    private static NativeHostInterop.InjectScanResult[] ScanCwHostProcesses(uint selfPid)
    {
        var names = new[]
        {
            "ShellHost",
            "ShellExperienceHost",
            "StartMenuExperienceHost",
            "SearchHost",
            "SearchApp",
            "WidgetService",
            "Widgets",
        };
        var pids = new List<uint>(8);
        foreach (var name in names)
        {
            try
            {
                foreach (var proc in System.Diagnostics.Process.GetProcessesByName(name))
                {
                    try
                    {
                        var pid = unchecked((uint)proc.Id);
                        if (pid != 0 && pid != selfPid)
                            pids.Add(pid);
                    }
                    finally
                    {
                        proc.Dispose();
                    }
                }
            }
            catch
            {
                // access / race
            }
        }

        if (pids.Count == 0)
            return Array.Empty<NativeHostInterop.InjectScanResult>();

        var distinct = pids.Distinct().ToArray();
        return NativeHostInterop.ScanGlassModules(distinct, out var hit)
            ? hit
            : Array.Empty<NativeHostInterop.InjectScanResult>();
    }

    private static NativeHostInterop.InjectScanResult[] MergeScanResults(
        NativeHostInterop.InjectScanResult[] a,
        NativeHostInterop.InjectScanResult[] b)
    {
        if (b.Length == 0)
            return a;
        if (a.Length == 0)
            return b;
        var map = new Dictionary<uint, NativeHostInterop.InjectScanResult>(a.Length + b.Length);
        foreach (var s in a)
        {
            if (s.Pid != 0)
                map[s.Pid] = s;
        }
        foreach (var s in b)
        {
            if (s.Pid == 0 || s.ModuleBits == 0)
                continue;
            if (map.TryGetValue(s.Pid, out var prev))
            {
                prev.ModuleBits |= s.ModuleBits;
                if (string.IsNullOrEmpty(prev.ExeName) && !string.IsNullOrEmpty(s.ExeName))
                    prev.ExeName = s.ExeName;
                map[s.Pid] = prev;
            }
            else
            {
                map[s.Pid] = s;
            }
        }
        return map.Values.ToArray();
    }

    private static uint ReadHostPidMapping(string name)
    {
        var hMap = OpenFileMapping(0x0004 /* FILE_MAP_READ */, false, name);
        if (hMap == IntPtr.Zero)
            return 0;
        try
        {
            var view = MapViewOfFile(hMap, 0x0004, 0, 0, (UIntPtr)4);
            if (view == IntPtr.Zero)
                return 0;
            try
            {
                return unchecked((uint)Marshal.ReadInt32(view));
            }
            finally
            {
                UnmapViewOfFile(view);
            }
        }
        finally
        {
            CloseHandle(hMap);
        }
    }

    private static NativeHostInterop.InjectArch GuessProcessArch(uint pid)
    {
        // GS x64: IsWow64Process(true) → цель WOW64 = x86.
        var h = OpenProcess(0x1000, false, pid);
        if (h == IntPtr.Zero)
            return NativeHostInterop.InjectArch.X64;
        try
        {
            if (!IsWow64Process(h, out var wow) || !wow)
                return NativeHostInterop.InjectArch.X64;
            return NativeHostInterop.InjectArch.X86;
        }
        finally
        {
            CloseHandle(h);
        }
    }

    /// <summary>
    /// The map can outlive its process. A nonexistent PID is different from
    /// access denied; a reused PID with a different executable is not the old
    /// slot either. Unknown states are retained rather than guessed dead.
    /// </summary>
    private static bool IsCurrentInjectMapSlot(NativeHostInterop.InjectMapSlot slot)
    {
        const int errorInvalidParameter = 87;
        const uint waitObject0 = 0;
        const uint waitTimeout = 258;
        const int mapExeNameLimit = 63; // kExeNameChars - terminating NUL
        if (slot.Pid == 0)
            return false;
        var h = OpenProcess(0x1000 | 0x100000 /* QUERY_LIMITED_INFORMATION | SYNCHRONIZE */, false, slot.Pid);
        if (h == IntPtr.Zero)
        {
            // No handle to the old process means the PID can be reused at any
            // moment. Hide a missing PID, but never clear its shared slot here.
            return Marshal.GetLastWin32Error() != errorInvalidParameter;
        }
        try
        {
            var wait = WaitForSingleObject(h, 0);
            if (wait == waitObject0)
            {
                // Keep the handle open while clearing: the old process object
                // and its PID cannot be reused before this handle is closed.
                _ = NativeHostInterop.ClearInjectMapPidForArch(slot.Arch, slot.Pid);
                return false;
            }
            if (wait != waitTimeout)
                return true; // unknown
            if (string.IsNullOrEmpty(slot.ExeName))
                return true;
            var path = new StringBuilder(1024);
            var length = (uint)path.Capacity;
            if (!QueryFullProcessImageName(h, 0, path, ref length))
                return true;
            var actual = Path.GetFileName(path.ToString());
            if (string.IsNullOrEmpty(actual))
                return true;
            return string.Equals(actual, slot.ExeName, StringComparison.OrdinalIgnoreCase)
                || (slot.ExeName.Length == mapExeNameLimit
                    && actual.StartsWith(slot.ExeName, StringComparison.OrdinalIgnoreCase));
        }
        finally
        {
            CloseHandle(h);
        }
    }

    private static NativeHostInterop.InjectMapSlot[] PruneDeadInjectMapSlots(
        NativeHostInterop.InjectMapSlot[] slots)
    {
        if (slots.Length == 0)
            return slots;

        List<NativeHostInterop.InjectMapSlot>? alive = null;
        for (var i = 0; i < slots.Length; i++)
        {
            var s = slots[i];
            if (IsCurrentInjectMapSlot(s))
            {
                if (alive is not null)
                    alive.Add(s);
                continue;
            }
            alive ??= slots.Take(i).ToList();
        }

        return alive?.ToArray() ?? slots;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenProcess(uint dwDesiredAccess, bool bInheritHandle, uint dwProcessId);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint WaitForSingleObject(IntPtr hHandle, uint milliseconds);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool QueryFullProcessImageName(
        IntPtr hProcess, uint flags, StringBuilder image, ref uint size);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseHandle(IntPtr hObject);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr OpenFileMapping(uint dwDesiredAccess, bool bInheritHandle, string lpName);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr MapViewOfFile(
        IntPtr hFileMappingObject,
        uint dwDesiredAccess,
        uint dwFileOffsetHigh,
        uint dwFileOffsetLow,
        UIntPtr dwNumberOfBytesToMap);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool UnmapViewOfFile(IntPtr lpBaseAddress);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsWow64Process(IntPtr hProcess, out bool wow64Process);

    private static InjectRuntimeSnapshot Merge(
        bool platformRunning,
        bool residualSweep,
        NativeHostInterop.InjectMapSlot[] slots,
        NativeHostInterop.InjectScanResult[] scan,
        uint selfPid)
    {
        var scanByPid = scan.ToDictionary(s => s.Pid, s => s);
        var list = new List<InjectRuntimeEntry>();

        if (platformRunning)
        {
            foreach (var slot in slots)
            {
                if (slot.Pid == 0)
                    continue;
                if (IsGlassSettingsProcess(slot.Pid, slot.ExeName, selfPid))
                {
                    continue;
                }

                scanByPid.TryGetValue(slot.Pid, out var scanned);
                // H42: AppContainer Toolhelp часто неполный (только Compat) — не даём scan
                // сузить Alive-слот (иначе UICompat «пропадает» из таблицы при живой платформе).
                uint bits = slot.ModuleBits;
                if (scanned.Pid != 0 && scanned.ModuleBits != 0)
                {
                    var scanSubsetOfSlot = (scanned.ModuleBits & slot.ModuleBits) == scanned.ModuleBits
                        && scanned.ModuleBits != slot.ModuleBits;
                    if (slot.State == NativeHostInterop.InjectState.Alive && scanSubsetOfSlot)
                        bits = slot.ModuleBits;
                    else
                        bits = scanned.ModuleBits | slot.ModuleBits;
                }
                var exe = !string.IsNullOrWhiteSpace(slot.ExeName)
                    ? slot.ExeName
                    : (scanned.ExeName ?? string.Empty);

                InjectRuntimeBeacon beacon;
                if (slot.Kind is NativeHostInterop.InjectKind.RejectedBlockList
                    or NativeHostInterop.InjectKind.RejectedForbidden)
                {
                    beacon = InjectRuntimeBeacon.Rejected;
                }
                else if (slot.State is NativeHostInterop.InjectState.Unloading
                    or NativeHostInterop.InjectState.Unloaded)
                {
                    // Модули выгружаются/выгружены → красный (невыгруженный остаток).
                    beacon = InjectRuntimeBeacon.Residual;
                }
                else
                {
                    beacon = InjectRuntimeBeacon.Active;
                }

                list.Add(new InjectRuntimeEntry
                {
                    Pid = slot.Pid,
                    ExeName = exe,
                    Beacon = beacon,
                    ModuleBits = bits,
                    Path = slot.Path,
                    Arch = slot.Arch,
                    DiagFlags = slot.DiagFlags,
                    DiagLifecycle = slot.DiagLifecycle,
                    SubclassWindowTotal = slot.SubclassWindowTotal,
                    SubclassKindMask = slot.SubclassKindMask,
                    State = slot.State,
                    ModulesText = FormatModules(bits),
                });
            }

            var mapped = slots.Select(s => s.Pid).ToHashSet();
            foreach (var s in scan)
            {
                if (mapped.Contains(s.Pid) || s.ModuleBits == 0)
                    continue;
                if (IsGlassSettingsProcess(s.Pid, s.ExeName, selfPid))
                    continue;
                // Хост платформы (rundll32+GlassCompat) — не residual injectee.
                if (IsGlassPlatformHostPid(s.Pid))
                {
                    continue;
                }
                // Вне карты: при живой платформе и пустых слотах — Residual (платформа выгружается).
                // При живой платформе с активными слотами — Active (слот может запаздывать при старте).
                // CFG-fix: residualSweep — sweep уже активен (мьютекс жив, но слоты пустеют),
                // scan-only процессы должны быть Residual (иначе зелёный flicker после stop).
                bool slotsEmpty = slots.Length == 0;
                var scanBeacon = (platformRunning && !slotsEmpty && !residualSweep)
                    ? InjectRuntimeBeacon.Active
                    : InjectRuntimeBeacon.Residual;

                list.Add(new InjectRuntimeEntry
                {
                    Pid = s.Pid,
                    ExeName = s.ExeName ?? string.Empty,
                    Beacon = scanBeacon,
                    ModuleBits = s.ModuleBits,
                    Arch = GuessProcessArch(s.Pid),
                    ModulesText = FormatModules(s.ModuleBits),
                });
            }
        }
        else
        {
            foreach (var s in scan)
            {
                if (s.ModuleBits == 0)
                    continue;
                if (IsGlassSettingsProcess(s.Pid, s.ExeName, selfPid))
                    continue;
                if (IsGlassPlatformHostPid(s.Pid))
                    continue;

                list.Add(new InjectRuntimeEntry
                {
                    Pid = s.Pid,
                    ExeName = s.ExeName ?? string.Empty,
                    Beacon = InjectRuntimeBeacon.Residual,
                    ModuleBits = s.ModuleBits,
                    Arch = GuessProcessArch(s.Pid),
                    ModulesText = FormatModules(s.ModuleBits),
                });
            }
        }

        list.Sort((a, b) =>
        {
            var c = a.Beacon.CompareTo(b.Beacon);
            if (c != 0)
                return c;
            c = string.Compare(a.ExeName, b.ExeName, StringComparison.OrdinalIgnoreCase);
            return c != 0 ? c : a.Pid.CompareTo(b.Pid);
        });

        var activeCount = list.Count(e => e.Beacon == InjectRuntimeBeacon.Active);
        var rejectedCount = list.Count(e => e.Beacon == InjectRuntimeBeacon.Rejected);
        var residualCount = list.Count(e => e.Beacon == InjectRuntimeBeacon.Residual);

        return new InjectRuntimeSnapshot
        {
            Entries = list,
            ActiveCount = activeCount,
            RejectedCount = rejectedCount,
            ResidualCount = residualCount,
            PlatformRunning = platformRunning,
        };
    }

    private static string FormatModules(uint bits)
    {
        var flags = (NativeHostInterop.InjectModuleBits)bits;
        var parts = new List<string>(5);
        if (flags.HasFlag(NativeHostInterop.InjectModuleBits.Compat))
            parts.Add("GlassCompat");
        if (flags.HasFlag(NativeHostInterop.InjectModuleBits.Core))
            parts.Add("GlassCore");
        if (flags.HasFlag(NativeHostInterop.InjectModuleBits.Wnd))
            parts.Add("WndCompat");
        if (flags.HasFlag(NativeHostInterop.InjectModuleBits.Explorer))
            parts.Add("ExplorerCompat");
        if (flags.HasFlag(NativeHostInterop.InjectModuleBits.Taskbar))
            parts.Add("TaskbarCompat");
        if (flags.HasFlag(NativeHostInterop.InjectModuleBits.UICompat))
            parts.Add("UICompat");
        return parts.Count == 0 ? "—" : string.Join(", ", parts);
    }

    private void ApplySnapshot(int generation, InjectRuntimeSnapshot snapshot)
    {
        lock (_gate)
        {
            if (generation != _generation)
                return;
            _latest = snapshot;
            _lastHadResiduals = snapshot.HasResiduals;

            if (_pendingColdProbeCompletion)
            {
                _pendingColdProbeCompletion = false;
                _idlePlatformProbePending = false;
                if (snapshot.HasResiduals)
                    _residualSweepActive = true;
            }

            if (!snapshot.PlatformRunning && !snapshot.HasResiduals)
            {
                ++_residualEmptyStreak;
                // SoT: снимаем sweep когда native Query Idle (и локальный список пуст).
                var lifeIdle =
                    NativeHostInterop.QueryInjectLifecycleUi(GlassPlatformArch.X64)
                        == NativeHostInterop.InjectLifecycleUi.Idle
                    && NativeHostInterop.QueryInjectLifecycleUi(GlassPlatformArch.X86)
                        == NativeHostInterop.InjectLifecycleUi.Idle;
                if (_residualEmptyStreak >= 8 && lifeIdle)
                {
                    _residualSweepActive = false;
                    _bridgePids.Clear();
                    _stickyPids.Clear();
                }
            }
            else if (snapshot.HasResiduals)
            {
                _residualEmptyStreak = 0;
                foreach (var e in snapshot.Entries)
                {
                    if (e.Pid == 0)
                        continue;
                    _bridgePids.Add(e.Pid);
                    if (e.ModuleBits != 0)
                        _stickyPids.Add(e.Pid);
                }
            }
            else
            {
                // Живая платформа / Active без residual: sticky из слотов с модулями.
                foreach (var e in snapshot.Entries)
                {
                    if (e.Pid != 0 && e.ModuleBits != 0)
                        _stickyPids.Add(e.Pid);
                }
            }
        }

        SnapshotChanged?.Invoke(snapshot);

        // Пока есть подписчики — таймер живёт (cheap-idle увидит старт платформы).
        if (_subscribers.Count > 0)
            _timer?.Start();
    }
}
