namespace GlassSettings.Core.Platform;

/// <summary>
/// HKCU InstallPath от установщика. x64 — Software\GlassPlatform;
/// x86 из процесса x64 — явно Wow6432Node (иначе 64-bit view даст путь x64).
/// </summary>
public static class PlatformInstallRegistry
{
    public const string SubKey = @"Software\GlassPlatform";
    public const string Wow6432SubKey = @"Software\Wow6432Node\GlassPlatform";
    public const string InstallPathValue = "InstallPath";

    public static string? TryGetInstallPath(GlassPlatformArch arch)
    {
        var subKey = ResolveSubKey(arch);
        if (subKey is null)
            return null;

        try
        {
            using var key = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(subKey);
            var raw = key?.GetValue(InstallPathValue) as string;
            return NormalizeDir(raw);
        }
        catch
        {
            return null;
        }
    }

    private static string? ResolveSubKey(GlassPlatformArch arch)
    {
        if (arch == GlassPlatformArch.X86
            && Environment.Is64BitProcess
            && Environment.Is64BitOperatingSystem)
            return Wow6432SubKey;

        if (arch == GlassPlatformArch.X64 && !Environment.Is64BitProcess)
            return null;

        return SubKey;
    }

    private static string? NormalizeDir(string? path)
    {
        if (string.IsNullOrWhiteSpace(path))
            return null;

        return path.Trim().TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
    }
}
