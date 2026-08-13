namespace GlassSettings.Services;

/// <summary>Имя темы из активного *.theme — как первый сегмент Themes_TooltipFormat.</summary>
public static class ThemeDisplayNameResolver
{
    public static string FromEntry(VisualStyleThemeEntry entry) =>
        ReadFromThemeFile(entry.ThemeFilePath);

    public static string? TryResolveForCurrentThemeFile()
    {
        if (!VisualStyleThemeManager.TryGetCurrentThemeFilePath(out var path))
            return null;

        if (IsOffGridTheme(path))
            return null;

        return ReadFromThemeFile(path);
    }

    /// <summary>Custom.theme и сохранённые user .theme — не в сетке каталога.</summary>
    public static bool IsOffGridTheme(string themeFilePath) =>
        UserThemeCatalog.IsUserThemePath(themeFilePath);

    public static string ReadFromThemeFile(string themeFilePath)
    {
        var metadata = ThemeFileMetadataReader.Read(themeFilePath);
        var fromTooltip = TryExtractFromTooltip(metadata.TooltipText);
        if (!string.IsNullOrWhiteSpace(fromTooltip))
            return fromTooltip;

        if (!string.IsNullOrWhiteSpace(metadata.DisplayName))
            return metadata.DisplayName;

        return Path.GetFileNameWithoutExtension(themeFilePath);
    }

    public static string? TryExtractFromTooltip(string? tooltipText)
    {
        if (string.IsNullOrWhiteSpace(tooltipText))
            return null;

        var comma = tooltipText.IndexOf(',', StringComparison.Ordinal);
        var name = comma > 0 ? tooltipText[..comma] : tooltipText;
        name = name.Trim();
        return string.IsNullOrWhiteSpace(name) ? null : name;
    }
}
