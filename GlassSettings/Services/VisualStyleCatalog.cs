using GlassSettings.Strings;

namespace GlassSettings.Services;

/// <summary>
/// Каталог установленных визуальных стилей Windows (.theme → [VisualStyles] Path).
/// Используется на Обзоре и странице «Визуальные стили».
/// </summary>
public static class VisualStyleCatalog
{
    private const string VisualStylesSection = "VisualStyles";
    private const string VisualStylesPathKey = "Path";
    private const string VisualStylesColorStyleKey = "ColorStyle";
    private const string VisualStylesSizeKey = "Size";
    private const string DefaultColorStyle = "NormalColor";
    private const string DefaultSize = "NormalSize";
    private const string DefaultVisualStyleFileName = "aero.msstyles";
    private const string LiteVisualStyleFileName = "aerolite.msstyles";

    public static string ResourcesDirectory =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "Resources");

    public static string SystemThemesDirectory =>
        Path.Combine(ResourcesDirectory, "Themes");

    public static string? UserThemesDirectory
    {
        get
        {
            var localAppData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            return string.IsNullOrWhiteSpace(localAppData)
                ? null
                : Path.Combine(localAppData, "Microsoft", "Windows", "Themes");
        }
    }

    public static IReadOnlyList<VisualStyleThemeEntry> GetInstalledThemes() =>
        EnumerateThemeFiles()
            .Select(TryParseThemeFile)
            .Where(entry => entry is not null)
            .Cast<VisualStyleThemeEntry>()
            .OrderBy(entry => entry.DisplayName, StringComparer.CurrentCultureIgnoreCase)
            .ToList();

    public static VisualStyleThemeEntry? TryGetEntryForThemeFile(string themeFilePath) =>
        TryParseThemeFile(themeFilePath);

    /// <summary>Метаданные активной .theme, если полный разбор ([VisualStyles] Path) недоступен.</summary>
    public static VisualStyleThemeEntry? TryGetFallbackEntryForThemeFile(string themeFilePath)
    {
        if (string.IsNullOrWhiteSpace(themeFilePath) || !File.Exists(themeFilePath))
            return null;

        if (!TryReadVisualStylesInfo(themeFilePath, out var visualStylePath, out var colorStyle, out var size))
        {
            visualStylePath = string.Empty;
            colorStyle = DefaultColorStyle;
            size = DefaultSize;
        }

        var metadata = ThemeFileMetadataReader.Read(themeFilePath);
        var accentColor = ThemeAccentColorHelper.TryParseColorizationColor(themeFilePath);
        var isDarkMode = ThemeAppModeHelper.ReadIsDarkMode(themeFilePath);
        return new VisualStyleThemeEntry(
            metadata.DisplayName,
            themeFilePath,
            visualStylePath,
            colorStyle,
            size,
            IsCustomVisualStylePath(visualStylePath),
            metadata.PreviewImagePath,
            metadata.ImageCount,
            metadata.TooltipText,
            accentColor,
            isDarkMode);
    }

    public static IReadOnlyList<VisualStyleThemeEntry> GetCustomThemes() =>
        GetInstalledThemes().Where(entry => entry.IsCustom).ToList();

    public static int CountCustomThemes() => GetCustomThemes().Count;

    public static string HomeLine()
    {
        var custom = CountCustomThemes();
        return custom == 0 ? GlassSettingsResources.Get("Themes_Default") : StyleCountLabel(custom);
    }

    public static bool IsCustomVisualStylePath(string visualStylePath) =>
        !IsDefaultVisualStylePath(visualStylePath);

    public static bool IsDefaultVisualStylePath(string visualStylePath)
    {
        if (string.IsNullOrWhiteSpace(visualStylePath))
            return true;

        var fileName = Path.GetFileName(visualStylePath.Trim().Trim('"'));
        return string.Equals(fileName, DefaultVisualStyleFileName, StringComparison.OrdinalIgnoreCase)
            || string.Equals(fileName, LiteVisualStyleFileName, StringComparison.OrdinalIgnoreCase);
    }

    public static bool TryReadVisualStylePath(string themeFilePath, out string visualStylePath) =>
        TryReadVisualStylesInfo(themeFilePath, out visualStylePath, out _, out _);

    /// <summary>
    /// [VisualStyles] Path / ColorStyle / Size. ColorStyle/Size по умолчанию NormalColor / NormalSize.
    /// </summary>
    public static bool TryReadVisualStylesInfo(
        string themeFilePath,
        out string visualStylePath,
        out string colorStyle,
        out string size)
    {
        visualStylePath = string.Empty;
        colorStyle = DefaultColorStyle;
        size = DefaultSize;
        if (string.IsNullOrWhiteSpace(themeFilePath) || !File.Exists(themeFilePath))
            return false;

        try
        {
            var inVisualStyles = false;
            string? pathValue = null;
            foreach (var rawLine in ThemeFileLineReader.ReadLines(themeFilePath))
            {
                var line = rawLine.Trim();
                if (line.Length == 0 || line[0] == ';')
                    continue;

                if (line.StartsWith('[') && line.EndsWith(']'))
                {
                    var section = line[1..^1].Trim();
                    // Вышли из секции — Path уже должен быть прочитан.
                    if (inVisualStyles && !string.Equals(section, VisualStylesSection, StringComparison.OrdinalIgnoreCase))
                        break;
                    inVisualStyles = string.Equals(section, VisualStylesSection, StringComparison.OrdinalIgnoreCase);
                    continue;
                }

                if (!inVisualStyles)
                    continue;

                var separator = line.IndexOf('=');
                if (separator <= 0)
                    continue;

                var key = line[..separator].Trim();
                var value = line[(separator + 1)..].Trim().Trim('"');
                if (string.IsNullOrWhiteSpace(value))
                    continue;

                if (string.Equals(key, VisualStylesPathKey, StringComparison.OrdinalIgnoreCase))
                    pathValue = value;
                else if (string.Equals(key, VisualStylesColorStyleKey, StringComparison.OrdinalIgnoreCase))
                    colorStyle = value;
                else if (string.Equals(key, VisualStylesSizeKey, StringComparison.OrdinalIgnoreCase))
                    size = value;
            }

            if (string.IsNullOrWhiteSpace(pathValue))
                return false;

            visualStylePath = NormalizeThemePath(pathValue, themeFilePath);
            return true;
        }
        catch
        {
            return false;
        }
    }

    internal static string NormalizeResourcePath(string path, string themeFilePath) =>
        NormalizeThemePath(path, themeFilePath);

    private static VisualStyleThemeEntry? TryParseThemeFile(string themeFilePath)
    {
        if (!TryReadVisualStylesInfo(themeFilePath, out var visualStylePath, out var colorStyle, out var size))
            return null;

        var metadata = ThemeFileMetadataReader.Read(themeFilePath);
        var accentColor = ThemeAccentColorHelper.TryParseColorizationColor(themeFilePath);
        var isDarkMode = ThemeAppModeHelper.ReadIsDarkMode(themeFilePath);
        return new VisualStyleThemeEntry(
            metadata.DisplayName,
            themeFilePath,
            visualStylePath,
            colorStyle,
            size,
            IsCustomVisualStylePath(visualStylePath),
            metadata.PreviewImagePath,
            metadata.ImageCount,
            metadata.TooltipText,
            accentColor,
            isDarkMode);
    }

    private static IEnumerable<string> EnumerateThemeFiles()
    {
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var root in ThemeScanRoots())
        {
            if (!Directory.Exists(root))
                continue;

            IEnumerable<string> files;
            var searchOption = SearchOption.TopDirectoryOnly;

            try
            {
                files = Directory.EnumerateFiles(root, "*.theme", searchOption);
            }
            catch
            {
                continue;
            }

            foreach (var file in files)
            {
                if (seen.Add(file))
                    yield return file;
            }
        }
    }

    private static IEnumerable<string> ThemeScanRoots()
    {
        yield return SystemThemesDirectory;
    }

    private static string NormalizeThemePath(string path, string themeFilePath)
    {
        var expanded = Environment.ExpandEnvironmentVariables(path.Trim().Trim('"'));
        expanded = expanded.Replace("%ResourceDir%", ResourcesDirectory, StringComparison.OrdinalIgnoreCase);

        if (Path.IsPathRooted(expanded))
            return Path.GetFullPath(expanded);

        var themeDir = Path.GetDirectoryName(themeFilePath) ?? SystemThemesDirectory;
        return Path.GetFullPath(Path.Combine(themeDir, expanded));
    }

    private static string StyleCountLabel(int count) =>
        GlassSettingsResources.CountForm(count, "Themes_OneInstalled", "Themes_FewInstalled", "Themes_ManyInstalled");
}

public sealed record VisualStyleThemeEntry(
    string DisplayName,
    string ThemeFilePath,
    string VisualStylePath,
    string ColorStyle,
    string Size,
    bool IsCustom,
    string? PreviewImagePath,
    int ImageCount,
    string TooltipText,
    Windows.UI.Color? AccentColor = null,
    bool IsDarkMode = false);
