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
        var (code, _) = RunSchtasks($"/Delete /TN \"{GlassPlatformNames.GlassAutorunTaskNameFor(arch)}\" /F");
        return code == 0;
    }

    private static (int exitCode, string output) RunSchtasks(string args)
    {
        var sys = Environment.GetFolderPath(Environment.SpecialFolder.System);
        var exe = Path.Combine(sys, "schtasks.exe");
        var psi = new ProcessStartInfo(exe, args)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true
        };
        using var p = Process.Start(psi);
        if (p is null) return (-1, "");
        var o = p.StandardOutput.ReadToEnd() + p.StandardError.ReadToEnd();
        p.WaitForExit(20000);
        return (p.ExitCode, o);
    }
}
