namespace GlassSettings.Services;

/// <summary>Нормализация пути .theme (полный путь, имя файла → system/user root).</summary>
internal static class ThemePathNormalizer
{
    public static string Normalize(string themeFilePath)
    {
        if (string.IsNullOrWhiteSpace(themeFilePath))
            return string.Empty;

        var trimmed = themeFilePath.Trim().Trim('"');
        if (Path.IsPathRooted(trimmed))
            return Path.GetFullPath(trimmed);

        var fileName = Path.GetFileName(trimmed);
        if (string.IsNullOrWhiteSpace(fileName))
            return trimmed;

        if (UserThemeCatalog.UserThemesDirectory is { } userDir)
        {
            var userCandidate = Path.Combine(userDir, fileName);
            if (File.Exists(userCandidate))
                return Path.GetFullPath(userCandidate);
        }

        var systemCandidate = Path.Combine(VisualStyleCatalog.SystemThemesDirectory, fileName);
        if (File.Exists(systemCandidate))
            return Path.GetFullPath(systemCandidate);

        return trimmed;
    }
}
