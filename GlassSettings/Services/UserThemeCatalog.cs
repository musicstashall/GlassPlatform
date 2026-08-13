namespace GlassSettings.Services;

/// <summary>Персонализированные и сохранённые темы в %LOCALAPPDATA%\...\Themes.</summary>
public static class UserThemeCatalog
{
    private const string PersonalizedThemeFileName = "Custom.theme";

    public static string? UserThemesDirectory => VisualStyleCatalog.UserThemesDirectory;

    public static string? TryGetPersonalizedThemePath()
    {
        var userDir = UserThemesDirectory;
        if (string.IsNullOrWhiteSpace(userDir))
            return null;

        var path = Path.Combine(userDir, PersonalizedThemeFileName);
        return File.Exists(path) ? path : null;
    }

    public static bool HasPersonalizedTheme() => TryGetPersonalizedThemePath() is not null;

    public static VisualStyleThemeEntry? TryGetPersonalizedThemeEntry()
    {
        var path = TryGetPersonalizedThemePath();
        return path is null ? null : VisualStyleCatalog.TryGetEntryForThemeFile(path);
    }

    public static bool IsUserThemePath(string themeFilePath)
    {
        if (string.IsNullOrWhiteSpace(themeFilePath))
            return false;

        var userDir = UserThemesDirectory;
        if (string.IsNullOrWhiteSpace(userDir))
            return false;

        try
        {
            var fullPath = Path.GetFullPath(themeFilePath);
            var userRoot = Path.GetFullPath(userDir);
            return fullPath.StartsWith(userRoot, StringComparison.OrdinalIgnoreCase);
        }
        catch
        {
            return false;
        }
    }

    public static IReadOnlyList<VisualStyleThemeEntry> GetSavedThemes()
    {
        var userDir = UserThemesDirectory;
        if (string.IsNullOrWhiteSpace(userDir) || !Directory.Exists(userDir))
            return Array.Empty<VisualStyleThemeEntry>();

        try
        {
            return Directory
                .EnumerateFiles(userDir, "*.theme", SearchOption.TopDirectoryOnly)
                .Where(path => !IsPersonalizedThemeFile(path))
                .Select(path => (Path: path, Entry: VisualStyleCatalog.TryGetEntryForThemeFile(path)))
                .Where(item => item.Entry is not null)
                .OrderByDescending(item =>
                {
                    try
                    {
                        return File.GetLastWriteTimeUtc(item.Path);
                    }
                    catch
                    {
                        return DateTime.MinValue;
                    }
                })
                .ThenBy(item => item.Entry!.DisplayName, StringComparer.CurrentCultureIgnoreCase)
                .Select(item => item.Entry!)
                .ToList();
        }
        catch
        {
            return Array.Empty<VisualStyleThemeEntry>();
        }
    }

    /// <summary>Именованные user-темы (новые первыми), затем системные.</summary>
    public static IReadOnlyList<VisualStyleThemeEntry> GetThemesForGrid()
    {
        var saved = GetSavedThemes();
        var system = VisualStyleCatalog.GetInstalledThemes();
        if (saved.Count == 0)
            return system;

        var result = new List<VisualStyleThemeEntry>(saved.Count + system.Count);
        result.AddRange(saved);
        var savedPaths = new HashSet<string>(
            saved.Select(t => Path.GetFullPath(t.ThemeFilePath)),
            StringComparer.OrdinalIgnoreCase);
        foreach (var theme in system)
        {
            if (!savedPaths.Contains(Path.GetFullPath(theme.ThemeFilePath)))
                result.Add(theme);
        }

        return result;
    }

    public static bool IsSavedThemePath(string themeFilePath)
    {
        if (!IsUserThemePath(themeFilePath))
            return false;

        return !IsPersonalizedThemeFile(themeFilePath);
    }

    public static bool HasSavedThemes() => GetSavedThemes().Count > 0;

    public static bool IsActivePersonalizedTheme()
    {
        if (!VisualStyleThemeManager.TryGetCurrentThemeFilePath(out var path))
            return false;

        return IsPersonalizedThemeFile(path);
    }

    public static bool ShouldShowPersonalizedThemeLink()
    {
        if (IsActivePersonalizedTheme())
            return false;

        return HasPersonalizedTheme();
    }

    internal static bool IsPersonalizedThemeFile(string themeFilePath) =>
        string.Equals(Path.GetFileName(themeFilePath), PersonalizedThemeFileName, StringComparison.OrdinalIgnoreCase);
}
