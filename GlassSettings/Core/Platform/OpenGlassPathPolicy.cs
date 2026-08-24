namespace GlassSettings.Core.Platform;

/// <summary>Пути bundled OpenGlass рядом с GlassSettings.exe (OrdinalIgnoreCase).</summary>
public static class OpenGlassPathPolicy
{
    private static string? _bundledRootFullPath;

    public static string BundledRootFullPath =>
        _bundledRootFullPath ??= Path.GetFullPath(AppContext.BaseDirectory);

    public static IEnumerable<string> EnumerateBundledOpenGlassDllCandidates()
    {
        var root = BundledRootFullPath;
        yield return Path.Combine(root, "OpenGlass.dll");
        yield return Path.Combine(root, "OpenGlass", "OpenGlass.dll");

        var parent = Path.GetDirectoryName(root.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar));
        if (!string.IsNullOrWhiteSpace(parent))
        {
            yield return Path.Combine(parent, "OpenGlass.dll");
            yield return Path.Combine(parent, "OpenGlass", "OpenGlass.dll");
        }
    }

    public static string? TryResolveBundledDllPath()
    {
        foreach (var candidate in EnumerateBundledOpenGlassDllCandidates())
        {
            if (File.Exists(candidate))
                return Path.GetFullPath(candidate);
        }

        return null;
    }

    public static bool IsBundledWithApplication() => TryResolveBundledDllPath() is not null;

    public static bool IsPathBundled(string? fullPath)
    {
        if (string.IsNullOrWhiteSpace(fullPath) || !File.Exists(fullPath))
            return false;

        var normalized = Path.GetFullPath(fullPath);
        foreach (var candidate in EnumerateBundledOpenGlassDllCandidates())
        {
            if (string.Equals(normalized, Path.GetFullPath(candidate), StringComparison.OrdinalIgnoreCase))
                return true;
        }

        var host = Path.Combine(Path.GetDirectoryName(normalized) ?? string.Empty, "OpenGlassHost.exe");
        if (File.Exists(host)
            && IsPathUnderBundledRoot(Path.GetFullPath(host)))
        {
            return true;
        }

        return IsPathUnderBundledRoot(normalized);
    }

    public static bool IsPathUnderBundledRoot(string? fullPath)
    {
        if (string.IsNullOrWhiteSpace(fullPath))
            return false;

        var normalized = Path.GetFullPath(fullPath);
        var root = BundledRootFullPath;
        if (!normalized.StartsWith(root, StringComparison.OrdinalIgnoreCase))
            return false;

        if (normalized.Length == root.Length)
            return true;

        var next = normalized[root.Length];
        return next is '\\' or '/';
    }
}
