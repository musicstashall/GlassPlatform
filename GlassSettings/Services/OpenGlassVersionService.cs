using GlassSettings.Core.Platform;
using GlassSettings.Core.Registry;
using Microsoft.Win32;

namespace GlassSettings.Services;

public static class OpenGlassVersionService
{
    private const string BwmRegKey = @"Software\StanislavKa\BlackWindowManager";
    private const string InstallPathValue = "InstallPath";
    private const string HardcodedOpenGlassDll =
        @"C:\Program Files (x86)\BlackWindowManager\openglass\OpenGlass.dll";

    public static bool TryGetInstalledVersion(out string? version, out string? dllPath)
    {
        version = null;
        dllPath = ResolveOpenGlassDllPath();
        if (dllPath is null || !File.Exists(dllPath))
            return false;

        version = PlatformModuleVersions.GetVersionFromPath(dllPath);
        return !string.IsNullOrWhiteSpace(version)
               && !string.Equals(version, GlassSettings.Strings.GlassSettingsResources.Get("Modules_NotFound"), StringComparison.Ordinal)
               && !string.Equals(version, GlassSettings.Strings.GlassSettingsResources.Get("Modules_Unavailable"), StringComparison.Ordinal);
    }

    /// <summary>Корень установки: родитель каталога OpenGlass.dll (или InstallPath из реестра).</summary>
    public static string? TryGetInstallRoot()
    {
        var dll = ResolveOpenGlassDllPath();
        if (!string.IsNullOrWhiteSpace(dll))
        {
            var oglassDir = Path.GetDirectoryName(dll);
            if (!string.IsNullOrWhiteSpace(oglassDir))
            {
                var leaf = Path.GetFileName(oglassDir);
                if (leaf.Equals("OpenGlass", StringComparison.OrdinalIgnoreCase)
                    || leaf.Equals("openglass", StringComparison.OrdinalIgnoreCase))
                {
                    var parent = Path.GetDirectoryName(oglassDir);
                    if (!string.IsNullOrWhiteSpace(parent))
                        return NormalizeDir(parent);
                }

                return NormalizeDir(oglassDir);
            }
        }

        var installDir = ReadBlackWindowManagerInstallPath();
        return string.IsNullOrWhiteSpace(installDir) ? null : NormalizeDir(installDir);
    }

    public static string? ResolveOpenGlassHostExePath()
    {
        var dll = ResolveOpenGlassDllPath();
        if (string.IsNullOrEmpty(dll))
            return null;

        var dir = Path.GetDirectoryName(dll);
        if (string.IsNullOrEmpty(dir))
            return null;

        string[] candidates =
        [
            Path.Combine(dir, "OpenGlassHost.exe"),
            Path.Combine(Path.GetDirectoryName(dir) ?? dir, "OpenGlassHost.exe"),
        ];

        foreach (var c in candidates)
        {
            if (File.Exists(c))
                return c;
        }

        return null;
    }

    public static string? ResolveOpenGlassDllPath()
    {
        var installDir = ReadBlackWindowManagerInstallPath();
        if (!string.IsNullOrEmpty(installDir))
        {
            var fromInstall = PathOpenGlassDllFromInstallDir(installDir);
            if (!string.IsNullOrEmpty(fromInstall))
                return fromInstall;
        }

        var fromReg = PathOpenGlassDllFromBwmInstall(installDir);
        if (File.Exists(fromReg))
            return fromReg;

        foreach (var candidate in OpenGlassPathPolicy.EnumerateBundledOpenGlassDllCandidates())
        {
            if (File.Exists(candidate))
                return Path.GetFullPath(candidate);
        }

        if (File.Exists(HardcodedOpenGlassDll))
            return HardcodedOpenGlassDll;

        return null;
    }

    /// <summary>Exe OpenGlassGUI / OpenGlass для запуска из UI.</summary>
    public static string? TryResolveOpenGlassGuiExePath()
    {
        foreach (var root in EnumerateOpenGlassInstallSearchRoots())
        {
            foreach (var name in new[] { "OpenGlassGUI.exe", "OpenGlass.exe" })
            {
                var candidate = Path.Combine(root, name);
                if (File.Exists(candidate))
                    return Path.GetFullPath(candidate);
            }
        }

        return null;
    }

    private static IEnumerable<string> EnumerateOpenGlassInstallSearchRoots()
    {
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        string? TryAdd(string? path)
        {
            if (string.IsNullOrWhiteSpace(path))
                return null;

            var full = Path.GetFullPath(path);
            return seen.Add(full) ? full : null;
        }

        var installRoot = TryGetInstallRoot();
        if (TryAdd(installRoot) is { } fromInstall)
            yield return fromInstall;

        var dll = ResolveOpenGlassDllPath();
        if (!string.IsNullOrWhiteSpace(dll))
        {
            var dir = Path.GetDirectoryName(dll);
            if (TryAdd(dir) is { } fromDll)
                yield return fromDll;

            if (!string.IsNullOrWhiteSpace(dir))
            {
                var parent = Path.GetDirectoryName(dir);
                if (TryAdd(parent) is { } fromParent)
                    yield return fromParent;
            }
        }

        if (TryAdd(AppContext.BaseDirectory) is { } fromExe)
            yield return fromExe;
    }

    private static string ReadBlackWindowManagerInstallPath()
    {
        var hkcu = ReadInstallPath(Registry.CurrentUser);
        if (!string.IsNullOrEmpty(hkcu))
            return hkcu;

        return ReadInstallPath(Registry.LocalMachine) ?? string.Empty;
    }

    private static string? ReadInstallPath(RegistryKey root)
    {
        using var key = root.OpenSubKey(BwmRegKey, false);
        return key?.GetValue(InstallPathValue) as string;
    }

    private static string PathOpenGlassDllFromBwmInstall(string installDir)
    {
        if (string.IsNullOrEmpty(installDir))
            return string.Empty;

        return Path.Combine(NormalizeDir(installDir), "OpenGlass", "OpenGlass.dll");
    }

    private static string? PathOpenGlassDllFromInstallDir(string installDir)
    {
        var baseDir = NormalizeDir(installDir);
        string[] candidates =
        [
            Path.Combine(baseDir, "openglass", "OpenGlass.dll"),
            Path.Combine(baseDir, "OpenGlass", "OpenGlass.dll"),
            Path.Combine(baseDir, "OpenGlass.dll")
        ];

        foreach (var candidate in candidates)
        {
            if (File.Exists(candidate))
                return candidate;
        }

        return null;
    }

    private static string NormalizeDir(string path)
    {
        path = path.TrimEnd('\\', '/');
        return path;
    }
}
