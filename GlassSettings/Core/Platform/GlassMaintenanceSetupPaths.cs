namespace GlassSettings.Core.Platform;

/// <summary>Кэш установщика в ProgramData (без буквы диска из реестра).</summary>
public static class GlassMaintenanceSetupPaths
{
    public static string CurrentDirectory()
    {
        var programData = Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData);
        return Path.Combine(programData, "Packages", "GlassPlatform", "Current");
    }

    public static string SetupExecutablePath()
    {
        var setup = Path.Combine(CurrentDirectory(), "GlassSetup.exe");
        return File.Exists(setup) ? setup : string.Empty;
    }

    public static string SecureUxThemeMsiPath()
    {
        var name = Environment.Is64BitOperatingSystem
            ? "SecureUxTheme-x64.msi"
            : "SecureUxTheme-x86.msi";
        var path = Path.Combine(CurrentDirectory(), name);
        return File.Exists(path) ? path : string.Empty;
    }
}
