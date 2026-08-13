using GlassSettings.Core.Registry;
using GlassSettings.Core.Text;
using GlassSettings.Services;

namespace GlassSettings.Core.Platform;

/// <summary>
/// Отложенный live-reload: не блокирует UI при частых изменениях (ColorPicker, слайдеры, комбобоксы).
/// </summary>
internal static class LiveReloadScheduler
{
    private static readonly object Gate = new();
    private static CancellationTokenSource? _cts;
    private static uint _pendingFlags;
    private static bool _pendingDwmNotify;

    private static readonly object RootGate = new();
    private static CancellationTokenSource? _rootCts;
    private static string? _pendingRootCompositionRegSub;
    private static bool _pendingRootCompositionOnly;
    private static (string RegSub, string ValueName, uint Value)? _pendingRootRegistryWrite;
    private static (uint Alpha, string RegSub, string ValueName)? _pendingThemeAlphaWrite;

    private const int DebounceMs = 100;
    private const int RootCompositionDebounceMs = 100;
    private const int RootRegistryDebounceMs = 100;
    private const int ThemeAlphaDebounceMs = 150;

    public static void ScheduleFlyoutReload() =>
        Schedule(ReloadRenderFlags.GlassSettingsCommit, dwmNotify: true);

    public static void ScheduleDwmColorizationReload() =>
        Schedule(ReloadRenderFlags.GlassSettingsCommit, dwmNotify: true);

    /// <summary>Немедленная рассылка WM_DWMCOLORIZATIONCOLORCHANGED + SignalReloadRender при запущенной OG.</summary>
    public static void NotifyDwmColorizationNow()
    {
        _ = Task.Run(() =>
        {
            ReloadRenderService.NotifyDwmColorizationChanged();
            ReloadRenderService.SignalReloadRender(ReloadRenderFlags.GlassSettingsCommit);
        });
    }

    /// <summary>Немедленная рассылка WM_THEMECHANGED (Reflection / Material / Atlas).</summary>
    public static void NotifyThemeChangedNow()
    {
        _ = Task.Run(() => ReloadRenderService.NotifyThemeChanged());
    }

    public static void ScheduleFlags(uint flags, bool dwmNotify = false) =>
        Schedule(flags, dwmNotify);

    /// <summary>Debounced PostLiveRootColorization (gradient/border/EffectType).</summary>
    public static void ScheduleRootCompositionReload(
        string? regSub,
        bool compositionOnly = false,
        int debounceMs = RootCompositionDebounceMs)
    {
        lock (RootGate)
        {
            _pendingRootCompositionRegSub = regSub;
            _pendingRootCompositionOnly = compositionOnly;
            RestartRootDebounced(debounceMs);
        }
    }

    /// <summary>Debounced REG_DWORD + composition reload (border/gradient preview).</summary>
    public static void ScheduleRegistryWrite(
        string regSub,
        string valueName,
        uint value,
        bool compositionOnly = true,
        int debounceMs = RootRegistryDebounceMs)
    {
        lock (RootGate)
        {
            _pendingRootRegistryWrite = (regSub, valueName, value);
            _pendingRootCompositionRegSub = regSub;
            _pendingRootCompositionOnly = compositionOnly;
            RestartRootDebounced(debounceMs);
        }
    }

    /// <summary>Отменить отложенный root live-reload (перед commit пикера).</summary>
    public static void CancelRootPending()
    {
        lock (RootGate)
        {
            _rootCts?.Cancel();
            _rootCts?.Dispose();
            _rootCts = null;
            _pendingRootCompositionRegSub = null;
            _pendingRootCompositionOnly = false;
            _pendingRootRegistryWrite = null;
            _pendingThemeAlphaWrite = null;
        }
    }

    /// <summary>Debounced alpha + InvalidateThemedChromeOnTrackedRoots.</summary>
    public static void ScheduleDrawThemeAlphaLive(
        uint alpha,
        string regSub,
        string valueName,
        int debounceMs = ThemeAlphaDebounceMs)
    {
        lock (RootGate)
        {
            _pendingThemeAlphaWrite = (alpha, regSub, valueName);
            RestartRootDebounced(debounceMs);
        }
    }

    public static void CancelPending()
    {
        lock (Gate)
        {
            _cts?.Cancel();
            _cts?.Dispose();
            _cts = null;
            _pendingFlags = 0;
            _pendingDwmNotify = false;
        }

        CancelDwmWritePending();

        lock (RootGate)
        {
            _rootCts?.Cancel();
            _rootCts?.Dispose();
            _rootCts = null;
            _pendingRootCompositionRegSub = null;
            _pendingRootCompositionOnly = false;
            _pendingRootRegistryWrite = null;
            _pendingThemeAlphaWrite = null;
        }

        lock (FlyoutGate)
        {
            _flyoutCts?.Cancel();
            _flyoutCts?.Dispose();
            _flyoutCts = null;
            _pendingFlyoutWrite = null;
        }

        lock (TextGlowGate)
        {
            CancelGlobalTextGlowPendingLocked();
            StopTextGlowPollLocked(flush: false);
            _textGlowPreviewDepth = 0;
            _textGlowSampleModel = null;
            _textGlowSampleQueue = null;
            _lastAppliedTextGlowPoll = null;
        }
    }

    private static void RestartRootDebounced(int debounceMs)
    {
        _rootCts?.Cancel();
        _rootCts?.Dispose();
        _rootCts = new CancellationTokenSource();
        var token = _rootCts.Token;
        _ = RunRootDebouncedAsync(debounceMs, token);
    }

    private static async Task RunRootDebouncedAsync(int debounceMs, CancellationToken token)
    {
        try
        {
            await Task.Delay(debounceMs, token).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            return;
        }

        string? compositionRegSub;
        bool compositionOnly;
        (string RegSub, string ValueName, uint Value)? registryWrite;
        (uint Alpha, string RegSub, string ValueName)? themeAlphaWrite;

        lock (RootGate)
        {
            compositionRegSub = _pendingRootCompositionRegSub;
            compositionOnly = _pendingRootCompositionOnly;
            registryWrite = _pendingRootRegistryWrite;
            themeAlphaWrite = _pendingThemeAlphaWrite;
            _pendingRootCompositionRegSub = null;
            _pendingRootCompositionOnly = false;
            _pendingRootRegistryWrite = null;
            _pendingThemeAlphaWrite = null;
        }

        if (themeAlphaWrite is not null || registryWrite is not null || compositionRegSub is not null)
        {
            await Task.Run(() =>
            {
                if (registryWrite is { } rw)
                    GlassRegistry.WriteDword(rw.RegSub, rw.ValueName, rw.Value);

                if (themeAlphaWrite is { } ta)
                {
                    GlassRegistry.WriteDword(ta.RegSub, ta.ValueName, ta.Alpha);
                    NativeHostInterop.InvalidateThemedChromeOnTrackedRootsIfEngineLoaded();
                    return;
                }

                RootWindowsLiveReload.PostLiveRootComposition(
                    compositionRegSub ?? registryWrite?.RegSub,
                    compositionOnly);
            }, token).ConfigureAwait(false);
        }
    }

    private static readonly object FlyoutGate = new();
    private static CancellationTokenSource? _flyoutCts;
    private static (string RegSub, string ValueName, uint Value, uint Flags)? _pendingFlyoutWrite;

    private static readonly object TextGlowGate = new();
    private static CancellationTokenSource? _textGlowCts;
    private static GlobalTextGlowModel? _pendingTextGlowWrite;
    private static CancellationTokenSource? _textGlowPollCts;
    private static Func<GlobalTextGlowModel>? _textGlowSampleModel;
    private static Microsoft.UI.Dispatching.DispatcherQueue? _textGlowSampleQueue;
    private static GlobalTextGlowModel? _lastAppliedTextGlowPoll;
    private static int _textGlowPreviewDepth;
    private const int TextGlowDebounceMs = 100;
    private const int TextGlowPollMs = 100;

    private const int FlyoutDebounceMs = 100;

    /// <summary>Debounced запись REG_DWORD + SignalReloadRender (Taskbar color preview).</summary>
    public static void ScheduleFlyoutRegistryWrite(
        string regSub,
        string valueName,
        uint value,
        uint reloadFlags,
        int debounceMs = FlyoutDebounceMs)
    {
        lock (FlyoutGate)
        {
            _pendingFlyoutWrite = (regSub, valueName, value, reloadFlags);
            _flyoutCts?.Cancel();
            _flyoutCts?.Dispose();
            _flyoutCts = new CancellationTokenSource();
            var token = _flyoutCts.Token;
            _ = RunFlyoutDebouncedAsync(debounceMs, token);
        }
    }

    /// <summary>Debounced REG_BINARY GlobalTextGlow + host/platform reload.</summary>
    public static void ScheduleGlobalTextGlowWrite(GlobalTextGlowModel model, int debounceMs = TextGlowDebounceMs)
    {
        lock (TextGlowGate)
        {
            _pendingTextGlowWrite = model;
            _textGlowCts?.Cancel();
            _textGlowCts?.Dispose();
            _textGlowCts = new CancellationTokenSource();
            var token = _textGlowCts.Token;
            _ = RunTextGlowDebouncedAsync(debounceMs, token);
        }
    }

    public static void CancelGlobalTextGlowPending()
    {
        lock (TextGlowGate)
        {
            CancelGlobalTextGlowPendingLocked();
            if (_textGlowPreviewDepth > 0)
            {
                StopTextGlowPollLocked(flush: false);
                _textGlowPreviewDepth = 0;
                _textGlowSampleModel = null;
                _textGlowSampleQueue = null;
                _lastAppliedTextGlowPoll = null;
            }
        }
    }

    /// <summary>Poll preview GlobalTextGlow (~100 ms).</summary>
    public static void BeginTextGlowLivePreview(
        Microsoft.UI.Dispatching.DispatcherQueue sampleQueue,
        Func<GlobalTextGlowModel> sampleModel)
    {
        lock (TextGlowGate)
        {
            CancelGlobalTextGlowPendingLocked();
            _textGlowSampleQueue = sampleQueue;
            _textGlowSampleModel = sampleModel;
            if (++_textGlowPreviewDepth > 1)
                return;

            _lastAppliedTextGlowPoll = null;
            _textGlowPollCts?.Cancel();
            _textGlowPollCts?.Dispose();
            _textGlowPollCts = new CancellationTokenSource();
            var token = _textGlowPollCts.Token;
            _ = RunTextGlowPollAsync(token);
        }
    }

    /// <summary>Остановить poll и финальный flush sampleModel().</summary>
    public static void EndTextGlowLivePreview()
    {
        Func<GlobalTextGlowModel>? sampleModel;
        Microsoft.UI.Dispatching.DispatcherQueue? sampleQueue;
        lock (TextGlowGate)
        {
            if (_textGlowPreviewDepth <= 0)
                return;

            if (--_textGlowPreviewDepth > 0)
                return;

            sampleModel = _textGlowSampleModel;
            sampleQueue = _textGlowSampleQueue;
            StopTextGlowPollLocked(flush: false);
            _textGlowSampleModel = null;
            _textGlowSampleQueue = null;
            _lastAppliedTextGlowPoll = null;
        }

        if (sampleModel is null || sampleQueue is null)
            return;

        _ = Task.Run(() =>
        {
            if (!TrySampleTextGlowOnUi(sampleQueue, sampleModel, out var model))
                return;

            TextGlowLiveReload.WriteAndReload(model);
        });
    }

    private static void CancelGlobalTextGlowPendingLocked()
    {
        _textGlowCts?.Cancel();
        _textGlowCts?.Dispose();
        _textGlowCts = null;
        _pendingTextGlowWrite = null;
    }

    private static void StopTextGlowPollLocked(bool flush)
    {
        _textGlowPollCts?.Cancel();
        _textGlowPollCts?.Dispose();
        _textGlowPollCts = null;
        _ = flush;
    }

    private static async Task RunTextGlowPollAsync(CancellationToken token)
    {
        Func<GlobalTextGlowModel>? sampleModel;
        Microsoft.UI.Dispatching.DispatcherQueue? sampleQueue;
        lock (TextGlowGate)
        {
            sampleModel = _textGlowSampleModel;
            sampleQueue = _textGlowSampleQueue;
        }

        if (sampleModel is null || sampleQueue is null)
            return;

        try
        {
            while (!token.IsCancellationRequested)
            {
                await Task.Delay(TextGlowPollMs, token).ConfigureAwait(false);

                if (!TrySampleTextGlowOnUi(sampleQueue, sampleModel, out var model))
                    continue;

                GlobalTextGlowModel? last;
                lock (TextGlowGate)
                {
                    last = _lastAppliedTextGlowPoll;
                }

                if (last is { } prev && TextGlowModelsEqual(prev, model))
                    continue;

                lock (TextGlowGate)
                {
                    _lastAppliedTextGlowPoll = model;
                }

                await Task.Run(() => TextGlowLiveReload.WriteAndReload(model), token).ConfigureAwait(false);
            }
        }
        catch (OperationCanceledException)
        {
        }
    }

    private static bool TrySampleTextGlowOnUi(
        Microsoft.UI.Dispatching.DispatcherQueue sampleQueue,
        Func<GlobalTextGlowModel> sampleModel,
        out GlobalTextGlowModel model)
    {
        model = default;
        GlobalTextGlowModel captured = default;
        var done = false;
        sampleQueue.TryEnqueue(() =>
        {
            captured = sampleModel();
            done = true;
        });

        var deadline = Environment.TickCount64 + 500;
        while (!done && Environment.TickCount64 < deadline)
            Thread.Sleep(1);

        if (!done)
            return false;

        model = captured;
        return true;
    }

    private static bool TextGlowModelsEqual(GlobalTextGlowModel left, GlobalTextGlowModel right) =>
        left.GlowEnabled == right.GlowEnabled
        && left.GlowRadiusThousandths == right.GlowRadiusThousandths
        && left.GlowColor == right.GlowColor
        && left.GlowSpreadMode == right.GlowSpreadMode;

    private static async Task RunTextGlowDebouncedAsync(int debounceMs, CancellationToken token)
    {
        try
        {
            await Task.Delay(debounceMs, token).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            return;
        }

        GlobalTextGlowModel? write;
        lock (TextGlowGate)
        {
            write = _pendingTextGlowWrite;
            _pendingTextGlowWrite = null;
        }

        if (write is not { } model)
            return;

        await Task.Run(() => TextGlowLiveReload.WriteAndReload(model), token).ConfigureAwait(false);
    }

    private static async Task RunFlyoutDebouncedAsync(int debounceMs, CancellationToken token)
    {
        try
        {
            await Task.Delay(debounceMs, token).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            return;
        }

        (string RegSub, string ValueName, uint Value, uint Flags)? write;
        lock (FlyoutGate)
        {
            write = _pendingFlyoutWrite;
            _pendingFlyoutWrite = null;
        }

        if (write is not { } w)
            return;

        await Task.Run(() =>
        {
            GlassRegistry.WriteDword(w.RegSub, w.ValueName, w.Value);
            if (w.Flags != 0)
                ReloadRenderService.SignalReloadRender(w.Flags);
        }, token).ConfigureAwait(false);
    }

    private static void Schedule(uint flags, bool dwmNotify)
    {
        lock (Gate)
        {
            _pendingFlags |= flags;
            if (dwmNotify)
                _pendingDwmNotify = true;

            _cts?.Cancel();
            _cts?.Dispose();
            _cts = new CancellationTokenSource();
            var token = _cts.Token;
            _ = RunDebouncedAsync(token);
        }
    }

    private static async Task RunDebouncedAsync(CancellationToken token)
    {
        try
        {
            await Task.Delay(DebounceMs, token).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            return;
        }

        uint flags;
        bool dwmNotify;
        lock (Gate)
        {
            flags = _pendingFlags;
            _pendingFlags = 0;
            dwmNotify = _pendingDwmNotify;
            _pendingDwmNotify = false;
        }

        if (flags == 0 && !dwmNotify)
            return;

        await Task.Run(() =>
        {
            if (dwmNotify)
                ReloadRenderService.NotifyDwmColorizationChanged();

            if (flags != 0)
                ReloadRenderService.SignalReloadRender(flags);
        }, token).ConfigureAwait(false);
    }

    private static readonly object DwmWriteGate = new();
    private static CancellationTokenSource? _dwmWriteCts;
    private static Action? _pendingDwmWriteAction;

    /// <summary>Debounced запись HKCU\DWM + NotifyDwmColorizationNow (preview пикеров/цветов).</summary>
    public static void ScheduleDwmWrite(Action writeAction, int debounceMs = DebounceMs)
    {
        lock (DwmWriteGate)
        {
            _pendingDwmWriteAction = writeAction;
            _dwmWriteCts?.Cancel();
            _dwmWriteCts?.Dispose();
            _dwmWriteCts = new CancellationTokenSource();
            var token = _dwmWriteCts.Token;
            _ = RunDwmWriteDebouncedAsync(debounceMs, token);
        }
    }

    public static void CancelDwmWritePending()
    {
        lock (DwmWriteGate)
        {
            _dwmWriteCts?.Cancel();
            _dwmWriteCts?.Dispose();
            _dwmWriteCts = null;
            _pendingDwmWriteAction = null;
        }
    }

    public static void FlushDwmWritePending()
    {
        Action? action;
        lock (DwmWriteGate)
        {
            _dwmWriteCts?.Cancel();
            _dwmWriteCts?.Dispose();
            _dwmWriteCts = null;
            action = _pendingDwmWriteAction;
            _pendingDwmWriteAction = null;
        }

        if (action is not null)
            Task.Run(action);
    }

    private static async Task RunDwmWriteDebouncedAsync(int debounceMs, CancellationToken token)
    {
        try
        {
            await Task.Delay(debounceMs, token).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            return;
        }

        Action? action;
        lock (DwmWriteGate)
        {
            action = _pendingDwmWriteAction;
            _pendingDwmWriteAction = null;
        }

        if (action is not null)
            await Task.Run(action, token).ConfigureAwait(false);
    }
}
