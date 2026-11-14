using GlassSettings.Core.Platform;
using Microsoft.Win32;

namespace GlassSettings.Services;

/// <summary>
/// Установленные продукты Glass, которые умеет обновлять каталог version.json.
/// Платформа — ProductVersion GlassCore.dll, Settings — FileVersion GlassSettings.exe,
/// Resources — DisplayVersion фиксированного ProductCode x64 MSI.
/// </summary>
internal static class GlassInstalledProducts
{
    private const string ResourcesProductCode = "{6D6046DB-6B28-4F2C-8A7D-EB6CF13CBBD9}";

    public static bool TryReadVersion(string id, out string version)
    {
        version = string.Empty;
        return id switch
        {
            "platform64" => TryReadPlatform(GlassPlatformArch.X64, out version),
            "platform32" => TryReadPlatform(GlassPlatformArch.X86, out version),
            "settings64" => TryReadSettings(GlassPlatformArch.X64, out version),
            "settings32" => TryReadSettings(GlassPlatformArch.X86, out version),
            "openglassResources" => TryReadResources(out version),
            _ => false
        };
    }

    public static bool IsStillOlder(GlassProductUpdate offer) =>
        TryReadVersion(offer.Id, out var installed)
        && GlassReleaseUpdateService.IsInstalledOlderThan(installed, offer.Version);

    private static bool TryReadPlatform(GlassPlatformArch arch, out string version)
    {
        version = string.Empty;
        if (arch == GlassPlatformArch.X64 && !Environment.Is64BitOperatingSystem)
            return false;

        var path = GlassModulePath.Resolve("GlassCore.dll", arch);
        return path is not null && PlatformModuleVersions.TryReadProductVersion(path, out version);
    }

    private static bool TryReadSettings(GlassPlatformArch arch, out string version)
    {
        version = string.Empty;
        if (arch == GlassPlatformArch.X64 && !Environment.Is64BitOperatingSystem)
            return false;

        var root = GlassModulePath.GetInstallRootForArch(arch);
        if (string.IsNullOrEmpty(root))
            return false;

        var nested = Path.Combine(root, "GlassSettings", "GlassSettings.exe");
        var flat = Path.Combine(root, "GlassSettings.exe");
        var path = File.Exists(nested) ? nested : File.Exists(flat) ? flat : null;
        return path is not null && PlatformModuleVersions.TryReadFileVersion(path, out version);
    }

    private static bool TryReadResources(out string version)
    {
        version = string.Empty;
        if (!Environment.Is64BitOperatingSystem)
            return false;

        try
        {
            using var hive = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64);
            using var key = hive.OpenSubKey(
                @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\" + ResourcesProductCode);
            var display = key?.GetValue("DisplayVersion") as string;
            if (string.IsNullOrWhiteSpace(display))
                return false;
            version = display.Trim();
            return true;
        }
        catch
        {
            return false;
        }
    }
}
