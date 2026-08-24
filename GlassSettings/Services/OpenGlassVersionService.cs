using GlassSettings.Core.Platform;

namespace GlassSettings.Services;

/// <summary>
/// Бизнес-логика версий OpenGlass. Пути — через <see cref="PlatformPaths"/>.
/// </summary>
public static class OpenGlassVersionService
{
    public static bool TryGetInstalledVersion(out string? version, out string? dllPath)
    {
        version = null;
        dllPath = PlatformPaths.OpenGlassDllPath;
        if (dllPath is null || !File.Exists(dllPath))
            return false;

        version = PlatformModuleVersions.GetVersionFromPath(dllPath);
        return !string.IsNullOrWhiteSpace(version)
               && !string.Equals(version, GlassSettings.Strings.GlassSettingsResources.Get("Modules_NotFound"), StringComparison.Ordinal)
               && !string.Equals(version, GlassSettings.Strings.GlassSettingsResources.Get("Modules_Unavailable"), StringComparison.Ordinal);
    }

    /// <summary>Корень установки: родитель каталога OpenGlass.dll (или InstallPath из реестра).</summary>
    public static string? TryGetInstallRoot() => PlatformPaths.OpenGlassInstallRoot;

    public static string? ResolveOpenGlassHostExePath() => PlatformPaths.OpenGlassHostPath;

    public static string? ResolveOpenGlassDllPath() => PlatformPaths.OpenGlassDllPath;

    /// <summary>Exe OpenGlassGUI / OpenGlass для запуска из UI.</summary>
    public static string? TryResolveOpenGlassGuiExePath() => PlatformPaths.OpenGlassGuiPath;
}
