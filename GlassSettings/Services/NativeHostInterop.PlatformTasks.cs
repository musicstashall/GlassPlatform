using System.Runtime.InteropServices;
using GlassSettings.Core.Platform;

namespace GlassSettings.Services;

public static partial class NativeHostInterop
{
    public enum NativeOpenGlassHostUiState
    {
        NoTask = 0,
        Disabled = 1,
        Ready = 2,
        Running = 3
    }

    public enum NativeGlassAutorunUiState
    {
        NoTask = 0,
        Disabled = 1,
        Ready = 2,
        Running = 3
    }

    public static NativeOpenGlassHostUiState QueryOpenGlassHostUiState() =>
        (NativeOpenGlassHostUiState)Native_QueryOpenGlassHostUiState();

    public static bool LegacyHostTaskExists() => Native_LegacyHostTaskExists();

    public static bool TryStartOpenGlassHostNoWait() => Native_TryStartOpenGlassHostNoWait();

    public static bool TryLaunchOpenGlassStartup() => Native_TryLaunchOpenGlassStartup();

    public static bool TryLaunchOpenGlassShutdown() => Native_TryLaunchOpenGlassShutdown();

    public static bool SetLegacyHostTaskSchedulerEnabled(bool enable) =>
        Native_SetLegacyHostTaskSchedulerEnabled(enable);

    public static bool RemoveLegacyHostTaskCompletely() => Native_RemoveLegacyHostTaskCompletely();

    public static NativeGlassAutorunUiState QueryGlassAutorunUiState() =>
        QueryGlassAutorunUiState(GlassPlatformNames.NativeArch);

    public static NativeGlassAutorunUiState QueryGlassAutorunUiState(GlassPlatformArch arch) =>
        (NativeGlassAutorunUiState)Native_QueryGlassAutorunUiStateForArch((int)arch);

    public static bool GlassAutorunTaskExistsNative() =>
        GlassAutorunTaskExistsNative(GlassPlatformNames.NativeArch);

    public static bool GlassAutorunTaskExistsNative(GlassPlatformArch arch) =>
        Native_GlassAutorunTaskExistsForArch((int)arch);

    public static bool SignalPlatformStop() => SignalPlatformStop(GlassPlatformNames.NativeArch);

    public static bool SignalPlatformStop(GlassPlatformArch arch) =>
        Native_SignalPlatformStopForArch((int)arch);

    public static bool WaitForPlatformStopped(uint timeoutMs = 30000) =>
        WaitForPlatformStopped(GlassPlatformNames.NativeArch, timeoutMs);

    public static bool WaitForPlatformStopped(GlassPlatformArch arch, uint timeoutMs = 30000) =>
        Native_WaitForPlatformStoppedForArch((int)arch, timeoutMs);

    /// <summary>Зомби Task Scheduler Running без mutex — IRegisteredTask::Stop / schtasks /End.</summary>
    public static bool StopOrphanAutorunTaskInstance(GlassPlatformArch arch) =>
        Native_StopOrphanAutorunTaskInstanceForArch((int)arch);

    public static void StartPlatform() => StartPlatform(GlassPlatformNames.NativeArch);

    public static void StartPlatform(GlassPlatformArch arch) =>
        Native_StartPlatformForArch((int)arch);

    public static bool SetGlassAutorunTaskSchedulerEnabled(bool enable) =>
        SetGlassAutorunTaskSchedulerEnabled(GlassPlatformNames.NativeArch, enable);

    public static bool SetGlassAutorunTaskSchedulerEnabled(GlassPlatformArch arch, bool enable) =>
        Native_SetGlassAutorunTaskSchedulerEnabledForArch((int)arch, enable);

    /// <summary>Флаги питания/простоя задачи уже не убивают хост?</summary>
    public static bool IsAutorunTaskPowerIndependent(GlassPlatformArch arch) =>
        Native_IsAutorunTaskPowerIndependentForArch((int)arch);

    /// <summary>Перерегистрация задачи без зависимости от питания (нужны права администратора).</summary>
    public static bool ApplyAutorunTaskPowerIndependence(GlassPlatformArch arch) =>
        Native_ApplyAutorunTaskPowerIndependenceForArch((int)arch);

    /// <summary>Переписать TrayXamlPolicy map из реестра (до reload UICompat в SEH).</summary>
    public static bool PublishTrayXamlPolicy() => Native_PublishTrayXamlPolicy();

    [DllImport(Dll, EntryPoint = "GlassSettings_QueryOpenGlassHostUiState")]
    private static extern int Native_QueryOpenGlassHostUiState();

    [DllImport(Dll, EntryPoint = "GlassSettings_LegacyHostTaskExists")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_LegacyHostTaskExists();

    [DllImport(Dll, EntryPoint = "GlassSettings_TryStartOpenGlassHostNoWait")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_TryStartOpenGlassHostNoWait();

    [DllImport(Dll, EntryPoint = "GlassSettings_TryLaunchOpenGlassStartup")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_TryLaunchOpenGlassStartup();

    [DllImport(Dll, EntryPoint = "GlassSettings_TryLaunchOpenGlassShutdown")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_TryLaunchOpenGlassShutdown();

    [DllImport(Dll, EntryPoint = "GlassSettings_SetLegacyHostTaskSchedulerEnabled")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_SetLegacyHostTaskSchedulerEnabled(
        [MarshalAs(UnmanagedType.Bool)] bool enable);

    [DllImport(Dll, EntryPoint = "GlassSettings_RemoveLegacyHostTaskCompletely")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_RemoveLegacyHostTaskCompletely();

    [DllImport(Dll, EntryPoint = "GlassSettings_QueryGlassAutorunUiStateForArch")]
    private static extern int Native_QueryGlassAutorunUiStateForArch(int arch);

    [DllImport(Dll, EntryPoint = "GlassSettings_GlassAutorunTaskExistsForArch")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_GlassAutorunTaskExistsForArch(int arch);

    [DllImport(Dll, EntryPoint = "GlassSettings_SignalPlatformStopForArch")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_SignalPlatformStopForArch(int arch);

    [DllImport(Dll, EntryPoint = "GlassSettings_WaitForPlatformStoppedForArch")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_WaitForPlatformStoppedForArch(int arch, uint timeoutMs);

    [DllImport(Dll, EntryPoint = "GlassSettings_StopOrphanAutorunTaskInstanceForArch")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_StopOrphanAutorunTaskInstanceForArch(int arch);

    [DllImport(Dll, EntryPoint = "GlassSettings_StartPlatformForArch")]
    private static extern void Native_StartPlatformForArch(int arch);

    [DllImport(Dll, EntryPoint = "GlassSettings_SetGlassAutorunTaskSchedulerEnabledForArch")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_SetGlassAutorunTaskSchedulerEnabledForArch(
        int arch,
        [MarshalAs(UnmanagedType.Bool)] bool enable);

    [DllImport(Dll, EntryPoint = "GlassSettings_IsAutorunTaskPowerIndependentForArch")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_IsAutorunTaskPowerIndependentForArch(int arch);

    [DllImport(Dll, EntryPoint = "GlassSettings_ApplyAutorunTaskPowerIndependenceForArch")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_ApplyAutorunTaskPowerIndependenceForArch(int arch);

    [DllImport(Dll, EntryPoint = "GlassSettings_PublishTrayXamlPolicy")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Native_PublishTrayXamlPolicy();

    public static void InvalidateOGCache() => Native_InvalidateOGCache();

    [DllImport(Dll, EntryPoint = "GlassSettings_InvalidateOGCache")]
    private static extern void Native_InvalidateOGCache();
}
