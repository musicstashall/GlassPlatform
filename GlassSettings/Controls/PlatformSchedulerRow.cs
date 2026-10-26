using GlassSettings.Core.Platform;
using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Controls;

public enum PlatformSchedulerKind { Glass, OpenGlass }

/// <summary>Логика OpenGlass/Glass scheduler: отдельные UI-элементы (паттерн Popup-страниц).</summary>
public sealed class PlatformSchedulerRow
{
    private readonly PlatformSchedulerStackableActions _stackable;
    private readonly GlassSettingsLabeledToggleSwitch _installGate;

    private bool _suppress;
    private bool _schedDisabled;
    private bool _busy;
    private bool? _pendingGateTarget;
    private bool _presenceHooked;

    /// <summary>Идёт install/uninstall через gate (блокирует Refresh и EnsureTrailingGateEnabled).</summary>
    public bool IsInstallGateBusy => _busy;

    public PlatformSchedulerRow()
    {
        _stackable = new PlatformSchedulerStackableActions();
        _installGate = new GlassSettingsLabeledToggleSwitch();

        _stackable.StatusSplitClicked += (_, _) => OnStatusSplitClick();
        _stackable.MenuToggleClicked += (_, _) => OnMenuToggleClick();
        _stackable.MenuDeleteClicked += (_, _) => OnMenuDeleteClick();
        _installGate.Toggled += InstallGate_OnToggled;

        _stackable.Loaded += OnStackableLoaded;
        _stackable.Unloaded += OnStackableUnloaded;
    }

    public PlatformSchedulerKind Kind { get; set; }

    /// <summary>Для Glass: явная битность задачи; null — native arch. Dual UI задаёт X86/X64 явно.</summary>
    public GlassPlatformArch? GlassArch { get; set; }

    private GlassPlatformArch EffectiveGlassArch => GlassArch ?? GlassPlatformNames.NativeArch;

    /// <summary>
    /// GS x64 + x86: блок только без задачи и без InstallPath (нет путей к модулям x86).
    /// </summary>
    private bool IsX86ControlsLocked =>
        Kind == PlatformSchedulerKind.Glass
        && GlassArch == GlassPlatformArch.X86
        && GlassPlatformUiPolicy.IsX86DualUiControlsLocked();

    /// <summary>UserControl целиком — в HeaderActions / ActionsBelowHost.</summary>
    public PlatformSchedulerStackableActions StackableActions => _stackable;

    /// <summary>GlassSettingsLabeledToggleSwitch — TrailingContent (как gate на hub-спойлерах).</summary>
    public GlassSettingsLabeledToggleSwitch InlineTrailing => _installGate;

    public event EventHandler? StateChanged;

    public bool TaskExists => QueryUiState().TaskExists;

    /// <summary>Direct query bypassing snapshot — always reflects current state.</summary>
    private PlatformTaskState QueryDirectState() =>
        PlatformPresenceService.Instance.QueryTaskStateDirect(Kind, EffectiveGlassArch);

    /// <summary>
    /// Install-gate (задача есть/нет): прямой планировщик. Snapshot dual x64 часто на кадр отстаёт после install/uninstall.
    /// </summary>
    private bool QueryInstallGateTaskExists()
    {
        if (_busy && _pendingGateTarget is { } target)
            return target;

        return QueryDirectState().TaskExists;
    }

    /// <summary>
    /// UI читает снимок presence. Прямой планировщик после сброса кэша на старте Glass
    /// на кадр показывает OpenGlass как отсутствующий.
    /// </summary>
    private PlatformTaskState QueryUiState()
    {
        var snap = PlatformPresenceService.Instance.CurrentSnapshot;
        if (snap.Revision > 0)
            return PlatformPresenceService.Instance.QueryTaskState(Kind, EffectiveGlassArch);

        if (Kind == PlatformSchedulerKind.Glass && GlassPlatformService.IsAnyLifecycleBusy())
        {
            var life = NativeHostInterop.QueryInjectLifecycleUi(EffectiveGlassArch);
            return new PlatformTaskState(
                TaskExists: true,
                Lifecycle: life,
                Autorun: life == NativeHostInterop.InjectLifecycleUi.Starting
                    ? GlassAutorunUiState.Ready
                    : GlassAutorunUiState.NoTask);
        }

        return QueryDirectState();
    }

    /// <summary>Подключение к navigate-строке после AddNavigateSection (строка уже в дереве).</summary>
    public void AttachToNavigateRow(SettingsNavigateRow navRow)
    {
        navRow.PlatformSchedulerOwner = this;
        navRow.HeaderActions = _stackable;
        navRow.TrailingContent = _installGate;
    }

    /// <summary>Split + gate в одной строке dual-шапки (без TrailingContent navigate-строки).</summary>
    public void AttachToDualControlRow(ContentPresenter actionsHost, ContentPresenter gateHost)
    {
        actionsHost.Content = _stackable;
        gateHost.Content = _installGate;
    }

    private void OnStackableLoaded(object sender, RoutedEventArgs e)
    {
        if (_presenceHooked)
            return;

        _presenceHooked = true;
        PlatformPresenceService.Instance.PresenceChanged += OnPresenceChanged;
        PlatformPresenceService.Instance.AddSubscriber();
        Refresh();
    }

    private void OnStackableUnloaded(object sender, RoutedEventArgs e)
    {
        if (!_presenceHooked)
            return;

        _presenceHooked = false;
        PlatformPresenceService.Instance.PresenceChanged -= OnPresenceChanged;
        PlatformPresenceService.Instance.RemoveSubscriber();
    }

    private void OnPresenceChanged(object? sender, EventArgs e)
    {
        if (_busy)
            return;

        _stackable.DispatcherQueue.TryEnqueue(Refresh);
    }

    public void Refresh()
    {
        if (_suppress)
            return;

        if (_busy && _pendingGateTarget is { } pendingTarget)
        {
            ApplyPendingInstallGateChrome(pendingTarget);
            return;
        }

        _suppress = true;
        try
        {
            // Во время start/stop снимок presence: прямой COM планировщика блокирует UI.
            var state = QueryUiState();
            var gateTaskExists = QueryInstallGateTaskExists();
            if (_busy)
                return;

            UpdateOrchestrationKindLabel();

            if (IsX86ControlsLocked)
            {
                ApplyExternalBusyChrome(false, splitEnabled: false);
                _installGate.IsOn = false;
                _installGate.IsEnabled = false;
                _stackable.SetStatusTone(
                    GlassSettingsResources.Get("Status_PlatformNotInstalled"),
                    PlatformSchedulerStackableActions.StatusToneKind.Disabled);
                return;
            }

            if (!gateTaskExists)
            {
                ApplyExternalBusyChrome(false, splitEnabled: false);
                _installGate.IsOn = false;
                _installGate.IsEnabled = true;
                _stackable.SetStatusTone(
                    GlassSettingsResources.Get("Status_Disabled"),
                    PlatformSchedulerStackableActions.StatusToneKind.Disabled);
                return;
            }

            if (Kind == PlatformSchedulerKind.Glass)
            {
                // Check named lifecycle busy event (set by ShellExt during menu start/stop).
                var externalBusy = PlatformLifecycleBusy.IsBusy(EffectiveGlassArch);
                var combinedBusy = state.IsLifecycleBusy || externalBusy;
                ApplyExternalBusyChrome(combinedBusy, splitEnabled: gateTaskExists && !combinedBusy);
                _installGate.IsOn = gateTaskExists;
                if (state.IsUnloading || (externalBusy && state.Lifecycle is NativeHostInterop.InjectLifecycleUi.Stopping or NativeHostInterop.InjectLifecycleUi.Residual))
                {
                    _stackable.SetStatusTone(
                        GlassSettingsResources.Get("Status_Unloading"),
                        PlatformSchedulerStackableActions.StatusToneKind.Unloading);
                }
                else
                {
                    _stackable.SetStatusTone(
                        StatusCaption(gateTaskExists, state.Autorun),
                        state.Autorun switch
                        {
                            GlassAutorunUiState.Running => PlatformSchedulerStackableActions.StatusToneKind.Running,
                            GlassAutorunUiState.Ready => PlatformSchedulerStackableActions.StatusToneKind.Ready,
                            _ => PlatformSchedulerStackableActions.StatusToneKind.Disabled
                        });
                }
                _schedDisabled = state.Autorun == GlassAutorunUiState.Disabled;
            }
            else
            {
                ApplyExternalBusyChrome(false, splitEnabled: gateTaskExists);
                _installGate.IsOn = gateTaskExists;
                var st = gateTaskExists
                    ? (OpenGlassHostUiState)(int)state.Autorun
                    : OpenGlassHostUiState.NoTask;
                _stackable.SetStatusTone(
                    OgStatusCaption(gateTaskExists, st),
                    st switch
                    {
                        OpenGlassHostUiState.Running => PlatformSchedulerStackableActions.StatusToneKind.Running,
                        OpenGlassHostUiState.Ready => PlatformSchedulerStackableActions.StatusToneKind.Ready,
                        _ => PlatformSchedulerStackableActions.StatusToneKind.Disabled
                    });
                _schedDisabled = !OpenGlassHostService.IsOrchestrationAutostartEnabled();
            }

            _stackable.SetMenuToggleText(_schedDisabled
                ? GlassSettingsResources.Get("Status_Enable")
                : GlassSettingsResources.Get("Status_Disable"));
        }
        finally
        {
            _suppress = false;
        }
    }

    public void RefreshStatusChrome() => _stackable.RefreshStatusChrome();

    private void UpdateOrchestrationKindLabel()
    {
        if (Kind == PlatformSchedulerKind.Glass && GlassArch is { } arch)
        {
            _stackable.SetOrchestrationKindLabel(arch == GlassPlatformArch.X86
                ? GlassSettingsResources.Get("System_OrchestrationTaskX86")
                : GlassSettingsResources.Get("System_OrchestrationTaskX64"));
            return;
        }

        var backend = Kind == PlatformSchedulerKind.Glass
            ? OpenGlassOrchestrationBackend.Task
            : OpenGlassHostService.ResolveOrchestrationBackend();

        _stackable.SetOrchestrationKindLabel(backend == OpenGlassOrchestrationBackend.Service
            ? GlassSettingsResources.Get("System_OrchestrationService")
            : GlassSettingsResources.Get("System_OrchestrationTask"));
    }

    /// <summary>BusyRing + disable без локального _busy (stop/start из ShellExt при открытом GS).</summary>
    private void ApplyExternalBusyChrome(bool busy, bool splitEnabled)
    {
        _stackable.SetBusyVisual(busy, splitEnabled);
        _installGate.IsEnabled = !busy;
    }

    private void SetBusyVisual(bool busy)
    {
        _busy = busy;
        ApplyExternalBusyChrome(busy, TaskExists);
    }

    private async Task BeginBusyVisualAsync()
    {
        SetBusyVisual(true);
        _stackable.UpdateLayout();
        var frameReady = new TaskCompletionSource<bool>();
        _stackable.DispatcherQueue.TryEnqueue(Microsoft.UI.Dispatching.DispatcherQueuePriority.Normal, () =>
        {
            _stackable.UpdateLayout();
            _stackable.DispatcherQueue.TryEnqueue(
                Microsoft.UI.Dispatching.DispatcherQueuePriority.Low,
                () => frameReady.TrySetResult(true));
        });
        await frameReady.Task.ConfigureAwait(true);
    }

    private static string StatusCaption(bool exists, GlassAutorunUiState st)
    {
        if (!exists)
            return GlassSettingsResources.Get("Status_Disabled");

        return st switch
        {
            GlassAutorunUiState.Running => GlassSettingsResources.Get("Status_Running"),
            GlassAutorunUiState.Ready => GlassSettingsResources.Get("Status_Ready"),
            _ => GlassSettingsResources.Get("Status_Disabled")
        };
    }

    private static string OgStatusCaption(bool exists, OpenGlassHostUiState st)
    {
        if (!exists)
            return GlassSettingsResources.Get("Status_Disabled");

        return st switch
        {
            OpenGlassHostUiState.Running => GlassSettingsResources.Get("Status_Running"),
            OpenGlassHostUiState.Ready => GlassSettingsResources.Get("Status_Ready"),
            _ => GlassSettingsResources.Get("Status_Disabled")
        };
    }

    private void OnStatusSplitClick()
    {
        var currentTaskExists = TaskExists;
        var currentBusy = _busy;

        if (!currentTaskExists || currentBusy)
            return;

        if (Kind == PlatformSchedulerKind.Glass)
        {
            var life = NativeHostInterop.QueryInjectLifecycleUi(EffectiveGlassArch);
            var extBusy = PlatformLifecycleBusy.IsBusy(EffectiveGlassArch);
            // Block during any lifecycle transition (Starting, Stopping, Residual) or external busy.
            if (life is not NativeHostInterop.InjectLifecycleUi.Idle
                and not NativeHostInterop.InjectLifecycleUi.Running
                || extBusy)
                return;

            var st = GlassPlatformService.QueryGlassAutorunUiState(EffectiveGlassArch);
            if (st == GlassAutorunUiState.Running)
            {
                _ = StopGlassPlatformAsync();
                return;
            }

            if (st == GlassAutorunUiState.Ready)
            {
                _ = StartGlassPlatformAsync();
                return;
            }
        }
        else
        {
            var st = OpenGlassHostService.QueryOpenGlassHostUiState();
            if (st == OpenGlassHostUiState.Running)
            {
                _ = StopOpenGlassAsync();
                return;
            }

            if (st == OpenGlassHostUiState.Ready)
            {
                _ = RunOpenGlassStartAsync();
                return;
            }
        }

        Refresh();
        StateChanged?.Invoke(this, EventArgs.Empty);
    }

    private async Task StopGlassPlatformAsync()
    {
        if (_busy)
            return;

        await BeginBusyVisualAsync().ConfigureAwait(true);
        // Show "Unloading" immediately — don't wait for snapshot update.
        _stackable.SetStatusTone(
            GlassSettingsResources.Get("Status_Unloading"),
            PlatformSchedulerStackableActions.StatusToneKind.Unloading);
        try
        {
            await SystemPlatformRunActions.StopGlassAsync(EffectiveGlassArch).ConfigureAwait(true);
        }
        finally
        {
            FinishBusyRefresh();
        }
    }

    private async Task StartGlassPlatformAsync()
    {
        if (_busy)
            return;

        await BeginBusyVisualAsync().ConfigureAwait(true);
        try
        {
            _ = await SystemPlatformRunActions.StartGlassAsync(EffectiveGlassArch).ConfigureAwait(true);
        }
        finally
        {
            FinishBusyRefresh();
        }
    }

    private void FinishBusyRefresh()
    {
        _busy = false;
        _pendingGateTarget = null;
        Refresh();
        StateChanged?.Invoke(this, EventArgs.Empty);
    }

    private async Task RunOpenGlassStartAsync()
    {
        if (_busy)
            return;

        await BeginBusyVisualAsync().ConfigureAwait(true);
        try
        {
            if (!await SystemPlatformRunActions.StartOpenGlassAsync().ConfigureAwait(true))
                return;
        }
        finally
        {
            FinishBusyRefresh();
        }
    }

    private async Task StopOpenGlassAsync()
    {
        if (_busy)
            return;

        await BeginBusyVisualAsync().ConfigureAwait(true);
        try
        {
            await SystemPlatformRunActions.StopOpenGlassAsync().ConfigureAwait(true);
        }
        finally
        {
            FinishBusyRefresh();
        }
    }

    private void InstallGate_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_suppress || _busy)
            return;

        if (IsX86ControlsLocked)
        {
            _suppress = true;
            _installGate.IsOn = false;
            _installGate.IsEnabled = false;
            _suppress = false;
            return;
        }

        var wantInstalled = _installGate.IsOn;
        var directExists = QueryDirectState().TaskExists;
        if (wantInstalled == directExists)
        {
            if (_installGate.IsOn != directExists)
            {
                _suppress = true;
                _installGate.IsOn = directExists;
                _suppress = false;
            }

            return;
        }

        BeginInstallGateOperation(wantInstalled);
        StateChanged?.Invoke(this, EventArgs.Empty);
        _ = ApplyInstallToggleAsync(wantInstalled);
    }

    private void BeginInstallGateOperation(bool targetOn)
    {
        _pendingGateTarget = targetOn;
        _busy = true;
        ApplyPendingInstallGateChrome(targetOn);
    }

    private void ApplyPendingInstallGateChrome(bool targetOn)
    {
        _suppress = true;
        try
        {
            _installGate.IsOn = targetOn;
            _installGate.IsEnabled = false;
            _stackable.SetBusyVisual(true, splitEnabled: false);
            _stackable.SetStatusTone(
                GlassSettingsResources.Get("Status_Disabled"),
                PlatformSchedulerStackableActions.StatusToneKind.Disabled);
        }
        finally
        {
            _suppress = false;
        }
    }

    private async Task ApplyInstallToggleAsync(bool install)
    {
        if (IsX86ControlsLocked)
            return;

        try
        {
            var operationOk = false;
            if (install)
            {
                if (Kind == PlatformSchedulerKind.OpenGlass)
                    operationOk = await SystemPlatformRunActions.InstallOpenGlassOrchestrationAsync().ConfigureAwait(true);
                else
                    operationOk = await Task.Run(() => GlassPlatformService.TryInstallGlassAutorunTask(EffectiveGlassArch))
                        .ConfigureAwait(true);
            }
            else
            {
                await Task.Run(DeleteTask).ConfigureAwait(true);
                if (Kind == PlatformSchedulerKind.OpenGlass)
                    await ApplyOpenGlassStopPolicyAfterDeleteAsync().ConfigureAwait(true);
                operationOk = !QueryDirectState().TaskExists;
            }

            PlatformPresenceService.Instance.RefreshSnapshotSynchronouslyFromDirectQueries();
            PlatformPresenceService.Instance.NotifyLifecycleChanged();
            if (!operationOk || QueryDirectState().TaskExists != install)
                RestoreInstallGateFromDirectState();
        }
        finally
        {
            _pendingGateTarget = null;
            FinishBusyRefresh();
        }
    }

    private void RestoreInstallGateFromDirectState()
    {
        _suppress = true;
        try
        {
            var exists = QueryDirectState().TaskExists;
            _installGate.IsOn = exists;
        }
        finally
        {
            _suppress = false;
        }
    }

    private void DeleteTask()
    {
        if (Kind == PlatformSchedulerKind.Glass)
        {
            GlassPlatformService.TryRemoveGlassAutorunTaskWithStop(EffectiveGlassArch);
            return;
        }

        OpenGlassInstallService.Uninstall();
    }

    private void OnMenuToggleClick()
    {
        var enable = _schedDisabled;
        if (Kind == PlatformSchedulerKind.Glass)
            GlassPlatformService.SetGlassAutorunTaskSchedulerEnabled(EffectiveGlassArch, enable);
        else
            OpenGlassHostService.SetOrchestrationAutostartEnabled(enable);

        Refresh();
        StateChanged?.Invoke(this, EventArgs.Empty);
    }

    private void OnMenuDeleteClick()
    {
        if (Kind == PlatformSchedulerKind.Glass)
        {
            _ = RemoveGlassTaskAsync();
            return;
        }

        OpenGlassInstallService.Uninstall();
        _ = ApplyOpenGlassStopPolicyAfterDeleteAsync();
        Refresh();
        StateChanged?.Invoke(this, EventArgs.Empty);
    }

    private async Task RemoveGlassTaskAsync()
    {
        if (_busy)
            return;

        await BeginBusyVisualAsync().ConfigureAwait(true);
        try
        {
            await Task.Run(() => GlassPlatformService.TryRemoveGlassAutorunTaskWithStop(EffectiveGlassArch))
                .ConfigureAwait(true);
        }
        finally
        {
            FinishBusyRefresh();
        }
    }

    private async Task ApplyOpenGlassStopPolicyAfterDeleteAsync()
    {
        await SystemPlatformRunActions.ApplyOpenGlassStopPolicyAfterDeleteAsync().ConfigureAwait(true);
    }
}
