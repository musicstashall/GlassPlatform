using System.Diagnostics;
using System.Runtime.InteropServices;
using GlassSettings.Core.Platform;
using Microsoft.Win32;

namespace GlassSettings.Services;

/// <summary>Per-machine SecureUxTheme: установка и удаление только через MSI из кэша.</summary>
public static class SecureUxThemeMsiService
{
    public const string ProductCodeX64 = "{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5076}";
    public const string ProductCodeX86 = "{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5077}";

    public static string ProductCodeForOs() =>
        Environment.Is64BitOperatingSystem ? ProductCodeX64 : ProductCodeX86;

    public static bool IsInstalled()
    {
        var state = MsiQueryProductState(ProductCodeForOs());
        return state == InstallState.Default;
    }

    public static bool CacheMsiAvailable() =>
        !string.IsNullOrEmpty(GlassMaintenanceSetupPaths.SecureUxThemeMsiPath());

    public static InstallResult InstallFromCache()
    {
        var msi = GlassMaintenanceSetupPaths.SecureUxThemeMsiPath();
        if (string.IsNullOrEmpty(msi))
            return new InstallResult(unchecked((int)0x80070002), false);

        var exit = RunMsiexec($"/i \"{msi}\" /qn REBOOT=ReallySuppress");
        return new InstallResult(exit, RebootRequired(exit));
    }

    public static InstallResult UninstallProduct()
    {
        var code = ProductCodeForOs();
        if (!IsInstalled())
            return new InstallResult(0, false);

        var exit = RunMsiexec($"/x {code} /qn REBOOT=ReallySuppress");
        if (exit is 0 or 3010 or 1641 or 1605)
        {
            if (IsInstalled())
                return new InstallResult(1603, RebootRequired(exit));
            RemoveOrphanUninstallEntry(code);
            return new InstallResult(0, RebootRequired(exit));
        }
        return new InstallResult(exit, false);
    }

    private static void RemoveOrphanUninstallEntry(string productCode)
    {
        if (IsInstalled())
            return;
        try
        {
            using var root = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64);
            root.DeleteSubKeyTree($@"Software\Microsoft\Windows\CurrentVersion\Uninstall\{productCode}", false);
        }
        catch
        {
            // Нет прав или ключ уже снят MSI.
        }
    }

    private static int RunMsiexec(string arguments)
    {
        using var process = new Process();
        process.StartInfo = new ProcessStartInfo
        {
            FileName = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "msiexec.exe"),
            Arguments = arguments,
            UseShellExecute = false,
            CreateNoWindow = true
        };
        process.Start();
        process.WaitForExit();
        return process.ExitCode;
    }

    // UI callers test HRESULT < 0, not a positive Win32/MSI exit code.
    public static int ErrorHResult(int exitCode) => exitCode is 0 or 3010 or 1641
        ? 0 : exitCode < 0 ? exitCode : unchecked((int)(0x80070000u | ((uint)exitCode & 0xffffu)));

    private static bool RebootRequired(int exitCode) =>
        exitCode is 3010 or 1641;

    private enum InstallState
    {
        Unknown = -1,
        Absent = 1,
        Default = 5
    }

    [DllImport("msi.dll", CharSet = CharSet.Unicode)]
    private static extern InstallState MsiQueryProductState(string product);

    public readonly record struct InstallResult(int ExitCode, bool RebootRequired)
    {
        public bool Success => ExitCode is 0 or 3010 or 1641;
    }
}
