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

        Process.Start(new ProcessStartInfo(setup)
        {
            Arguments = "--modify",
            UseShellExecute = true
        });
        return true;
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
            process.WaitForExit(60_000);
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
        return File.Exists(NativeDllName) ? Path.GetFullPath(NativeDllName) : null;
    }
}
