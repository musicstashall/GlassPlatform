using GlassSettings.Core.Customization;
using GlassSettings.Core.Registry;
using GlassSettings.Core.Snapshot;
using GlassSettings.Services;

namespace GlassSettings.Core.Platform;

/// <summary>Запуск/остановка Glass Platform и OpenGlass — общая логика для SystemPage и HomePage.</summary>
public static class SystemPlatformRunActions
{
    private static readonly TimeSpan DefaultRunningWait = TimeSpan.FromSeconds(7);
    private static readonly TimeSpan OpenGlassLaunchWait = TimeSpan.FromSeconds(12);
    private static readonly TimeSpan DeleteStopWait = TimeSpan.FromSeconds(5);

    public static Task<bool> StartGlassAsync() =>
        StartGlassAsync(GlassPlatformNames.NativeArch);

    public static async Task<bool> StartGlassAsync(GlassPlatformArch arch)
    {
        if (!await ConfirmSaveBeforePlatformStartAsync().ConfigureAwait(true))
            return false;

        using var busy = PlatformLifecycleBusy.Begin(arch);
        await Task.Run(() =>
        {
            _ = GlassPlatformService.TrySyncAutorunTaskActionPath(arch);
            GlassPlatformService.StartPlatform(arch);
        }).ConfigureAwait(true);
        if (!await WaitForGlassRunningAsync(arch, DefaultRunningWait).ConfigureAwait(true))
            return false;

        await SettingsSession.Current.ReloadBaselinesAfterPlatformSubsystemStartAsync().ConfigureAwait(true);
        PlatformPresenceService.Instance.NotifyLifecycleChanged();
        return true;
    }

    public static Task StopGlassAsync() => StopGlassAsync(GlassPlatformNames.NativeArch);

    public static async Task StopGlassAsync(GlassPlatformArch arch)
    {
        NativeHostInterop.SignalPlatformStop(arch);
        var busy = PlatformLifecycleBusy.TryBegin(arch);
        try
        {
            await Task.Run(() => GlassPlatformService.StopPlatformAndWait(arch)).ConfigureAwait(true);
            var residualDeadline = DateTime.UtcNow + TimeSpan.FromSeconds(20);
            var emptyStreak = 0;
            while (DateTime.UtcNow < residualDeadline)
            {
                if (!NativeHostInterop.RefreshInjectResidualSticky(arch))
                {
                    if (++emptyStreak >= 6)
                        break;
                }
                else
                {
                    emptyStreak = 0;
                }
                await Task.Delay(250).ConfigureAwait(true);
            }
        }
        finally
        {
            busy?.Dispose();
            PlatformPresenceService.Instance.NotifyLifecycleChanged();
        }
    }

    public static async Task<bool> InstallOpenGlassOrchestrationAsync()
    {
        var ok = await Task.Run(OpenGlassInstallService.Install).ConfigureAwait(true);
        return ok && OpenGlassHostService.LegacyHostTaskExists();
    }

    public static async Task<bool> StartOpenGlassAsync()
    {
        if (!await ConfirmSaveBeforePlatformStartAsync().ConfigureAwait(true))
            return false;

        if (IsOpenGlassLaunchComplete())
        {
            CompleteOpenGlassStartSuccess();
            return true;
        }

        var busy = OpenGlassLifecycleBusy.TryBegin();
        if (busy is null)
            return false;
        try
        {
            if (!await Task.Run(OpenGlassHostService.TryStartHostNoWait).ConfigureAwait(true))
                return false;

            if (!await WaitForOpenGlassLaunchCompleteAsync(OpenGlassLaunchWait).ConfigureAwait(true))
            {
                await RollbackFailedOpenGlassStartAsync().ConfigureAwait(true);
                return false;
            }
        }
        finally
        {
            busy.Dispose();
        }

        CompleteOpenGlassStartSuccess();
        return true;
    }

    /// <summary>OG реально работает: pipeline в dwm или orchestration Running + окно Dwm.</summary>
    private static bool IsOpenGlassLaunchComplete() =>
        NativeHostInterop.IsOpenGlassBackdropPipelineActive();

    private static void CompleteOpenGlassStartSuccess()
    {
        OpenGlassBorderPolicyCoordinator.NotifyOpenGlassStateChanged();
        SettingsSession.Current.ReloadDwmSnapshotBaseline();
        SettingsSession.Current.SyncScopeBaseline(SettingsCommitScope.ForArea(CustomizationAreaId.Taskbar));
    }

    public static async Task StopOpenGlassAsync()
    {
        var busy = OpenGlassLifecycleBusy.TryBegin();
        if (busy is null)
            return;
        try
        {
            await StopOpenGlassOrchestrationAsync().ConfigureAwait(true);
        }
        finally
        {
            busy.Dispose();
        }
        OpenGlassBorderPolicyCoordinator.NotifyOpenGlassStateChanged();
        NotifyRootBackdropAfterOpenGlassStop();
    }

    /// <summary>Откат после неудачного старта: только остановка службы/задачи, без удаления установки.</summary>
    private static async Task RollbackFailedOpenGlassStartAsync()
    {
        await StopOpenGlassOrchestrationAsync().ConfigureAwait(true);
        OpenGlassBorderPolicyCoordinator.NotifyOpenGlassStateChanged();
        NotifyRootBackdropAfterOpenGlassStop();
    }

    private static async Task StopOpenGlassOrchestrationAsync()
    {
        await Task.Run(OpenGlassHostService.TryStopOrchestration).ConfigureAwait(true);
        await WaitForOpenGlassStoppedAsync(DefaultRunningWait).ConfigureAwait(true);
    }

    public static async Task ApplyOpenGlassStopPolicyAfterDeleteAsync()
    {
        await WaitForOpenGlassStoppedAsync(DeleteStopWait).ConfigureAwait(true);
        OpenGlassBorderPolicyCoordinator.NotifyOpenGlassStateChanged();
        NotifyRootBackdropAfterOpenGlassStop();
    }

    private static void NotifyRootBackdropAfterOpenGlassStop()
    {
        var noBackdrop = RootNoBackdropPolicy.ReadEffectiveNoBackdrop();
        RootBackdropLiveReload.NotifyNoBackdropChange(noBackdrop);
    }

    private static async Task<bool> ConfirmSaveBeforePlatformStartAsync()
    {
        if (!SettingsSession.Current.HasUnsavedChanges)
            return true;

        if (!await SaveBeforePlatformDialogService.ConfirmSaveAndStartAsync().ConfigureAwait(true))
            return false;

        await SettingsSession.Current.ApplyCommitAsync().ConfigureAwait(true);
        return true;
    }

    private static Task WaitForGlassRunningAsync(TimeSpan timeout) =>
        WaitForGlassRunningAsync(GlassPlatformNames.NativeArch, timeout);

    private static async Task<bool> WaitForGlassRunningAsync(GlassPlatformArch arch, TimeSpan timeout)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (DateTime.UtcNow < deadline)
        {
            if (GlassPlatformService.IsGlassPlatformRunning(arch))
                return true;

            await Task.Delay(250).ConfigureAwait(true);
        }

        return GlassPlatformService.IsGlassPlatformRunning(arch);
    }

    private static Task WaitForGlassStoppedAsync(TimeSpan timeout) =>
        WaitForGlassStoppedAsync(GlassPlatformNames.NativeArch, timeout);

    private static async Task WaitForGlassStoppedAsync(GlassPlatformArch arch, TimeSpan timeout)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (DateTime.UtcNow < deadline)
        {
            // Mutex — источник истины (как при Start). TASK_STATE_RUNNING без mutex — зомби.
            if (!GlassPlatformService.IsGlassPlatformRunning(arch))
            {
                NativeHostInterop.StopOrphanAutorunTaskInstance(arch);
                return;
            }

            await Task.Delay(250).ConfigureAwait(true);
        }

        if (!GlassPlatformService.IsGlassPlatformRunning(arch))
            NativeHostInterop.StopOrphanAutorunTaskInstance(arch);
    }

    private static async Task<bool> WaitForOpenGlassLaunchCompleteAsync(TimeSpan timeout)
    {
        if (IsOpenGlassLaunchComplete())
            return true;

        var deadline = DateTime.UtcNow + timeout;
        while (DateTime.UtcNow < deadline)
        {
            NativeHostInterop.RequestPlatformPresenceRefresh();

            if (IsOpenGlassLaunchComplete())
                return true;

            await Task.Delay(250).ConfigureAwait(true);
        }

        return IsOpenGlassLaunchComplete();
    }

    private static async Task WaitForOpenGlassStoppedAsync(TimeSpan timeout)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (DateTime.UtcNow < deadline)
        {
            var st = OpenGlassHostService.QueryOpenGlassHostUiState();
            if (st is OpenGlassHostUiState.Ready or OpenGlassHostUiState.Disabled or OpenGlassHostUiState.NoTask)
                return;

            await Task.Delay(250).ConfigureAwait(true);
        }
    }
}
