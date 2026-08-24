using System.Diagnostics;
using GlassSettings.Services;

namespace GlassSettings.Core.Platform;

/// <summary>Установка OpenGlass: service-first (OpenGlassHost.exe), иначе legacy task /install.</summary>
public static class OpenGlassInstallService
{
    public static bool Install()
    {
        var exe = PlatformPaths.OpenGlassHostPath;
        if (!string.IsNullOrEmpty(exe))
            return TryInstallService(exe);

        return OpenGlassHostService.TryInstallLegacyHost();
    }

    public static void Uninstall()
    {
        if (NativeHostInterop.OpenGlassOrchestrationInstalled()
            && PlatformPaths.OpenGlassHostPath is not null)
        {
            NativeHostInterop.StopOpenGlassService();
            _ = NativeHostInterop.RemoveOpenGlassService();
        }

        OpenGlassHostService.StopAndRemoveLegacyHostTask();
    }

    private static bool TryInstallService(string exePath)
    {
        if (NativeHostInterop.InstallOpenGlassService(exePath))
            return NativeHostInterop.OpenGlassOrchestrationInstalled();

        return TryInstallServiceViaSc(exePath);
    }

    /// <summary>GlassSettings уже elevated (requireAdministrator) — sc.exe напрямую, без mshta/runas.</summary>
    private static bool TryInstallServiceViaSc(string exePath)
    {
        try
        {
            var sys = Environment.GetFolderPath(Environment.SpecialFolder.System);
            var sc = Path.Combine(sys, "sc.exe");
            var args =
                $"create OpenGlassHost binPath= \"\\\"{exePath}\\\"\" start= auto DisplayName= \"OpenGlassHost\"";

            using var process = Process.Start(new ProcessStartInfo(sc, args)
            {
                UseShellExecute = false,
                CreateNoWindow = true,
            });
            if (process is null)
                return false;

            process.WaitForExit(30_000);
            return process.ExitCode == 0 && NativeHostInterop.OpenGlassOrchestrationInstalled();
        }
        catch
        {
            return false;
        }
    }
}
