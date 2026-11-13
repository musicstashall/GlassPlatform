using System.Diagnostics;
using GlassSettings.Core.Platform;

namespace GlassSettings.Services;

/// <summary>Запуск кэшированного GlassSetup в режиме обслуживания (--modify).</summary>
public static class GlassMaintenanceSetupLaunchService
{
    private const string NativeDllName = "GlassSettings.Native.dll";
    private const string TicketExport = "GlassSettings_IssueMaintenanceSetupTicketV1W";

    public static bool TryLaunch()
    {
        var setup = GlassMaintenanceSetupPaths.SetupExecutablePath();
        if (string.IsNullOrEmpty(setup))
            return false;

        if (!TryIssueMaintenanceTicket())
            return false;

        try
        {
            using var process = Process.Start(new ProcessStartInfo(setup)
            {
                Arguments = "--modify",
                UseShellExecute = true
            });
            return process is not null;
        }
        catch (System.ComponentModel.Win32Exception) { return false; }
        catch (InvalidOperationException) { return false; }
        catch (IOException) { return false; }
    }

    private static bool TryIssueMaintenanceTicket()
    {
        var nativeDll = ResolveNativeDllPath();
        if (string.IsNullOrEmpty(nativeDll))
            return false;

        var rundll = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "rundll32.exe");
        var arguments = $"\"{nativeDll}\",{TicketExport}";

        try
        {
            using var process = Process.Start(new ProcessStartInfo
            {
                FileName = rundll,
                Arguments = arguments,
                Verb = "runas",
                UseShellExecute = true
            });
            if (process is null)
                return false;
            if (!process.WaitForExit(60_000))
                return false;
            return process.ExitCode == 0;
        }
        catch (System.ComponentModel.Win32Exception)
        {
            return false;
        }
    }

    private static string? ResolveNativeDllPath()
    {
        var nextToExe = Path.Combine(PlatformPaths.AppBase, NativeDllName);
        if (File.Exists(nextToExe))
            return nextToExe;
        // Never execute a helper selected from the process working directory.
        return null;
    }
}
