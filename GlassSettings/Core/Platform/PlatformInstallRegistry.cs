namespace GlassSettings.Core.Platform;

/// <summary>
/// HKCU/HKLM InstallPath от установщика.
/// Новая схема без WOW64-коллизий: InstallPathX64 и InstallPathX86 в Software\GlassPlatform;
/// для обратной совместимости сохраняется fallback на legacy InstallPath / Wow6432Node.
/// </summary>
public static class PlatformInstallRegistry
{
    public const string SubKey = @"Software\GlassPlatform";
    public const string Wow6432SubKey = @"Software\Wow6432Node\GlassPlatform";
    public const string InstallPathValue = "InstallPath";
    public const string InstallPathX64Value = "InstallPathX64";
    public const string InstallPathX86Value = "InstallPathX86";

    public static string? TryGetInstallPath(GlassPlatformArch arch)
    {
        if (arch == GlassPlatformArch.X64 && !Environment.Is64BitOperatingSystem)
            return null;

        var explicitValue = arch == GlassPlatformArch.X64 ? InstallPathX64Value : InstallPathX86Value;

        var lmView = arch == GlassPlatformArch.X64
            ? Microsoft.Win32.RegistryView.Registry64
            : Microsoft.Win32.RegistryView.Registry32;
        if (TryReadValue(Microsoft.Win32.RegistryHive.LocalMachine, lmView, SubKey, explicitValue) is { } lmExplicit)
            return lmExplicit;
        if (TryReadValue(Microsoft.Win32.RegistryHive.LocalMachine, lmView, SubKey, InstallPathValue) is { } lmLegacy)
            return lmLegacy;

        if (TryReadValue(Microsoft.Win32.RegistryHive.CurrentUser, Microsoft.Win32.RegistryView.Default, SubKey, explicitValue) is { } cuExplicit)
            return cuExplicit;

        if (arch == GlassPlatformArch.X86 && Environment.Is64BitOperatingSystem)
        {
            if (TryReadValue(Microsoft.Win32.RegistryHive.CurrentUser, Microsoft.Win32.RegistryView.Default, Wow6432SubKey, InstallPathValue) is { } cuWow)
                return cuWow;
            return null;
        }

        if (TryReadValue(Microsoft.Win32.RegistryHive.CurrentUser, Microsoft.Win32.RegistryView.Default, SubKey, InstallPathValue) is { } cuLegacy)
        {
            if (arch == GlassPlatformArch.X64 && Environment.Is64BitOperatingSystem
                && !MatchesX64Module(cuLegacy))
                return null;
            return cuLegacy;
        }

        return null;
    }

    private static string? TryReadValue(
        Microsoft.Win32.RegistryHive hive,
        Microsoft.Win32.RegistryView view,
        string subKey,
        string valueName)
    {
        try
        {
            using var baseKey = Microsoft.Win32.RegistryKey.OpenBaseKey(hive, view);
            using var key = baseKey.OpenSubKey(subKey, writable: false);
            var raw = key?.GetValue(valueName) as string;
            return NormalizeDir(raw);
        }
        catch
        {
            return null;
        }
    }

    private static bool MatchesX64Module(string root)
    {
        try
        {
            using var stream = File.OpenRead(Path.Combine(root, "GlassCompat.dll"));
            using var reader = new BinaryReader(stream);
            if (stream.Length < 64 || reader.ReadUInt16() != 0x5a4d) return false;
            stream.Position = 0x3c;
            var peOffset = reader.ReadInt32();
            if (peOffset < 0 || peOffset > stream.Length - 6) return false;
            stream.Position = peOffset;
            return reader.ReadUInt32() == 0x00004550 && reader.ReadUInt16() == 0x8664;
        }
        catch
        {
            return false;
        }
    }

    private static string? NormalizeDir(string? path)
    {
        if (string.IsNullOrWhiteSpace(path))
            return null;

        return path.Trim().TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
    }
}
