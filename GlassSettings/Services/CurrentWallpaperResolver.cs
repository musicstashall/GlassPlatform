using System.Collections.Generic;
using System.Text;
using GlassSettings.Strings;
using Microsoft.Win32;

namespace GlassSettings.Services;

/// <summary>Текущие обои рабочего стола (реестр и .theme).</summary>
public static class CurrentWallpaperResolver
{
    private const string DesktopRegistryPath = @"Control Panel\Desktop";
    private const string DesktopSlideshowRegistryPath = @"Control Panel\Personalization\Desktop Slideshow";
    private const string WallpaperValueName = "Wallpaper";
    private const string ImageColorValueName = "ImageColor";
    private const string LastTickLowValueName = "LastTickLow";
    private const string LastTickHighValueName = "LastTickHigh";
    private const string TranscodedImageCacheValueName = "TranscodedImageCache_000";
    private const string TranscodedImageCacheFallbackValueName = "TranscodedImageCache";
    private const string WallpaperHistoryRegistryPath =
        @"Software\Microsoft\Windows\CurrentVersion\Explorer\Wallpapers";
    private const string BackgroundTypeValueName = "BackgroundType";

    /// <summary>Explorer\Wallpapers\BackgroundType: фото.</summary>
    public const int BackgroundTypePicture = 0;
    /// <summary>Explorer\Wallpapers\BackgroundType: сплошной цвет.</summary>
    public const int BackgroundTypeSolid = 1;
    /// <summary>Explorer\Wallpapers\BackgroundType: слайдшоу.</summary>
    public const int BackgroundTypeSlideshow = 2;
    /// <summary>Explorer\Wallpapers\BackgroundType: Windows Spotlight.</summary>
    public const int BackgroundTypeSpotlight = 3;

    private const int MaxHistoryPaths = 32;

    public static string? TryGetDesktopWallpaperPath()
    {
        // Win11: Wallpaper часто %LOCALAPPDATA%\...\TranscodedWallpaper — это актуальный растр.
        if (TryReadRegistryWallpaper(out var registryPath))
            return registryPath;

        if (TryReadWallpaperHistory(out var historyPath))
            return historyPath;

        if (TryReadThemeWallpaper(out var themeWallpaper))
            return themeWallpaper;

        return null;
    }

    /// <summary>
    /// Подпись кнопки «Фон»: слайд-шоу → счётчик; иначе всегда «Windows: интересное»
    /// (как на хабе Параметров Windows).
    /// </summary>
    public static string TryGetDesktopBackgroundSubtitle()
    {
        try
        {
            if (TryGetBackgroundType(out var backgroundType)
                && backgroundType == BackgroundTypeSlideshow
                && IsLiveSlideshow(out var imageCount)
                && imageCount > 1)
            {
                return GlassSettingsResources.Format("Themes_BackgroundSlideshowFormat", imageCount);
            }

            return GlassSettingsResources.Get("Themes_BackgroundSpotlight");
        }
        catch
        {
            return GlassSettingsResources.Get("Themes_BackgroundSpotlight");
        }
    }

    /// <summary>Текущий BackgroundType из Explorer\Wallpapers (0…3).</summary>
    public static bool TryGetBackgroundType(out int backgroundType) =>
        TryReadBackgroundType(out backgroundType);

    /// <summary>
    /// Сигнал смены обоев: LastTick, TranscodedImageCache, BackgroundType, history paths.
    /// </summary>
    public static string? TryGetWallpaperRegistrySignal()
    {
        var parts = new List<string>(8);

        var lastTickLow = RegistryValueChangeWatcher.ReadRegistryValue(
            DesktopSlideshowRegistryPath,
            LastTickLowValueName);
        if (!string.IsNullOrWhiteSpace(lastTickLow))
            parts.Add(lastTickLow);

        var lastTickHigh = RegistryValueChangeWatcher.ReadRegistryValue(
            DesktopSlideshowRegistryPath,
            LastTickHighValueName);
        if (!string.IsNullOrWhiteSpace(lastTickHigh))
            parts.Add(lastTickHigh);

        var transcoded = RegistryValueChangeWatcher.ReadRegistryValue(
            DesktopRegistryPath,
            TranscodedImageCacheValueName);
        if (!string.IsNullOrWhiteSpace(transcoded))
            parts.Add(transcoded);
        else
        {
            var transcodedFallback = RegistryValueChangeWatcher.ReadRegistryValue(
                DesktopRegistryPath,
                TranscodedImageCacheFallbackValueName);
            if (!string.IsNullOrWhiteSpace(transcodedFallback))
                parts.Add(transcodedFallback);
        }

        var backgroundType = RegistryValueChangeWatcher.ReadRegistryValue(
            WallpaperHistoryRegistryPath,
            BackgroundTypeValueName);
        if (!string.IsNullOrWhiteSpace(backgroundType))
            parts.Add("bt:" + backgroundType);

        var wallpaperStyle = RegistryValueChangeWatcher.ReadRegistryValue(
            DesktopRegistryPath,
            "WallpaperStyle");
        if (!string.IsNullOrWhiteSpace(wallpaperStyle))
            parts.Add("ws:" + wallpaperStyle);

        var tileWallpaper = RegistryValueChangeWatcher.ReadRegistryValue(
            DesktopRegistryPath,
            "TileWallpaper");
        if (!string.IsNullOrWhiteSpace(tileWallpaper))
            parts.Add("tw:" + tileWallpaper);

        var historyFingerprint = BuildHistoryPathsFingerprint();
        if (!string.IsNullOrWhiteSpace(historyFingerprint))
            parts.Add(historyFingerprint);

        // Не добавлять ImagesRootPath / GlassLast / Desktop\Wallpaper сюда:
        // они меняются до TranscodedImageCache → двойной crossfade превью.
        // Смена слайдшоу детектится LastTick + Transcoded + history paths.

        return parts.Count == 0 ? null : string.Join('|', parts);
    }

    /// <summary>
    /// Число картинок live-альбома слайдшоу (COM subset / папка / Windows album).
    /// </summary>
    public static int GetLiveSlideshowImageCount() => CountLiveSlideshowImages();

    private static bool IsLiveSlideshow(out int imageCount)
    {
        imageCount = 0;

        // BackgroundType — источник истины (history/LastTick часто остаются после picture).
        if (TryReadBackgroundType(out var backgroundType))
        {
            if (backgroundType != BackgroundTypeSlideshow)
                return false;

            imageCount = CountLiveSlideshowImages();
            if (imageCount <= 0)
                imageCount = TryGetThemeSlideshowImageCount();
            return true;
        }

        // BackgroundType отсутствует: слабый fallback только по theme Slideshow.
        var themeCount = TryGetThemeSlideshowImageCount();
        if (themeCount > 1)
        {
            imageCount = themeCount;
            return true;
        }

        return false;
    }

    /// <summary>
    /// Число картинок альбома: live <see cref="DesktopWallpaperCom.TryGetSlideshowAlbum"/> (FILESYS subset).
    /// ItemNPath не используем — часто устаревший относительно COM.
    /// </summary>
    private static int CountLiveSlideshowImages()
    {
        if (DesktopWallpaperCom.TryGetSlideshowAlbum(out var snap))
        {
            if (snap.IsFileSubset && snap.ImagePaths.Count > 0)
            {
                var n = 0;
                foreach (var path in snap.ImagePaths)
                {
                    if (File.Exists(path))
                        n++;
                }

                if (n > 0)
                    return n;
            }

            if (snap.Kind == SlideshowAlbumKind.WindowsDesktopBackgrounds)
            {
                var windowsCount = DesktopBackgroundService.EnumerateWindowsDesktopBackgroundsAlbumImages().Count;
                if (windowsCount > 0)
                    return windowsCount;
            }

            if (snap.Kind == SlideshowAlbumKind.Folder
                && !string.IsNullOrWhiteSpace(snap.FolderPath)
                && Directory.Exists(snap.FolderPath))
            {
                var folderCount = CountImageFilesInDirectory(snap.FolderPath);
                if (folderCount > 0)
                    return folderCount;
            }
        }

        if (TryReadDesktopSlideshowString("ImagesRootPath", out var imagesRoot)
            && Directory.Exists(imagesRoot))
        {
            var rootCount = CountImageFilesInDirectory(imagesRoot);
            if (rootCount > 0)
                return rootCount;
        }

        return 0;
    }

    private static bool TryReadDesktopSlideshowString(string valueName, out string path)
    {
        path = string.Empty;
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(DesktopSlideshowRegistryPath);
            if (key?.GetValue(valueName) is not string raw || string.IsNullOrWhiteSpace(raw))
                return false;

            var expanded = Environment.ExpandEnvironmentVariables(raw.Trim().Trim('"'));
            if (string.IsNullOrWhiteSpace(expanded))
                return false;

            path = Path.GetFullPath(expanded);
            return true;
        }
        catch
        {
            return false;
        }
    }

    private static readonly HashSet<string> SlideshowImageExtensions = new(StringComparer.OrdinalIgnoreCase)
    {
        ".jpg", ".jpeg", ".png", ".bmp", ".gif", ".tif", ".tiff", ".jfif", ".webp", ".heic"
    };

    private static int CountImageFilesInDirectory(string directory)
    {
        try
        {
            return Directory
                .EnumerateFiles(directory, "*.*", SearchOption.TopDirectoryOnly)
                .Count(file => SlideshowImageExtensions.Contains(Path.GetExtension(file)));
        }
        catch
        {
            return 0;
        }
    }

    private static string? TryGetSingleWallpaperDisplayName()
    {
        if (TryReadHistoryPath(0, out var history0) && !IsTranscodedWallpaperPath(history0))
            return Path.GetFileName(history0);

        if (TryReadThemeWallpaper(out var themeWallpaper) && !IsTranscodedWallpaperPath(themeWallpaper))
            return Path.GetFileName(themeWallpaper);

        if (TryReadRegistryWallpaper(out var registryPath) && !IsTranscodedWallpaperPath(registryPath))
            return Path.GetFileName(registryPath);

        return null;
    }

    private static bool IsTranscodedWallpaperPath(string path) =>
        path.Contains("TranscodedWallpaper", StringComparison.OrdinalIgnoreCase);

    private static bool TryReadBackgroundType(out int backgroundType)
    {
        backgroundType = -1;
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(WallpaperHistoryRegistryPath);
            if (key?.GetValue(BackgroundTypeValueName) is int i)
            {
                backgroundType = i;
                return true;
            }

            if (key?.GetValue(BackgroundTypeValueName) is long l)
            {
                backgroundType = (int)l;
                return true;
            }

            var raw = RegistryValueChangeWatcher.ReadRegistryValue(
                WallpaperHistoryRegistryPath,
                BackgroundTypeValueName);
            if (int.TryParse(raw, out var parsed))
            {
                backgroundType = parsed;
                return true;
            }
        }
        catch
        {
            // ignore
        }

        return false;
    }

    private static int CountExistingHistoryPaths()
    {
        var count = 0;
        for (var i = 0; i < MaxHistoryPaths; i++)
        {
            if (TryReadHistoryPath(i, out _))
                count++;
        }

        return count;
    }

    private static string? BuildHistoryPathsFingerprint()
    {
        var sb = new StringBuilder();
        for (var i = 0; i < MaxHistoryPaths; i++)
        {
            var raw = RegistryValueChangeWatcher.ReadRegistryValue(
                WallpaperHistoryRegistryPath,
                "BackgroundHistoryPath" + i);
            if (string.IsNullOrWhiteSpace(raw))
                continue;

            if (sb.Length > 0)
                sb.Append(';');
            sb.Append(i).Append('=').Append(raw);
        }

        return sb.Length == 0 ? null : sb.ToString();
    }

    private static bool TryReadHistoryPath(int index, out string path) =>
        TryReadRegistryString(WallpaperHistoryRegistryPath, "BackgroundHistoryPath" + index, out path);

    private static int TryGetThemeSlideshowImageCount()
    {
        if (!VisualStyleThemeManager.TryGetCurrentThemeFilePath(out var themePath)
            || string.IsNullOrWhiteSpace(themePath))
        {
            return 0;
        }

        var normalized = ThemePathNormalizer.Normalize(themePath);
        if (string.IsNullOrWhiteSpace(normalized))
            return 0;

        return ThemeFileMetadataReader.Read(normalized).ImageCount;
    }

    /// <summary>
    /// Идентичность обоев для кэша превью: реестровый сигнал и фактический путь растра.
    /// </summary>
    public static string? TryGetWallpaperImageColorIdentity()
    {
        var registrySignal = TryGetWallpaperRegistrySignal();
        var resolvedPath = TryGetDesktopWallpaperPath();

        var parts = new List<string>(4);
        if (!string.IsNullOrWhiteSpace(registrySignal))
            parts.Add(registrySignal);
        if (!string.IsNullOrWhiteSpace(resolvedPath))
        {
            parts.Add(resolvedPath);
            if (resolvedPath.Contains("TranscodedWallpaper", StringComparison.OrdinalIgnoreCase)
                && File.Exists(resolvedPath))
            {
                parts.Add(File.GetLastWriteTimeUtc(resolvedPath).Ticks.ToString());
            }
        }

        return parts.Count == 0 ? null : string.Join('|', parts);
    }

    internal static bool IsUnsavedThemeFile(string themeFilePath) =>
        UserThemeCatalog.IsPersonalizedThemeFile(themeFilePath);

    private static bool TryReadThemeWallpaper(out string path)
    {
        path = string.Empty;
        if (!VisualStyleThemeManager.TryGetCurrentThemeFilePath(out var themePath))
            return false;

        var normalizedTheme = ThemePathNormalizer.Normalize(themePath);
        if (string.IsNullOrWhiteSpace(normalizedTheme))
            return false;

        var metadata = ThemeFileMetadataReader.Read(normalizedTheme);
        if (string.IsNullOrWhiteSpace(metadata.PreviewImagePath))
            return false;

        if (!File.Exists(metadata.PreviewImagePath))
            return false;

        path = metadata.PreviewImagePath;
        return true;
    }

    private static bool TryReadRegistryWallpaper(out string path) =>
        TryExpandRegistryPath(WallpaperValueName, out path);

    private static string? TryReadRegistryWallpaperRaw()
    {
        using var key = Registry.CurrentUser.OpenSubKey(DesktopRegistryPath);
        var raw = key?.GetValue(WallpaperValueName) as string;
        if (string.IsNullOrWhiteSpace(raw))
            return null;

        return Environment.ExpandEnvironmentVariables(raw.Trim().Trim('"'));
    }

    private static bool TryExpandRegistryPath(string valueName, out string path)
    {
        path = string.Empty;
        if (!TryReadRegistryString(DesktopRegistryPath, valueName, out path))
            return false;

        return true;
    }

    private static bool TryReadWallpaperHistory(out string path) =>
        TryReadRegistryString(WallpaperHistoryRegistryPath, "BackgroundHistoryPath0", out path);

    private static bool TryReadRegistryString(string subKeyPath, string valueName, out string path)
    {
        path = string.Empty;
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(subKeyPath);
            if (key?.GetValue(valueName) is not string raw || string.IsNullOrWhiteSpace(raw))
                return false;

            var expanded = Environment.ExpandEnvironmentVariables(raw.Trim().Trim('"'));
            if (string.IsNullOrWhiteSpace(expanded))
                return false;

            expanded = Path.GetFullPath(expanded);
            if (!File.Exists(expanded))
                return false;

            path = expanded;
            return true;
        }
        catch
        {
            return false;
        }
    }
}
