using System.Diagnostics;
using GlassSettings.Services;

namespace GlassSettings.Core.Platform;

public enum OpenGlassHostUiState { NoTask = 0, Disabled, Ready, Running }

public enum OpenGlassOrchestrationBackend { Task, Service }

public static class OpenGlassHostService
{
    private const string TaskNameCurrent = "OpenGlass Host";
    private const string TaskNameLegacy = "OpenGlass Legacy Host";

    public static bool LegacyHostTaskExists() =>
        NativeHostInterop.LegacyHostTaskExists();

    /// <summary>Backend OG: служба, если установлена или рядом с DLL есть OpenGlassHost.exe.</summary>
    public static OpenGlassOrchestrationBackend ResolveOrchestrationBackend()
    {
        if (NativeHostInterop.TryGetOpenGlassServiceBinPath(out _))
            return OpenGlassOrchestrationBackend.Service;

        if (LegacyHostTaskExists())
            return OpenGlassOrchestrationBackend.Task;

        return !string.IsNullOrEmpty(PlatformPaths.OpenGlassHostPath)
            ? OpenGlassOrchestrationBackend.Service
            : OpenGlassOrchestrationBackend.Task;
    }

    public static bool IsServiceBackendInstalled() =>
        NativeHostInterop.TryGetOpenGlassServiceBinPath(out _);

    public static bool IsServiceAutostartEnabled() =>
        NativeHostInterop.IsOpenGlassServiceAutostartEnabled();

    public static bool SetServiceAutostartEnabled(bool enable) =>
        NativeHostInterop.SetOpenGlassServiceAutostartEnabled(enable);

    public static bool SetOrchestrationAutostartEnabled(bool enable) =>
        ResolveOrchestrationBackend() == OpenGlassOrchestrationBackend.Service
            ? SetServiceAutostartEnabled(enable)
            : SetLegacyHostTaskSchedulerEnabled(enable);

    public static bool IsOrchestrationAutostartEnabled()
    {
        if (ResolveOrchestrationBackend() == OpenGlassOrchestrationBackend.Service)
            return IsServiceAutostartEnabled();

        return QueryOpenGlassHostUiState() != OpenGlassHostUiState.Disabled;
    }

    public static OpenGlassHostUiState QueryOpenGlassHostUiState() =>
        (OpenGlassHostUiState)(int)NativeHostInterop.QueryOpenGlassHostUiState();

    public static bool SetLegacyHostTaskSchedulerEnabled(bool enable) =>
        NativeHostInterop.SetLegacyHostTaskSchedulerEnabled(enable);

    public static bool RemoveLegacyHostTaskCompletely() =>
        NativeHostInterop.RemoveLegacyHostTaskCompletely();

    /// <summary>
    /// Установка задачи OpenGlass: rundll32 /install напрямую (процесс уже elevated).
    /// </summary>
    public static bool TryInstallLegacyHost()
    {
        try
        {
            var dll = PlatformPaths.OpenGlassDllPath;
            if (string.IsNullOrEmpty(dll))
                return LegacyHostTaskExists();

            var rundll32 = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.System),
                "rundll32.exe");
            var args = $"\"{dll}\",OpenGlass_RunDLL /install";

            using var process = Process.Start(new ProcessStartInfo(rundll32, args)
            {
                UseShellExecute = false,
                CreateNoWindow = true,
            });
            if (process is null)
                return false;

            process.WaitForExit(120_000);
            return process.ExitCode == 0 && LegacyHostTaskExists();
        }
        catch
        {
            return false;
        }
    }

    /// <summary>Остановка задачи через планировщик и её удаление (порядок: стоп → удалить).</summary>
    public static void StopAndRemoveLegacyHostTask()
    {
        StopLegacyHostViaScheduler();
        RemoveLegacyHostTaskCompletely();
        RunSchtasks($"/Delete /TN \"{TaskNameCurrent}\" /F");
        RunSchtasks($"/Delete /TN \"{TaskNameLegacy}\" /F");
    }

    public static void StopLegacyHostViaScheduler()
    {
        RunSchtasks($"/End /TN \"{TaskNameCurrent}\"");
        RunSchtasks($"/End /TN \"{TaskNameLegacy}\"");
    }

    public static bool TryStartHostNoWait() => NativeHostInterop.TryStartOpenGlassHostNoWait();

    public static bool TryLaunchStartup() => NativeHostInterop.TryLaunchOpenGlassStartup();

    /// <summary>Останавливает службу/задачу OpenGlass. Установку не снимает.</summary>
    public static bool TryStopOrchestration() => NativeHostInterop.TryLaunchOpenGlassShutdown();

    public static void TryLaunchShutdown() => TryStopOrchestration();

    private static void RunSchtasks(string args)
    {
        try
        {
            var sys = Environment.GetFolderPath(Environment.SpecialFolder.System);
            var psi = new ProcessStartInfo(Path.Combine(sys, "schtasks.exe"), args)
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true
            };
            using var p = Process.Start(psi);
            p?.WaitForExit(15000);
        }
        catch { /* ignore */ }
    }
}
