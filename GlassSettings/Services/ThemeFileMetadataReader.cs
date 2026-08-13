using GlassSettings.Strings;

namespace GlassSettings.Services;

/// <summary>Метаданные .theme для плиток, превью и тултипов (как в системных «Настройках»).</summary>
public static class ThemeFileMetadataReader
{
    private const string ThemeSection = "Theme";
    private const string DesktopSection = "Control Panel\\Desktop";
    private const string SlideshowSection = "Slideshow";
    private const string SoundsSection = "Sounds";

    private static readonly string[] ImageExtensions = [".jpg", ".jpeg", ".png", ".bmp", ".webp"];

    public static ThemeFileMetadata Read(string themeFilePath)
    {
        if (string.IsNullOrWhiteSpace(themeFilePath) || !File.Exists(themeFilePath))
        {
            var fallbackName = Path.GetFileNameWithoutExtension(themeFilePath ?? string.Empty);
            return CreateMetadata(fallbackName, null, 0);
        }

        try
        {
            var sections = ParseSections(themeFilePath);
            var displayName = ResolveDisplayName(sections, themeFilePath);
            var previewPath = ResolvePreviewPath(sections, themeFilePath);
            var imageCount = CountImages(sections, themeFilePath, previewPath);
            return CreateMetadata(displayName, previewPath, imageCount);
        }
        catch
        {
            var fallbackName = Path.GetFileNameWithoutExtension(themeFilePath);
            return CreateMetadata(fallbackName, null, 0);
        }
    }

    public static string ReadDisplayName(string themeFilePath) => Read(themeFilePath).DisplayName;

    public static string? TryReadPreviewImagePath(string themeFilePath) => Read(themeFilePath).PreviewImagePath;

    private static ThemeFileMetadata CreateMetadata(string displayName, string? previewPath, int imageCount)
    {
        var tooltip = GlassSettingsResources.Format("Themes_TooltipFormat", displayName, imageCount);
        return new ThemeFileMetadata(displayName, previewPath, imageCount, tooltip);
    }

    private static string ResolveDisplayName(IReadOnlyDictionary<string, Dictionary<string, string>> sections, string themeFilePath)
    {
        if (sections.TryGetValue(ThemeSection, out var themeSection) &&
            themeSection.TryGetValue("DisplayName", out var rawDisplayName))
        {
            var resolved = ThemeIndirectStringResolver.Resolve(rawDisplayName);
            if (IsUsableDisplayName(resolved))
                return resolved.Trim();
        }

        // Повтор с альтернативными кодировками (кириллица в UTF-16 без BOM).
        foreach (var encoding in ThemeFileLineReader.CandidateEncodings(themeFilePath))
        {
            if (TryReadRawDisplayName(themeFilePath, encoding, out var raw)
                && IsUsableDisplayName(ThemeIndirectStringResolver.Resolve(raw)))
            {
                return ThemeIndirectStringResolver.Resolve(raw).Trim();
            }
        }

        return Path.GetFileNameWithoutExtension(themeFilePath);
    }

    private static bool TryReadRawDisplayName(string themeFilePath, System.Text.Encoding encoding, out string value)
    {
        value = string.Empty;
        try
        {
            var inTheme = false;
            foreach (var rawLine in File.ReadLines(themeFilePath, encoding))
            {
                var line = rawLine.Trim();
                if (line.Length == 0 || line[0] == ';')
                    continue;

                if (line.StartsWith('[') && line.EndsWith(']'))
                {
                    inTheme = string.Equals(line[1..^1].Trim(), ThemeSection, StringComparison.OrdinalIgnoreCase);
                    continue;
                }

                if (!inTheme)
                    continue;

                var separator = line.IndexOf('=');
                if (separator <= 0)
                    continue;

                if (!string.Equals(line[..separator].Trim(), "DisplayName", StringComparison.OrdinalIgnoreCase))
                    continue;

                value = line[(separator + 1)..].Trim().Trim('"');
                return !string.IsNullOrWhiteSpace(value);
            }
        }
        catch
        {
            return false;
        }

        return false;
    }

    private static bool IsUsableDisplayName(string? value)
    {
        if (string.IsNullOrWhiteSpace(value) || LooksLikeResourceReference(value))
            return false;

        // Replacement char после неверной кодировки.
        if (value.IndexOf('\uFFFD') >= 0)
            return false;

        // UTF-16, прочитанный как UTF-8, часто даёт NUL внутри строки.
        if (value.IndexOf('\0') >= 0)
            return false;

        return true;
    }

    private static bool LooksLikeResourceReference(string value)
    {
        var trimmed = value.Trim();
        return trimmed.StartsWith('@') ||
               (trimmed.Contains('\\') && trimmed.Contains(".dll", StringComparison.OrdinalIgnoreCase));
    }

    private static string? ResolvePreviewPath(
        IReadOnlyDictionary<string, Dictionary<string, string>> sections,
        string themeFilePath)
    {
        if (sections.TryGetValue(DesktopSection, out var desktopSection) &&
            desktopSection.TryGetValue("Wallpaper", out var wallpaperRaw) &&
            TryResolveExistingImagePath(wallpaperRaw, themeFilePath, out var wallpaperPath))
        {
            return wallpaperPath;
        }

        if (sections.TryGetValue(ThemeSection, out var themeSection))
        {
            if (themeSection.TryGetValue("Wallpaper", out var themeWallpaperRaw) &&
                TryResolveExistingImagePath(themeWallpaperRaw, themeFilePath, out var themeWallpaperPath))
            {
                return themeWallpaperPath;
            }

            if (themeSection.TryGetValue("BrandImages", out var brandImagesRaw) &&
                TryExtractBrandImagePath(brandImagesRaw, themeFilePath, out var brandPath))
            {
                return brandPath;
            }
        }

        return null;
    }

    private static int CountImages(
        IReadOnlyDictionary<string, Dictionary<string, string>> sections,
        string themeFilePath,
        string? previewPath)
    {
        if (sections.TryGetValue(SlideshowSection, out var slideshowSection))
        {
            // 1) Явный список Windows: Item0Path…ItemNPath (inbox themeA–D и сохранённые).
            var itemCount = CountSlideshowItemPaths(slideshowSection, themeFilePath);
            if (itemCount > 0)
                return itemCount;

            // 2) Папка слайдшоу (только если нет ItemNPath).
            if (slideshowSection.TryGetValue("ImagesRootPath", out var imagesRootRaw))
            {
                var imagesRoot = VisualStyleCatalog.NormalizeResourcePath(imagesRootRaw, themeFilePath);
                if (Directory.Exists(imagesRoot))
                {
                    try
                    {
                        var count = Directory
                            .EnumerateFiles(imagesRoot, "*.*", SearchOption.TopDirectoryOnly)
                            .Count(file => ImageExtensions.Contains(Path.GetExtension(file).ToLowerInvariant()));
                        if (count > 0)
                            return count;
                    }
                    catch
                    {
                        // fallback ниже
                    }
                }
            }
        }

        if (!string.IsNullOrWhiteSpace(previewPath))
            return 1;

        return 0;
    }

    private static int CountSlideshowItemPaths(
        Dictionary<string, string> slideshowSection,
        string themeFilePath)
    {
        var count = 0;
        for (var i = 0; i < 64; i++)
        {
            if (!slideshowSection.TryGetValue("Item" + i + "Path", out var raw)
                || string.IsNullOrWhiteSpace(raw))
            {
                // Разрешаем дырки только в начале; после первого пропуска — стоп.
                if (i > 0)
                    break;
                continue;
            }

            if (TryResolveExistingImagePath(raw, themeFilePath, out _))
                count++;
        }

        return count;
    }

    private static bool TryExtractBrandImagePath(string brandImages, string themeFilePath, out string path)
    {
        path = string.Empty;
        foreach (var part in brandImages.Split(','))
        {
            var candidate = part.Trim().Trim('"');
            if (!LooksLikeImagePath(candidate))
                continue;

            if (TryResolveExistingImagePath(candidate, themeFilePath, out path))
                return true;
        }

        path = string.Empty;
        return false;
    }

    private static bool LooksLikeImagePath(string value)
    {
        if (string.IsNullOrWhiteSpace(value))
            return false;

        var ext = Path.GetExtension(value).ToLowerInvariant();
        return ImageExtensions.Contains(ext);
    }

    private static bool TryResolveExistingImagePath(string rawPath, string themeFilePath, out string path)
    {
        path = VisualStyleCatalog.NormalizeResourcePath(rawPath, themeFilePath);
        return File.Exists(path);
    }

    private static Dictionary<string, Dictionary<string, string>> ParseSections(string themeFilePath)
    {
        var sections = new Dictionary<string, Dictionary<string, string>>(StringComparer.OrdinalIgnoreCase);
        string? currentSection = null;

        foreach (var rawLine in ThemeFileLineReader.ReadLines(themeFilePath))
        {
            var line = rawLine.Trim();
            if (line.Length == 0 || line[0] == ';')
                continue;

            if (line.StartsWith('[') && line.EndsWith(']'))
            {
                currentSection = line[1..^1].Trim();
                if (!sections.ContainsKey(currentSection))
                    sections[currentSection] = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
                continue;
            }

            if (currentSection is null)
                continue;

            var separator = line.IndexOf('=');
            if (separator <= 0)
                continue;

            var key = line[..separator].Trim();
            var value = line[(separator + 1)..].Trim();
            sections[currentSection][key] = value;
        }

        return sections;
    }
}

public sealed record ThemeFileMetadata(
    string DisplayName,
    string? PreviewImagePath,
    int ImageCount,
    string TooltipText);
