namespace GlassSettings.Services;

/// <summary>
/// Безопасное удаление тем: системные — запрет; user .theme — файл;
/// полный pack только если все ресурсы вне SystemRoot/ProgramFiles, в пакете темы и msstyles refcount==1.
/// </summary>
public static class ThemeDeleteService
{
    public sealed record DeleteResult(bool Success, string? ErrorDetail);

    public static bool CanDelete(VisualStyleThemeEntry entry)
    {
        if (entry is null || string.IsNullOrWhiteSpace(entry.ThemeFilePath))
            return false;

        string fullPath;
        try
        {
            fullPath = Path.GetFullPath(entry.ThemeFilePath);
        }
        catch
        {
            return false;
        }

        if (!File.Exists(fullPath))
            return false;

        // Системные (%SystemRoot%\Resources\Themes) и Custom.theme — нельзя.
        if (IsUnderSystemThemes(fullPath))
            return false;

        if (UserThemeCatalog.IsPersonalizedThemeFile(fullPath))
            return false;

        // Любая именованная тема в user Themes (в т.ч. юникодное имя файла).
        return UserThemeCatalog.IsUserThemePath(fullPath);
    }

    public static DeleteResult Delete(VisualStyleThemeEntry entry)
    {
        if (!CanDelete(entry))
            return new DeleteResult(false, "not allowed");

        try
        {
            var themePath = Path.GetFullPath(entry.ThemeFilePath);
            var packageRoot = Path.GetDirectoryName(themePath);
            if (string.IsNullOrWhiteSpace(packageRoot))
                return new DeleteResult(false, "no directory");

            var resourcePaths = CollectOwnedResourcePaths(themePath, packageRoot);
            File.Delete(themePath);

            if (resourcePaths.Count > 0)
            {
                foreach (var path in resourcePaths)
                {
                    try
                    {
                        if (File.Exists(path))
                            File.Delete(path);
                    }
                    catch (Exception ex)
                    {
                        System.Diagnostics.Debug.WriteLine($"GlassSettings ThemeDeleteService resource: {ex}");
                    }
                }
            }

            return new DeleteResult(true, null);
        }
        catch (Exception ex)
        {
            return new DeleteResult(false, ex.Message);
        }
    }

    /// <summary>
    /// Ресурсы для полного удаления: только вне защищённых корней, внутри packageRoot,
    /// и для .msstyles — единственная ссылка среди system+user Themes.
    /// </summary>
    private static List<string> CollectOwnedResourcePaths(string themePath, string packageRoot)
    {
        var result = new List<string>();
        if (!VisualStyleCatalog.TryReadVisualStylePath(themePath, out var msstylesRaw)
            || string.IsNullOrWhiteSpace(msstylesRaw))
        {
            return result;
        }

        var msstyles = VisualStyleCatalog.NormalizeResourcePath(msstylesRaw, themePath);
        if (string.IsNullOrWhiteSpace(msstyles) || !File.Exists(msstyles))
            return result;

        if (IsProtectedPath(msstyles))
            return result;

        if (!IsUnderDirectory(msstyles, packageRoot))
            return result;

        if (CountMsstylesReferences(msstyles) != 1)
            return result;

        result.Add(Path.GetFullPath(msstyles));

        foreach (var resource in EnumerateThemeResourcePaths(themePath))
        {
            if (IsProtectedPath(resource))
                continue;

            if (!IsUnderDirectory(resource, packageRoot))
                continue;

            if (!File.Exists(resource))
                continue;

            var full = Path.GetFullPath(resource);
            if (!result.Contains(full, StringComparer.OrdinalIgnoreCase))
                result.Add(full);
        }

        return result;
    }

    private static int CountMsstylesReferences(string msstylesPath)
    {
        var target = Path.GetFullPath(msstylesPath);
        var count = 0;
        foreach (var themeFile in EnumerateAllThemeFiles())
        {
            if (!VisualStyleCatalog.TryReadVisualStylePath(themeFile, out var path)
                || string.IsNullOrWhiteSpace(path))
            {
                continue;
            }

            var normalized = VisualStyleCatalog.NormalizeResourcePath(path, themeFile);
            if (string.IsNullOrWhiteSpace(normalized))
                continue;

            try
            {
                if (string.Equals(Path.GetFullPath(normalized), target, StringComparison.OrdinalIgnoreCase))
                    count++;
            }
            catch
            {
                /* ignore */
            }
        }

        return count;
    }

    private static IEnumerable<string> EnumerateAllThemeFiles()
    {
        foreach (var root in EnumerateThemeRoots())
        {
            if (!Directory.Exists(root))
                continue;

            IEnumerable<string> files;
            try
            {
                files = Directory.EnumerateFiles(root, "*.theme", SearchOption.TopDirectoryOnly);
            }
            catch
            {
                continue;
            }

            foreach (var file in files)
                yield return file;
        }
    }

    private static IEnumerable<string> EnumerateThemeRoots()
    {
        var system = VisualStyleCatalog.SystemThemesDirectory;
        if (!string.IsNullOrWhiteSpace(system))
            yield return system;

        var user = UserThemeCatalog.UserThemesDirectory;
        if (!string.IsNullOrWhiteSpace(user))
            yield return user;
    }

    private static IEnumerable<string> EnumerateThemeResourcePaths(string themeFilePath)
    {
        string? currentSection = null;
        foreach (var raw in ThemeFileLineReader.ReadLines(themeFilePath))
        {
            var line = raw.Trim();
            if (line.Length == 0 || line[0] == ';')
                continue;

            if (line.StartsWith('[') && line.EndsWith(']'))
            {
                currentSection = line[1..^1].Trim();
                continue;
            }

            if (currentSection is null)
                continue;

            // Секции с файловыми путями.
            if (!IsResourceSection(currentSection))
                continue;

            var separator = line.IndexOf('=');
            if (separator <= 0)
                continue;

            var value = line[(separator + 1)..].Trim().Trim('"');
            if (string.IsNullOrWhiteSpace(value))
                continue;

            if (value.StartsWith("@", StringComparison.Ordinal))
                continue;

            var path = VisualStyleCatalog.NormalizeResourcePath(value, themeFilePath);
            if (!string.IsNullOrWhiteSpace(path))
                yield return path;
        }
    }

    private static bool IsResourceSection(string section) =>
        section.Equals("Control Panel\\Desktop", StringComparison.OrdinalIgnoreCase)
        || section.Equals("Slideshow", StringComparison.OrdinalIgnoreCase)
        || section.Equals("Sounds", StringComparison.OrdinalIgnoreCase)
        || section.Equals("Cursors", StringComparison.OrdinalIgnoreCase)
        || section.Equals("Boot", StringComparison.OrdinalIgnoreCase)
        || section.Equals("Theme", StringComparison.OrdinalIgnoreCase);

    private static bool IsUnderSystemThemes(string themeFilePath)
    {
        var system = VisualStyleCatalog.SystemThemesDirectory;
        return !string.IsNullOrWhiteSpace(system) && IsUnderDirectory(themeFilePath, system);
    }

    private static bool IsUnderUserThemes(string themeFilePath)
    {
        var user = UserThemeCatalog.UserThemesDirectory;
        return !string.IsNullOrWhiteSpace(user) && IsUnderDirectory(themeFilePath, user);
    }

    private static bool IsProtectedPath(string path)
    {
        try
        {
            var full = Path.GetFullPath(path);
            var systemRoot = Environment.GetFolderPath(Environment.SpecialFolder.Windows);
            var programFiles = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles);
            var programFilesX86 = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86);

            if (!string.IsNullOrWhiteSpace(systemRoot) && IsUnderDirectory(full, systemRoot))
                return true;
            if (!string.IsNullOrWhiteSpace(programFiles) && IsUnderDirectory(full, programFiles))
                return true;
            if (!string.IsNullOrWhiteSpace(programFilesX86) && IsUnderDirectory(full, programFilesX86))
                return true;
        }
        catch
        {
            return true;
        }

        return false;
    }

    private static bool IsUnderDirectory(string path, string root)
    {
        try
        {
            var full = Path.GetFullPath(path);
            var rootFull = Path.GetFullPath(root);
            if (!rootFull.EndsWith(Path.DirectorySeparatorChar)
                && !rootFull.EndsWith(Path.AltDirectorySeparatorChar))
            {
                rootFull += Path.DirectorySeparatorChar;
            }

            return full.StartsWith(rootFull, StringComparison.OrdinalIgnoreCase)
                   || string.Equals(full, Path.GetFullPath(root), StringComparison.OrdinalIgnoreCase);
        }
        catch
        {
            return false;
        }
    }
}
