using System.Diagnostics;
using GlassSettings.Services;

namespace GlassSettings.Core.Platform;

public enum GlassAutorunUiState { NoTask = 0, Disabled, Ready, Running }

public static partial class GlassPlatformService
{
    public const int DefaultPlatformShutdownWaitMs = 30000;

    public static bool IsGlassPlatformRunning() => IsGlassPlatformRunning(GlassPlatformNames.NativeArch);

    public static bool IsGlassPlatformRunning(GlassPlatformArch arch) =>
        ReloadRenderService.IsGlassPlatformRunning(arch);

    public static bool GlassAutorunTaskExists() => GlassAutorunTaskExists(GlassPlatformNames.NativeArch);

    public static bool GlassAutorunTaskExists(GlassPlatformArch arch) =>
        NativeHostInterop.GlassAutorunTaskExistsNative(arch);

    /// <summary>
    /// Наличие задачи для UI. Пока идёт старт или стоп любой битности, планировщик
    /// с диспетчера не вызывается — берётся снимок presence.
    /// </summary>
    public static bool GlassAutorunTaskExistsForUi(GlassPlatformArch arch)
    {
        var snap = PlatformPresenceService.Instance.CurrentSnapshot;
        if (snap.Revision > 0)
        {
            return arch == GlassPlatformArch.X64
                ? snap.GlassTaskExistsX64 != 0
                : snap.GlassTaskExistsX86 != 0;
        }

        if (IsAnyLifecycleBusy())
            return true;

        return GlassAutorunTaskExists(arch);
    }

    /// <summary>Busy start/stop: COM планировщика на UI в этот момент блокирует окно.</summary>
    public static bool IsAnyLifecycleBusy() =>
        PlatformLifecycleBusy.IsBusy(GlassPlatformArch.X64)
        || PlatformLifecycleBusy.IsBusy(GlassPlatformArch.X86);

    /// <summary>Старт в полёте: тумблер остаётся вкл, пока mutex ещё не Running. Стоп не считается.</summary>
    public static bool IsStartInFlight(GlassPlatformArch arch, NativeHostInterop.InjectLifecycleUi life)
    {
        if (life is NativeHostInterop.InjectLifecycleUi.Stopping
            or NativeHostInterop.InjectLifecycleUi.Residual)
            return false;

        if (life == NativeHostInterop.InjectLifecycleUi.Starting)
            return true;

        return PlatformLifecycleBusy.IsBusy(arch) && !HomeStatusSummary.IsStopEventSignaled(arch);
    }

    public static GlassAutorunUiState QueryGlassAutorunUiState() =>
        QueryGlassAutorunUiState(GlassPlatformNames.NativeArch);

    public static GlassAutorunUiState QueryGlassAutorunUiState(GlassPlatformArch arch) =>
        (GlassAutorunUiState)(int)NativeHostInterop.QueryGlassAutorunUiState(arch);

    public static void StartPlatform() => StartPlatform(GlassPlatformNames.NativeArch);

    public static void StartPlatform(GlassPlatformArch arch) => NativeHostInterop.StartPlatform(arch);

    public static void StopPlatform() => StopPlatform(GlassPlatformNames.NativeArch);

    public static void StopPlatform(GlassPlatformArch arch) => NativeHostInterop.SignalPlatformStop(arch);

    public static bool WaitForPlatformStopped(int timeoutMs = DefaultPlatformShutdownWaitMs) =>
        WaitForPlatformStopped(GlassPlatformNames.NativeArch, timeoutMs);

    public static bool WaitForPlatformStopped(GlassPlatformArch arch, int timeoutMs = DefaultPlatformShutdownWaitMs) =>
        NativeHostInterop.WaitForPlatformStopped(arch, (uint)timeoutMs);

    public static void StopPlatformAndWait(int timeoutMs = DefaultPlatformShutdownWaitMs) =>
        StopPlatformAndWait(GlassPlatformNames.NativeArch, timeoutMs);

    public static void StopPlatformAndWait(GlassPlatformArch arch, int timeoutMs = DefaultPlatformShutdownWaitMs)
    {
        NativeHostInterop.SignalPlatformStop(arch);
        NativeHostInterop.WaitForPlatformStopped(arch, (uint)timeoutMs);
        // После ForceUnload/краша: mutex уже нет, StopEvent недоступен, задача всё ещё Running.
        if (!IsGlassPlatformRunning(arch))
            NativeHostInterop.StopOrphanAutorunTaskInstance(arch);
    }

    public static bool SetGlassAutorunTaskSchedulerEnabled(bool enable) =>
        SetGlassAutorunTaskSchedulerEnabled(GlassPlatformNames.NativeArch, enable);

    public static bool SetGlassAutorunTaskSchedulerEnabled(GlassPlatformArch arch, bool enable) =>
        NativeHostInterop.SetGlassAutorunTaskSchedulerEnabled(arch, enable);

    public static bool RemoveGlassAutorunTask() => RemoveGlassAutorunTask(GlassPlatformNames.NativeArch);

    public static bool RemoveGlassAutorunTask(GlassPlatformArch arch)
    {
        var (code, _) = RunSchtasks(
            "/Delete",
            "/TN",
            GlassPlatformNames.GlassAutorunTaskNameFor(arch),
            "/F");
        return code == 0;
    }

    internal static (int exitCode, string output) RunSchtasks(params string[] args)
    {
        var sys = Environment.GetFolderPath(Environment.SpecialFolder.System);
        var exe = Path.Combine(sys, "schtasks.exe");
        var psi = new ProcessStartInfo(exe)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true
        };
        foreach (var arg in args)
            psi.ArgumentList.Add(arg);

        using var p = Process.Start(psi);
        if (p is null) return (-1, "");
        var o = p.StandardOutput.ReadToEnd() + p.StandardError.ReadToEnd();
        p.WaitForExit(20000);
        return (p.ExitCode, o);
    }
}
