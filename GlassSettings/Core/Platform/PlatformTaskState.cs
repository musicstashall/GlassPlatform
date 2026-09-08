using GlassSettings.Services;

namespace GlassSettings.Core.Platform;

/// <summary>
/// Единый источник истины для состояния задачи платформы.
/// Консолидирует данные из snapshot (PlatformPresenceService) в один record.
/// </summary>
public readonly record struct PlatformTaskState(
    bool TaskExists,
    NativeHostInterop.InjectLifecycleUi Lifecycle,
    GlassAutorunUiState Autorun)
{
    public static readonly PlatformTaskState Default = new(false, default, default);

    public bool IsUnloading =>
        Lifecycle == NativeHostInterop.InjectLifecycleUi.Residual
        || Lifecycle == NativeHostInterop.InjectLifecycleUi.Stopping;

    public bool IsRunning => Autorun == GlassAutorunUiState.Running;

    public bool IsLifecycleBusy =>
        Lifecycle is NativeHostInterop.InjectLifecycleUi.Stopping
        or NativeHostInterop.InjectLifecycleUi.Residual
        or NativeHostInterop.InjectLifecycleUi.Starting;
}
