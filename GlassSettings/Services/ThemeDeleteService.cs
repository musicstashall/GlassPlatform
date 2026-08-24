namespace GlassSettings.Services;

/// <summary>
/// Удаление тем: стоковые aero/aerolite в Resources\Themes — запрет;
/// всё остальное (в т.ч. Custom.theme и кастом в Resources\Themes) — .theme,
/// и папка .msstyles если библиотека больше никому не нужна.
/// </summary>
public static class ThemeDeleteService
{
    public enum ThemeDeleteKind
    {
        ThemeFileOnly,
        FullLibrary
    }

    public sealed record ThemeDeletePlan(
        string ThemeFilePath,
        string DisplayName,
        ThemeDeleteKind Kind,
        string? MsstylesPath);

    public sealed record DeleteResult(bool Success, string? ErrorDetail);

    public static bool CanDelete(VisualStyleThemeEntry entry) =>
        TryCreatePlan(entry) is not null;

    public static ThemeDeletePlan? TryCreatePlan(VisualStyleThemeEntry entry)
    {
        if (entry is null || string.IsNullOrWhiteSpace(entry.ThemeFilePath))
            return null;

        string themePath;
        try
        {
            themePath = Path.GetFullPath(entry.ThemeFilePath);
        }
        catch
        {
            return null;
        }

        if (!File.Exists(themePath))
            return null;

        var displayName = entry.DisplayName ?? string.Empty;
        var visualStylePath = entry.VisualStylePath;
        if (string.IsNullOrWhiteSpace(visualStylePath))
            VisualStyleCatalog.TryReadVisualStylePath(themePath, out visualStylePath);

        // Только стоковые aero/aerolite в Resources\Themes. Custom.theme и кастомные
        // .theme в том же каталоге — не системные (как до регрессии IsSystemThemeFile).
        if (IsStockSystemTheme(themePath, visualStylePath))
            return null;

        var msstyles = TryResolveMsstylesPath(themePath, visualStylePath);
        if (string.IsNullOrWhiteSpace(msstyles)
            || !File.Exists(msstyles)
            || VisualStyleCatalog.IsDefaultVisualStylePath(msstyles)
            || IsProgramFilesPath(msstyles))
        {
            return new ThemeDeletePlan(themePath, displayName, ThemeDeleteKind.ThemeFileOnly, null);
        }

        var libraryDir = Path.GetDirectoryName(msstyles);
        if (string.IsNullOrWhiteSpace(libraryDir) || IsSystemLibraryFolder(libraryDir))
            return new ThemeDeletePlan(themePath, displayName, ThemeDeleteKind.ThemeFileOnly, null);

        if (CountMsstylesReferences(msstyles) != 1)
            return new ThemeDeletePlan(themePath, displayName, ThemeDeleteKind.ThemeFileOnly, null);

        return new ThemeDeletePlan(themePath, displayName, ThemeDeleteKind.FullLibrary, msstyles);
    }

    /// <summary>
    /// FullLibrary трогает .msstyles, который сейчас загружен (текущий .theme или Path текущей темы).
    /// </summary>
    public static bool IsActiveLibrary(ThemeDeletePlan plan)
    {
        if (plan is null || plan.Kind != ThemeDeleteKind.FullLibrary)
            return false;
        if (string.IsNullOrWhiteSpace(plan.MsstylesPath))
            return false;

        if (!VisualStyleThemeManager.TryGetCurrentThemeFilePath(out var currentTheme)
            || string.IsNullOrWhiteSpace(currentTheme))
        {
            return false;
        }

        try
        {
            if (PathsEqual(currentTheme, plan.ThemeFilePath))
                return true;
        }
        catch
        {
            /* ignore */
        }

        var currentMsstyles = TryResolveMsstylesPath(currentTheme, visualStylePath: null);
        return !string.IsNullOrWhiteSpace(currentMsstyles)
            && PathsEqual(currentMsstyles, plan.MsstylesPath);
    }

    public static DeleteResult Execute(ThemeDeletePlan plan)
    {
        if (plan is null || string.IsNullOrWhiteSpace(plan.ThemeFilePath))
            return new DeleteResult(false, "not allowed");

        try
        {
            if (!File.Exists(plan.ThemeFilePath))
                return new DeleteResult(false, "not allowed");

            File.Delete(plan.ThemeFilePath);

            if (plan.Kind == ThemeDeleteKind.FullLibrary
                && !string.IsNullOrWhiteSpace(plan.MsstylesPath))
            {
                TryDeleteLibrary(plan.MsstylesPath);
            }

            return new DeleteResult(true, null);
        }
        catch (Exception ex)
        {
            return new DeleteResult(false, ex.Message);
        }
    }

    private static string? TryResolveMsstylesPath(string themePath, string? visualStylePath)
    {
        var raw = visualStylePath;
        if (string.IsNullOrWhiteSpace(raw))
        {
            if (!VisualStyleCatalog.TryReadVisualStylePath(themePath, out raw)
                || string.IsNullOrWhiteSpace(raw))
            {
                return null;
            }
        }

        try
        {
            var normalized = VisualStyleCatalog.NormalizeResourcePath(raw, themePath);
            return string.IsNullOrWhiteSpace(normalized) ? null : Path.GetFullPath(normalized);
        }
        catch
        {
            return null;
        }
    }

    private static void TryDeleteLibrary(string msstylesPath)
    {
        try
        {
            var full = Path.GetFullPath(msstylesPath);
            var dir = Path.GetDirectoryName(full);
            if (string.IsNullOrWhiteSpace(dir))
                return;

            if (IsProgramFilesPath(full) || IsSystemLibraryFolder(dir))
                return;

            // .msstyles в корне каталога тем — только файл, не весь Themes.
            if (IsThemeCatalogRoot(dir))
            {
                if (File.Exists(full))
                    File.Delete(full);
                return;
            }

            // Соседние .msstyles ещё нужны другим .theme — не трогать pack, только эту библиотеку.
            if (FolderHasOtherReferencedMsstyles(dir, full))
            {
                if (File.Exists(full))
                    File.Delete(full);
                return;
            }

            if (Directory.Exists(dir))
                Directory.Delete(dir, recursive: true);
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"GlassSettings ThemeDeleteService library: {ex}");
        }
    }

    private static bool FolderHasOtherReferencedMsstyles(string directory, string msstylesPath)
    {
        try
        {
            var target = Path.GetFullPath(msstylesPath);
            foreach (var file in Directory.EnumerateFiles(directory, "*.msstyles", SearchOption.AllDirectories))
            {
                if (string.Equals(Path.GetFullPath(file), target, StringComparison.OrdinalIgnoreCase))
                    continue;
                if (CountMsstylesReferences(file) > 0)
                    return true;
            }

            return false;
        }
        catch
        {
            return true;
        }
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

    /// <summary>
    /// Стоковая тема Windows: файл в Resources\Themes и [VisualStyles] Path = aero/aerolite.
    /// </summary>
    private static bool IsStockSystemTheme(string themeFilePath, string? visualStylePath)
    {
        var system = VisualStyleCatalog.SystemThemesDirectory;
        if (string.IsNullOrWhiteSpace(system) || !IsUnderDirectory(themeFilePath, system))
            return false;

        return VisualStyleCatalog.IsDefaultVisualStylePath(visualStylePath ?? string.Empty);
    }

    private static bool IsThemeCatalogRoot(string directory)
    {
        var system = VisualStyleCatalog.SystemThemesDirectory;
        if (!string.IsNullOrWhiteSpace(system) && PathsEqual(directory, system))
            return true;

        var user = UserThemeCatalog.UserThemesDirectory;
        return !string.IsNullOrWhiteSpace(user) && PathsEqual(directory, user);
    }

    private static bool IsSystemLibraryFolder(string directory)
    {
        var system = VisualStyleCatalog.SystemThemesDirectory;
        if (string.IsNullOrWhiteSpace(system))
            return false;

        return PathsEqual(directory, Path.Combine(system, "Aero"))
            || PathsEqual(directory, Path.Combine(system, "AeroLite"));
    }

    private static bool IsProgramFilesPath(string path)
    {
        try
        {
            var full = Path.GetFullPath(path);
            var programFiles = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles);
            var programFilesX86 = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86);

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

    private static bool PathsEqual(string left, string right)
    {
        try
        {
            return string.Equals(Path.GetFullPath(left), Path.GetFullPath(right), StringComparison.OrdinalIgnoreCase);
        }
        catch
        {
            return false;
        }
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
