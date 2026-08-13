namespace GlassSettings.Services;

/// <summary>Построение цепочки BreadcrumbBar из полного пути.</summary>
internal static class ExecutableFilePickerPathHelper
{
    public static IReadOnlyList<ExecutableFilePickerBreadcrumbItem> BuildTrail(string path)
    {
        var items = new List<ExecutableFilePickerBreadcrumbItem>();
        if (string.IsNullOrWhiteSpace(path))
            return items;

        var root = Path.GetPathRoot(path);
        if (string.IsNullOrEmpty(root))
            return items;

        items.Add(new ExecutableFilePickerBreadcrumbItem(root.TrimEnd('\\'), root));

        var remainder = path.Substring(root.Length).TrimStart(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        if (string.IsNullOrEmpty(remainder))
            return items;

        var cumulative = root.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        foreach (var segment in remainder.Split(new[] { Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar },
                     StringSplitOptions.RemoveEmptyEntries))
        {
            cumulative = string.IsNullOrEmpty(cumulative)
                ? segment
                : cumulative + Path.DirectorySeparatorChar + segment;
            items.Add(new ExecutableFilePickerBreadcrumbItem(segment, cumulative));
        }

        return items;
    }

    /// <summary>Разбор ввода адресной строки: %ENV%, кавычки, нормализация пути.</summary>
    public static bool TryResolveDirectoryInput(string input, out string resolvedPath)
    {
        resolvedPath = string.Empty;
        if (string.IsNullOrWhiteSpace(input))
            return false;

        try
        {
            var trimmed = input.Trim().Trim('"');
            if (string.IsNullOrWhiteSpace(trimmed))
                return false;

            var expanded = Environment.ExpandEnvironmentVariables(trimmed);
            if (string.IsNullOrWhiteSpace(expanded))
                return false;

            var candidate = expanded.Trim();
            var root = Path.GetPathRoot(candidate);
            if (!string.IsNullOrEmpty(root))
            {
                var relative = candidate.Substring(root.Length)
                    .TrimStart(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
                if (string.IsNullOrEmpty(relative))
                {
                    resolvedPath = ExecutableFilePickerCatalog.NormalizeBrowsablePath(candidate);
                    return ExecutableFilePickerCatalog.IsValidDirectory(resolvedPath);
                }
            }

            resolvedPath = ExecutableFilePickerCatalog.NormalizeBrowsablePath(Path.GetFullPath(candidate));
            return ExecutableFilePickerCatalog.IsValidDirectory(resolvedPath);
        }
        catch
        {
            return false;
        }
    }
}
