using System.Runtime.InteropServices;
using Microsoft.UI.Xaml;
using Microsoft.Win32;
using WinRT.Interop;

namespace GlassSettings.Services;

/// <summary>
/// Фон рабочего стола: BackgroundType, WallpaperStyle / TileWallpaper, apply picture + history.
/// </summary>
internal static class DesktopBackgroundService
{
    private const uint SpiSetDeskWallpaper = 0x0014;
    private const uint SpifUpdateIniFile = 0x01;
    private const uint SpifSendChange = 0x02;

    private const string DesktopRegistryPath = @"Control Panel\Desktop";
    private const string WallpaperHistoryRegistryPath =
        @"Software\Microsoft\Windows\CurrentVersion\Explorer\Wallpapers";
    private const string DesktopSlideshowRegistryPath = @"Control Panel\Personalization\Desktop Slideshow";
    private const string WallpaperValueName = "Wallpaper";
    private const string WallpaperStyleValueName = "WallpaperStyle";
    private const string TileWallpaperValueName = "TileWallpaper";
    private const string BackgroundTypeValueName = "BackgroundType";
    /// <summary>Наша персистентная папка слайдшоу (COM после Picture/Spotlight часто пуст).</summary>
    private const string LastSlideshowFolderValueName = "GlassLastSlideshowFolder";
    /// <summary>Folder | WindowsDesktopBackgrounds — identity альбома для restore/UI.</summary>
    private const string LastSlideshowKindValueName = "GlassLastSlideshowKind";
    private const string SlideshowKindFolder = "Folder";
    private const string SlideshowKindWindowsDesktopBackgrounds = "WindowsDesktopBackgrounds";

    private const int RecentHistoryCount = 5;

    /// <summary>Пункты combo = BackgroundType: Фото / Solid / Слайд-шоу / Spotlight.</summary>
    public enum ModeChoice
    {
        Picture = 0,
        Solid = 1,
        Slideshow = 2,
        Spotlight = 3
    }

    public enum WallpaperFit
    {
        Fill = 0,
        Fit = 1,
        Stretch = 2,
        Tile = 3,
        Center = 4,
        Span = 5
    }

    /// <summary>Интервалы смены кадра (мс), как в Параметрах Windows.</summary>
    public static readonly uint[] SlideshowIntervalMs =
    [
        60_000,
        600_000,
        1_800_000,
        3_600_000,
        21_600_000,
        86_400_000
    ];

    private const string DesktopSpotlightSettingsPath =
        @"Software\Microsoft\Windows\CurrentVersion\DesktopSpotlight\Settings";
    private const string ColorsRegistryPath = @"Control Panel\Colors";

    public static bool TryGetBackgroundType(out int backgroundType) =>
        CurrentWallpaperResolver.TryGetBackgroundType(out backgroundType);

    public static ModeChoice GetModeChoice()
    {
        if (!TryGetBackgroundType(out var type))
            return ModeChoice.Picture;

        return type switch
        {
            CurrentWallpaperResolver.BackgroundTypeSolid => ModeChoice.Solid,
            CurrentWallpaperResolver.BackgroundTypeSlideshow => ModeChoice.Slideshow,
            CurrentWallpaperResolver.BackgroundTypeSpotlight => ModeChoice.Spotlight,
            _ => ModeChoice.Picture
        };
    }

    public static int ModeChoiceToBackgroundType(ModeChoice mode) => (int)mode;

    public static void ApplyModeChoice(ModeChoice mode)
    {
        if (mode == ModeChoice.Slideshow)
        {
            if (TryRestoreSlideshowFolder())
                return;

            // Последний шанс: inbox Nature — не писать BackgroundType=2 без живого альбома
            // (иначе чёрный стол при ImagesRootPath из UI Параметров).
            var nature = Path.Combine(
                Environment.GetEnvironmentVariable("SystemRoot") ?? @"C:\Windows",
                "Web",
                "Wallpaper",
                "Nature");
            if (TryApplySlideshowFolder(nature))
                return;

            return;
        }

        if (mode == ModeChoice.Spotlight)
        {
            RememberSlideshowFolderFromCom();
            TryApplySpotlight();
            return;
        }

        if (mode == ModeChoice.Solid)
        {
            RememberSlideshowFolderFromCom();
            TryApplySolidBackground();
            return;
        }

        // Фото: нужна реальная картинка. Enable(false) гасит весь фон → чёрный стол;
        // SetWallpaper сам останавливает слайдшоу.
        RememberSlideshowFolderFromCom();
        if (!TrySwitchToPictureFromHistory())
        {
            TryWriteDword(
                WallpaperHistoryRegistryPath,
                BackgroundTypeValueName,
                CurrentWallpaperResolver.BackgroundTypePicture);
        }
    }

    /// <summary>Сплошной цвет без палитры UI — текущий BackgroundColor системы.</summary>
    public static bool TryApplySolidBackground()
    {
        uint colorRef = 0;
        if (!DesktopWallpaperCom.TryGetBackgroundColor(out colorRef))
            colorRef = TryReadColorsBackgroundColorRef() ?? 0x00191919;

        if (!DesktopWallpaperCom.TrySetSolidBackground(colorRef))
        {
            TryWriteDesktopString(WallpaperValueName, string.Empty);
            TryWriteColorsBackground(colorRef);
            ApplySpiWallpaper(string.Empty);
        }
        else
        {
            TryWriteDesktopString(WallpaperValueName, string.Empty);
            TryWriteColorsBackground(colorRef);
        }

        TryWriteDword(
            WallpaperHistoryRegistryPath,
            BackgroundTypeValueName,
            CurrentWallpaperResolver.BackgroundTypeSolid);
        return true;
    }

    /// <summary>Windows Spotlight: EnabledState + BackgroundType + asset если есть.</summary>
    public static bool TryApplySpotlight()
    {
        TryWriteDword(DesktopSpotlightSettingsPath, "EnabledState", 1);
        TryWriteDword(
            WallpaperHistoryRegistryPath,
            BackgroundTypeValueName,
            CurrentWallpaperResolver.BackgroundTypeSpotlight);

        var hasAsset = TryFindSpotlightLandscapeAsset(out var asset);
        var setOk = false;
        if (hasAsset)
        {
            setOk = DesktopWallpaperCom.TrySetWallpaper(asset);
            // Сразу пишем Desktop\Wallpaper — иначе preview/identity ещё на старом path.
            if (setOk)
            {
                TryWriteDesktopString(WallpaperValueName, asset);
                ApplySpiWallpaper(asset);
            }
        }

        return true;
    }

    private static bool TryFindSpotlightLandscapeAsset(out string path)
    {
        path = string.Empty;
        try
        {
            var systemApps = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.Windows),
                "SystemApps");
            if (Directory.Exists(systemApps))
            {
                foreach (var cbs in Directory.EnumerateDirectories(systemApps, "MicrosoftWindows.Client.CBS_*"))
                {
                    var images = Path.Combine(cbs, "DesktopSpotlight", "Assets", "Images");
                    if (!Directory.Exists(images))
                        continue;

                    foreach (var name in new[] { "image_0.jpg", "image_1.jpg", "image_2.jpg" })
                    {
                        var candidate = Path.Combine(images, name);
                        if (File.Exists(candidate))
                        {
                            path = candidate;
                            return true;
                        }
                    }

                    var any = Directory.EnumerateFiles(images, "*.jpg").FirstOrDefault();
                    if (!string.IsNullOrWhiteSpace(any))
                    {
                        path = any;
                        return true;
                    }
                }
            }

            var localPackages = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "Packages");
            if (Directory.Exists(localPackages))
            {
                foreach (var pkg in Directory.EnumerateDirectories(localPackages, "MicrosoftWindows.Client.CBS_*"))
                {
                    var iris = Path.Combine(pkg, "LocalCache", "Microsoft", "IrisService");
                    if (!Directory.Exists(iris))
                        continue;

                    var jpg = Directory
                        .EnumerateFiles(iris, "*.jpg", SearchOption.AllDirectories)
                        .FirstOrDefault();
                    if (!string.IsNullOrWhiteSpace(jpg))
                    {
                        path = jpg;
                        return true;
                    }
                }
            }
        }
        catch
        {
            // ignore
        }

        return false;
    }

    private static uint? TryReadColorsBackgroundColorRef()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(ColorsRegistryPath);
            if (key?.GetValue("Background") is not string raw || string.IsNullOrWhiteSpace(raw))
                return null;

            var parts = raw.Split(' ', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
            if (parts.Length < 3
                || !byte.TryParse(parts[0], out var r)
                || !byte.TryParse(parts[1], out var g)
                || !byte.TryParse(parts[2], out var b))
            {
                return null;
            }

            // COLORREF: 0x00BBGGRR
            return (uint)(r | (g << 8) | (b << 16));
        }
        catch
        {
            return null;
        }
    }

    private static void TryWriteColorsBackground(uint colorRef)
    {
        var r = (byte)(colorRef & 0xFF);
        var g = (byte)((colorRef >> 8) & 0xFF);
        var b = (byte)((colorRef >> 16) & 0xFF);
        try
        {
            using var key = Registry.CurrentUser.CreateSubKey(ColorsRegistryPath);
            key?.SetValue("Background", $"{r} {g} {b}", RegistryValueKind.String);
        }
        catch
        {
            // ignore
        }
    }

    /// <summary>Повторно включает слайдшоу с последней папкой / Windows-альбомом.</summary>
    public static bool TryRestoreSlideshowFolder()
    {
        if (DesktopWallpaperCom.TryGetSlideshowAlbum(out var live)
            && live.Kind == SlideshowAlbumKind.WindowsDesktopBackgrounds)
        {
            bool windowsOk;
            if (live.IsFileSubset && live.ImagePaths.Count > 0)
            {
                windowsOk = TryApplySlideshowImageSubset(
                    live.ImagePaths,
                    SlideshowAlbumKind.WindowsDesktopBackgrounds,
                    live.DisplayName);
            }
            else
            {
                windowsOk = TryApplyWindowsDesktopBackgroundsAlbum();
            }

            if (windowsOk)
                return true;
        }
        else if (TryReadPersistedSlideshowKind() == SlideshowAlbumKind.WindowsDesktopBackgrounds
                 || _lastSlideshowKind == SlideshowAlbumKind.WindowsDesktopBackgrounds)
        {
            if (TryApplyWindowsDesktopBackgroundsAlbum())
                return true;

            // Windows search-folder / полный subset не встали — ниже FS-папка (Nature).
        }

        var comOk = TryNormalizeSlideshowFolder(
            DesktopWallpaperCom.TryGetSlideshowFolder(out var comFolder, out _) ? comFolder : null,
            out comFolder);
        var cacheOk = TryNormalizeSlideshowFolder(_lastSlideshowFolderPath, out var cacheFolder);
        var persistedOk = TryNormalizeSlideshowFolder(TryReadPersistedSlideshowFolder(), out var persisted);
        var registryOk = TryNormalizeSlideshowFolder(
            TryResolveSlideshowFolderFromDesktopSlideshowRegistry(),
            out var registryFolder);

        // Self-heal: GlassLast/ImagesRoot отравлены Item0Path → ThemeA..D, тогда как
        // типичный последний альбом Windows — Nature (и он же был в COM у пользователя).
        _ = TryHealInboxThemeSlideshowFolder(ref persistedOk, ref persisted)
            | TryHealInboxThemeSlideshowFolder(ref registryOk, ref registryFolder);

        // GlassLast = Web\Wallpaper (Windows kind) — не FS-альбом; не блокируем ImagesRootPath.
        if (persistedOk
            && persisted is not null
            && DesktopWallpaperCom.IsUnderWindowsDesktopBackgroundsRoot(persisted)
            && Path.GetFullPath(persisted).Equals(
                DesktopWallpaperCom.GetWindowsDesktopBackgroundsRoot(),
                StringComparison.OrdinalIgnoreCase))
        {
            persistedOk = false;
            persisted = null;
        }

        // Приоритет: живой COM → кэш сессии → GlassLast (явный выбор/COM) → ImagesRootPath.
        // Item0Path больше не участвует (ложно давал ThemeA вместо Nature).
        if (comOk)
            return TryApplySlideshowFolder(comFolder!);
        if (cacheOk
            && cacheFolder is not null
            && !Path.GetFullPath(cacheFolder).Equals(
                DesktopWallpaperCom.GetWindowsDesktopBackgroundsRoot(),
                StringComparison.OrdinalIgnoreCase))
        {
            return TryApplySlideshowFolder(cacheFolder);
        }

        if (persistedOk)
            return TryApplySlideshowFolder(persisted!);
        if (registryOk)
            return TryApplySlideshowFolder(registryFolder!);
        return false;
    }

    private static string? _lastSlideshowFolderPath;
    private static SlideshowAlbumKind _lastSlideshowKind = SlideshowAlbumKind.None;

    private static void RememberSlideshowFolder(string folderPath)
    {
        if (!TryNormalizeSlideshowFolder(folderPath, out var normalized) || normalized is null)
            return;

        _lastSlideshowFolderPath = normalized;
        _lastSlideshowKind = SlideshowAlbumKind.Folder;
        TryWriteString(
            WallpaperHistoryRegistryPath,
            LastSlideshowFolderValueName,
            _lastSlideshowFolderPath);
        TryWriteString(
            WallpaperHistoryRegistryPath,
            LastSlideshowKindValueName,
            SlideshowKindFolder);
        // Синхронизировать ImagesRootPath с live Folder (иначе UI/реестр расходятся с COM).
        TryWriteString(DesktopSlideshowRegistryPath, "ImagesRootPath", _lastSlideshowFolderPath);
    }

    private static void RememberWindowsDesktopBackgroundsAlbum()
    {
        var root = DesktopWallpaperCom.GetWindowsDesktopBackgroundsRoot();
        _lastSlideshowFolderPath = root;
        _lastSlideshowKind = SlideshowAlbumKind.WindowsDesktopBackgrounds;
        TryWriteString(WallpaperHistoryRegistryPath, LastSlideshowFolderValueName, root);
        TryWriteString(
            WallpaperHistoryRegistryPath,
            LastSlideshowKindValueName,
            SlideshowKindWindowsDesktopBackgrounds);
        TryWriteString(DesktopSlideshowRegistryPath, "ImagesRootPath", root);
    }

    /// <summary>
    /// Папка слайдшоу должна содержать файлы изображений в корне.
    /// Иначе IDesktopWallpaper.SetSlideshow «успешен», но рабочий стол чёрный
    /// (типичный случай: %SystemRoot%\Web\Wallpaper без картинок, только Theme*).
    /// </summary>
    private static bool TryNormalizeSlideshowFolder(string? folderPath, out string? normalized)
    {
        normalized = null;
        if (string.IsNullOrWhiteSpace(folderPath))
            return false;

        try
        {
            var full = Path.GetFullPath(Environment.ExpandEnvironmentVariables(folderPath.Trim().Trim('"')));
            if (!Directory.Exists(full) || !FolderHasWallpaperImages(full))
                return false;

            normalized = full;
            return true;
        }
        catch
        {
            return false;
        }
    }

    private static bool FolderHasWallpaperImages(string folderPath)
    {
        try
        {
            return Directory.EnumerateFiles(folderPath)
                .Any(static path =>
                {
                    var ext = Path.GetExtension(path);
                    return ext.Equals(".jpg", StringComparison.OrdinalIgnoreCase)
                        || ext.Equals(".jpeg", StringComparison.OrdinalIgnoreCase)
                        || ext.Equals(".png", StringComparison.OrdinalIgnoreCase)
                        || ext.Equals(".bmp", StringComparison.OrdinalIgnoreCase)
                        || ext.Equals(".gif", StringComparison.OrdinalIgnoreCase)
                        || ext.Equals(".jfif", StringComparison.OrdinalIgnoreCase)
                        || ext.Equals(".tif", StringComparison.OrdinalIgnoreCase)
                        || ext.Equals(".tiff", StringComparison.OrdinalIgnoreCase)
                        || ext.Equals(".webp", StringComparison.OrdinalIgnoreCase)
                        || ext.Equals(".heic", StringComparison.OrdinalIgnoreCase);
                });
        }
        catch
        {
            return false;
        }
    }

    private static void RememberSlideshowFolderFromCom()
    {
        if (!DesktopWallpaperCom.TryGetSlideshowAlbum(out var snap))
            return;

        if (snap.Kind == SlideshowAlbumKind.WindowsDesktopBackgrounds)
        {
            RememberWindowsDesktopBackgroundsAlbum();
            return;
        }

        // ThemeA–D не запоминать как альбом — иначе после apply темы UI залипает на ThemeD.
        if (snap.Kind == SlideshowAlbumKind.Folder
            && !string.IsNullOrWhiteSpace(snap.FolderPath)
            && IsWindowsDesktopBackgroundsExcludedFolder(snap.FolderPath))
        {
            return;
        }

        // Live Folder (в т.ч. Nature) — писать GlassLast как Folder, даже если раньше был Windows.
        if (snap.Kind == SlideshowAlbumKind.Folder && !string.IsNullOrWhiteSpace(snap.FolderPath))
        {
            RememberSlideshowFolder(snap.FolderPath);
        }
    }

    private static string? TryReadPersistedSlideshowFolder()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(WallpaperHistoryRegistryPath);
            if (key?.GetValue(LastSlideshowFolderValueName) is not string raw
                || string.IsNullOrWhiteSpace(raw))
            {
                return null;
            }

            return Path.GetFullPath(Environment.ExpandEnvironmentVariables(raw.Trim().Trim('"')));
        }
        catch
        {
            return null;
        }
    }

    private static SlideshowAlbumKind TryReadPersistedSlideshowKind()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(WallpaperHistoryRegistryPath);
            if (key?.GetValue(LastSlideshowKindValueName) is not string raw
                || string.IsNullOrWhiteSpace(raw))
            {
                return SlideshowAlbumKind.None;
            }

            if (raw.Equals(SlideshowKindWindowsDesktopBackgrounds, StringComparison.OrdinalIgnoreCase))
                return SlideshowAlbumKind.WindowsDesktopBackgrounds;
            if (raw.Equals(SlideshowKindFolder, StringComparison.OrdinalIgnoreCase))
                return SlideshowAlbumKind.Folder;
            return SlideshowAlbumKind.None;
        }
        catch
        {
            return SlideshowAlbumKind.None;
        }
    }

    /// <summary>
    /// Fallback из реестра Desktop Slideshow: только ImagesRootPath с картинками в корне.
    /// Item0Path намеренно не используем — родитель файла (часто ThemeA) не совпадает
    /// с последним альбомом IDesktopWallpaper (например Nature).
    /// </summary>
    private static string? TryResolveSlideshowFolderFromDesktopSlideshowRegistry()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(DesktopSlideshowRegistryPath);
            if (key is null)
                return null;

            if (key.GetValue("ImagesRootPath") is string root
                && !string.IsNullOrWhiteSpace(root)
                && TryNormalizeSlideshowFolder(root, out var fromRoot)
                && fromRoot is not null)
            {
                return fromRoot;
            }
        }
        catch
        {
            // ignore
        }

        return null;
    }

    /// <summary>
    /// Item0Path раньше запоминал ThemeA–D как «последнюю» папку, хотя COM-альбом был Nature.
    /// </summary>
    private static bool TryHealInboxThemeSlideshowFolder(ref bool ok, ref string? folder)
    {
        if (!ok || folder is null || !IsInboxWebWallpaperThemeFolder(folder))
            return false;

        var nature = Path.Combine(
            Environment.GetEnvironmentVariable("SystemRoot") ?? @"C:\Windows",
            "Web",
            "Wallpaper",
            "Nature");
        if (!TryNormalizeSlideshowFolder(nature, out var healed) || healed is null)
            return false;

        folder = healed;
        ok = true;
        RememberSlideshowFolder(healed);
        return true;
    }

    private static bool IsInboxWebWallpaperThemeFolder(string folderPath) =>
        IsWindowsDesktopBackgroundsExcludedFolder(folderPath);

    /// <summary>
    /// ThemeA–D не входят в альбом «Фоны рабочего стола Windows» (и не в combo подпапок).
    /// </summary>
    public static bool IsWindowsDesktopBackgroundsExcludedFolder(string folderPath)
    {
        try
        {
            var full = Path.GetFullPath(folderPath);
            var webRoot = DesktopWallpaperCom.GetWindowsDesktopBackgroundsRoot();
            var prefix = webRoot.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)
                + Path.DirectorySeparatorChar;
            if (!full.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
                return false;

            var name = Path.GetFileName(full.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar));
            return name.Equals("ThemeA", StringComparison.OrdinalIgnoreCase)
                || name.Equals("ThemeB", StringComparison.OrdinalIgnoreCase)
                || name.Equals("ThemeC", StringComparison.OrdinalIgnoreCase)
                || name.Equals("ThemeD", StringComparison.OrdinalIgnoreCase);
        }
        catch
        {
            return false;
        }
    }

    /// <summary>Файл лежит в исключённой Theme*-подпапке Web\Wallpaper.</summary>
    public static bool IsUnderWindowsDesktopBackgroundsExcludedFolder(string filePath)
    {
        try
        {
            var dir = Path.GetDirectoryName(Path.GetFullPath(filePath));
            return !string.IsNullOrWhiteSpace(dir) && IsWindowsDesktopBackgroundsExcludedFolder(dir);
        }
        catch
        {
            return false;
        }
    }

    /// <summary>Переход слайдшоу → фото: первая доступная history-обоина.</summary>
    public static bool TrySwitchToPictureFromHistory()
    {
        foreach (var path in GetRecentHistoryPaths())
        {
            if (TryApplyPicture(path))
                return true;
        }

        var current = CurrentWallpaperResolver.TryGetDesktopWallpaperPath();
        if (!string.IsNullOrWhiteSpace(current) && File.Exists(current))
            return TryApplyPicture(current);

        return false;
    }

    /// <summary>
    /// После apply .theme с одной обоиной Windows часто оставляет BackgroundType=Slideshow
    /// и альбом ThemeA–D — нормализуем в Фото только при ≤1 картинке.
    /// ThemeA–D с несколькими файлами (Восход/ThemeC и т.п.) — легитимное слайдшоу
    /// как в системных Параметрах; не трогаем.
    /// Также: любой live-альбом с ≤1 картинкой.
    /// </summary>
    public static bool TryNormalizeDegenerateSlideshowToPicture()
    {
        if (!TryGetBackgroundType(out var type)
            || type != CurrentWallpaperResolver.BackgroundTypeSlideshow)
        {
            return false;
        }

        if (!TryResolveDegenerateSlideshowPicturePath(out var path))
            return false;

        return TryApplyPicture(path);
    }

    private static bool TryResolveDegenerateSlideshowPicturePath(out string path)
    {
        path = string.Empty;

        var desktop = CurrentWallpaperResolver.TryGetDesktopWallpaperPath();
        var degenerate = false;
        string? preferred = null;

        if (DesktopWallpaperCom.TryGetSlideshowAlbum(out var snap)
            && snap.Kind != SlideshowAlbumKind.None)
        {
            // Полный Windows-альбом не трогаем.
            if (snap.Kind == SlideshowAlbumKind.WindowsDesktopBackgrounds)
                return false;

            if (snap.Kind == SlideshowAlbumKind.Folder
                && !string.IsNullOrWhiteSpace(snap.FolderPath)
                && IsWindowsDesktopBackgroundsExcludedFolder(snap.FolderPath))
            {
                // ThemeA–D: heal только вырожденный альбом (≤1), не «Восход» с 4 кадрами.
                var themeCount = CurrentWallpaperResolver.GetLiveSlideshowImageCount();
                if (themeCount <= 1)
                {
                    degenerate = true;
                    preferred = snap.ImagePaths.FirstOrDefault(File.Exists);
                }
            }
            else if (snap.IsFileSubset)
            {
                var existing = snap.ImagePaths.Where(File.Exists).ToList();
                if (existing.Count <= 1)
                {
                    degenerate = true;
                    if (existing.Count == 1)
                        preferred = existing[0];
                }
            }
            else if (CurrentWallpaperResolver.GetLiveSlideshowImageCount() <= 1)
            {
                degenerate = true;
                preferred = snap.ImagePaths.FirstOrDefault(File.Exists);
            }
        }
        else if (TryNormalizeSlideshowFolder(TryReadImagesRootPath(), out var imagesRoot)
                 && imagesRoot is not null
                 && IsWindowsDesktopBackgroundsExcludedFolder(imagesRoot)
                 && CurrentWallpaperResolver.GetLiveSlideshowImageCount() <= 1)
        {
            degenerate = true;
        }
        else if (CurrentWallpaperResolver.GetLiveSlideshowImageCount() <= 1)
        {
            // BackgroundType=2, но картинок фактически 0–1.
            degenerate = true;
        }

        if (!degenerate)
            return false;

        if (!string.IsNullOrWhiteSpace(preferred) && File.Exists(preferred))
        {
            path = preferred;
            return true;
        }

        if (!string.IsNullOrWhiteSpace(desktop) && File.Exists(desktop))
        {
            path = desktop;
            return true;
        }

        foreach (var history in GetRecentHistoryPaths())
        {
            if (File.Exists(history))
            {
                path = history;
                return true;
            }
        }

        return false;
    }

    private static string? TryReadImagesRootPath()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(DesktopSlideshowRegistryPath);
            if (key?.GetValue("ImagesRootPath") is not string raw || string.IsNullOrWhiteSpace(raw))
                return null;

            return Path.GetFullPath(Environment.ExpandEnvironmentVariables(raw.Trim().Trim('"')));
        }
        catch
        {
            return null;
        }
    }

    public static void SetBackgroundType(int backgroundType)
    {
        if (backgroundType is < CurrentWallpaperResolver.BackgroundTypePicture
            or > CurrentWallpaperResolver.BackgroundTypeSpotlight)
        {
            return;
        }

        TryWriteDword(WallpaperHistoryRegistryPath, BackgroundTypeValueName, backgroundType);
    }

    public static WallpaperFit GetWallpaperFit()
    {
        var style = TryReadDesktopString(WallpaperStyleValueName) ?? "10";
        var tile = TryReadDesktopString(TileWallpaperValueName) ?? "0";

        if (string.Equals(tile, "1", StringComparison.Ordinal)
            && (style == "0" || string.IsNullOrWhiteSpace(style)))
        {
            return WallpaperFit.Tile;
        }

        return style switch
        {
            "6" => WallpaperFit.Fit,
            "2" => WallpaperFit.Stretch,
            "0" => WallpaperFit.Center,
            "22" => WallpaperFit.Span,
            _ => WallpaperFit.Fill
        };
    }

    public static void SetWallpaperFit(WallpaperFit fit)
    {
        var (style, tile) = fit switch
        {
            WallpaperFit.Fit => ("6", "0"),
            WallpaperFit.Stretch => ("2", "0"),
            WallpaperFit.Tile => ("0", "1"),
            WallpaperFit.Center => ("0", "0"),
            WallpaperFit.Span => ("22", "0"),
            _ => ("10", "0")
        };

        TryWriteDesktopString(WallpaperStyleValueName, style);
        TryWriteDesktopString(TileWallpaperValueName, tile);

        var path = CurrentWallpaperResolver.TryGetDesktopWallpaperPath();
        if (!string.IsNullOrWhiteSpace(path) && File.Exists(path))
            ApplySpiWallpaper(path);
    }

    /// <summary>До 5 путей из BackgroundHistoryPath0..4 (существующие файлы).</summary>
    public static IReadOnlyList<string> GetRecentHistoryPaths()
    {
        var list = new List<string>(RecentHistoryCount);
        for (var i = 0; i < RecentHistoryCount; i++)
        {
            if (!TryReadHistoryPath(i, out var path))
                continue;

            if (list.Exists(p => string.Equals(p, path, StringComparison.OrdinalIgnoreCase)))
                continue;

            list.Add(path);
        }

        return list;
    }

    public static bool TryApplyPicture(string imagePath)
    {
        if (string.IsNullOrWhiteSpace(imagePath))
            return false;

        string fullPath;
        try
        {
            fullPath = Path.GetFullPath(Environment.ExpandEnvironmentVariables(imagePath.Trim().Trim('"')));
        }
        catch
        {
            return false;
        }

        if (!File.Exists(fullPath))
            return false;

        // IDesktopWallpaper.SetWallpaper выключает слайдшоу и ставит статичную обоину.
        if (!DesktopWallpaperCom.TrySetWallpaper(fullPath))
        {
            TryWriteDesktopString(WallpaperValueName, fullPath);
            if (!ApplySpiWallpaper(fullPath))
                return false;
        }

        TryWriteDword(
            WallpaperHistoryRegistryPath,
            BackgroundTypeValueName,
            CurrentWallpaperResolver.BackgroundTypePicture);
        PushHistoryPath(fullPath);
        return true;
    }

    public static bool TryPickImageFile(Window window, out string fullPath)
    {
        fullPath = string.Empty;
        if (window is null)
            return false;

        var owner = WindowNative.GetWindowHandle(window);
        var buffer = new char[260];
        if (!NativeHostInterop.TryPickImageFile(owner, buffer))
            return false;

        fullPath = new string(buffer).TrimEnd('\0');
        return !string.IsNullOrWhiteSpace(fullPath);
    }

    public static bool TryPickFolder(Window window, out string fullPath)
    {
        fullPath = string.Empty;
        if (window is null)
            return false;

        return NativeHostInterop.TryPickFolder(window, out fullPath);
    }

    public static bool TryGetSlideshowFolder(out string path, out string displayName) =>
        TryGetSlideshowAlbum(out path, out displayName, out _);

    public static bool TryGetSlideshowAlbum(
        out string path,
        out string displayName,
        out SlideshowAlbumKind kind)
    {
        path = string.Empty;
        displayName = string.Empty;
        kind = SlideshowAlbumKind.None;

        if (DesktopWallpaperCom.TryGetSlideshowAlbum(out var live) && live.Kind != SlideshowAlbumKind.None)
        {
            // UI = live COM. Не подменять Folder(Nature) на Windows из-за GlassLast —
            // иначе при старте combo всегда «Фоны Windows».
            // Read-only: Remember* — только из apply / ApplyModeChoice, не из sync UI.
            path = live.FolderPath;
            displayName = live.DisplayName;
            kind = live.Kind;
            return true;
        }

        var persistedKind = TryReadPersistedSlideshowKind();
        if (persistedKind == SlideshowAlbumKind.WindowsDesktopBackgrounds
            || _lastSlideshowKind == SlideshowAlbumKind.WindowsDesktopBackgrounds)
        {
            path = DesktopWallpaperCom.GetWindowsDesktopBackgroundsRoot();
            displayName = ResolveWindowsDesktopBackgroundsDisplayName();
            kind = SlideshowAlbumKind.WindowsDesktopBackgrounds;
            return Directory.Exists(path);
        }

        // COM пуст в режиме Picture — для UI/restore показываем последнюю запомненную папку.
        if (TryNormalizeSlideshowFolder(TryReadPersistedSlideshowFolder(), out var persisted)
            && persisted is not null)
        {
            path = persisted;
            displayName = ResolvePersistedFolderDisplayName(persisted);
            kind = SlideshowAlbumKind.Folder;
            return true;
        }

        if (TryNormalizeSlideshowFolder(_lastSlideshowFolderPath, out var cache) && cache is not null)
        {
            path = cache;
            displayName = ResolvePersistedFolderDisplayName(cache);
            kind = SlideshowAlbumKind.Folder;
            return true;
        }

        return false;
    }

    private static string ResolvePersistedFolderDisplayName(string folderPath)
    {
        if (DesktopWallpaperCom.TryGetShellFolderDisplayName(folderPath, out var shellName)
            && !string.IsNullOrWhiteSpace(shellName))
        {
            return shellName;
        }

        var name = Path.GetFileName(
            folderPath.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar));
        return string.IsNullOrWhiteSpace(name) ? folderPath : name;
    }

    /// <summary>Синхронизировать GlassLastSlideshowFolder с живым COM-альбомом (если есть).</summary>
    public static void SyncRememberedSlideshowFolderFromCom()
    {
        RememberSlideshowFolderFromCom();
    }

    public static string ResolveWindowsDesktopBackgroundsDisplayName()
    {
        try
        {
            var ui = System.Globalization.CultureInfo.CurrentUICulture.TwoLetterISOLanguageName;
            if (ui.Equals("ru", StringComparison.OrdinalIgnoreCase))
                return "Фоны рабочего стола Windows";
        }
        catch
        {
            // ignore
        }

        return "Windows Desktop Backgrounds";
    }

    public static bool TryApplyWindowsDesktopBackgroundsAlbum()
    {
        var intervalMs = TryReadDword(DesktopSlideshowRegistryPath, "Interval", out var intervalRaw)
            && intervalRaw > 0
            ? (uint)intervalRaw
            : 600_000u;
        var shuffle = TryReadDword(DesktopSlideshowRegistryPath, "Shuffle", out var shuffleRaw)
            && shuffleRaw != 0;

        var displayName = ResolveWindowsDesktopBackgroundsDisplayName();
        // Эталон 0950: только search-folder Set (без FILESYS-subset / Verify).
        // Set уже применил search-folder. Get на virtual PIDL часто NotImplemented —
        // не abort: иначе UI/ImagesRootPath остаются Nature при живом Windows на столе.
        if (!DesktopWallpaperCom.TrySetWindowsDesktopBackgroundsSlideshow(displayName))
            return false;

        var root = DesktopWallpaperCom.GetWindowsDesktopBackgroundsRoot();
        // Identity search-folder / Windows album — ImagesRootPath = Web\Wallpaper.
        TryWriteString(DesktopSlideshowRegistryPath, "ImagesRootPath", root);

        DesktopWallpaperCom.SetSlideshowOptions(shuffle, intervalMs);
        TryWriteDword(DesktopSlideshowRegistryPath, "Shuffle", shuffle ? 1 : 0);
        TryWriteDword(DesktopSlideshowRegistryPath, "Interval", (int)intervalMs);

        RememberWindowsDesktopBackgroundsAlbum();
        TryWriteDword(
            WallpaperHistoryRegistryPath,
            BackgroundTypeValueName,
            CurrentWallpaperResolver.BackgroundTypeSlideshow);
        TryWriteDword(WallpaperHistoryRegistryPath, "SlideshowSourceDirectoriesSet", 1);

        // Как Win32: не трогать Desktop\Wallpaper (TranscodedWallpaper) и не двигать History0 —
        // иначе ранний WallpaperChanged до обновления Transcoded → два crossfade.

        return true;
    }

    public static bool TryApplySlideshowFolder(string folderPath)
    {
        if (!TryNormalizeSlideshowFolder(folderPath, out var normalized) || normalized is null)
            return false;

        // Options до SetSlideshow — после COM иногда отдаёт мусор.
        var intervalMs = TryReadDword(DesktopSlideshowRegistryPath, "Interval", out var intervalRaw)
            && intervalRaw > 0
            ? (uint)intervalRaw
            : 600_000u;
        var shuffle = TryReadDword(DesktopSlideshowRegistryPath, "Shuffle", out var shuffleRaw)
            && shuffleRaw != 0;

        if (!DesktopWallpaperCom.TrySetSlideshowFolder(normalized))
            return false;

        // Синхронизировать ImagesRootPath с реальной папкой (иначе UI/реестр расходятся с альбомом).
        TryWriteString(DesktopSlideshowRegistryPath, "ImagesRootPath", normalized);

        DesktopWallpaperCom.SetSlideshowOptions(shuffle, intervalMs);
        TryWriteDword(DesktopSlideshowRegistryPath, "Shuffle", shuffle ? 1 : 0);
        TryWriteDword(DesktopSlideshowRegistryPath, "Interval", (int)intervalMs);

        RememberSlideshowFolder(normalized);
        TryWriteDword(
            WallpaperHistoryRegistryPath,
            BackgroundTypeValueName,
            CurrentWallpaperResolver.BackgroundTypeSlideshow);
        TryWriteDword(WallpaperHistoryRegistryPath, "SlideshowSourceDirectoriesSet", 1);

        // Не писать Desktop\Wallpaper / History0: иначе fingerprint меняется до Transcoded
        // → два crossfade подряд. Превью только по LastTick/Transcoded от shell.
        return true;
    }

    /// <summary>
    /// Подмножество картинок альбома. Included-set = COM GetSlideshow, не ItemNPath.
    /// </summary>
    public static bool TryApplySlideshowImageSubset(
        IReadOnlyList<string> imagePaths,
        SlideshowAlbumKind albumKind,
        string? albumDisplayName = null)
    {
        if (imagePaths is null || imagePaths.Count == 0)
            return false;

        // Полный выбор Windows-альбома — только search-folder (не 51 search-ms PIDL → чёрный стол).
        if (albumKind == SlideshowAlbumKind.WindowsDesktopBackgrounds
            && IsFullWindowsAlbumSelection(imagePaths))
        {
            return TryApplyWindowsDesktopBackgroundsAlbum();
        }

        var intervalMs = TryReadDword(DesktopSlideshowRegistryPath, "Interval", out var intervalRaw)
            && intervalRaw > 0
            ? (uint)intervalRaw
            : 600_000u;
        var shuffle = TryReadDword(DesktopSlideshowRegistryPath, "Shuffle", out var shuffleRaw)
            && shuffleRaw != 0;

        var display = albumDisplayName;
        if (albumKind == SlideshowAlbumKind.WindowsDesktopBackgrounds
            && string.IsNullOrWhiteSpace(display))
        {
            display = ResolveWindowsDesktopBackgroundsDisplayName();
        }

        if (!DesktopWallpaperCom.TrySetSlideshowImages(imagePaths, albumKind, display))
            return false;

        DesktopWallpaperCom.SetSlideshowOptions(shuffle, intervalMs);
        TryWriteDword(DesktopSlideshowRegistryPath, "Shuffle", shuffle ? 1 : 0);
        TryWriteDword(DesktopSlideshowRegistryPath, "Interval", (int)intervalMs);

        if (albumKind == SlideshowAlbumKind.WindowsDesktopBackgrounds)
            RememberWindowsDesktopBackgroundsAlbum();
        else if (TryNormalizeSlideshowFolder(
                     Path.GetDirectoryName(imagePaths[0]),
                     out var folder)
                 && folder is not null)
        {
            RememberSlideshowFolder(folder);
            TryWriteString(DesktopSlideshowRegistryPath, "ImagesRootPath", folder);
        }

        TryWriteDword(
            WallpaperHistoryRegistryPath,
            BackgroundTypeValueName,
            CurrentWallpaperResolver.BackgroundTypeSlideshow);
        TryWriteDword(WallpaperHistoryRegistryPath, "SlideshowSourceDirectoriesSet", 1);

        var first = imagePaths.FirstOrDefault(File.Exists);
        if (!string.IsNullOrWhiteSpace(first))
            TryWriteDesktopString(WallpaperValueName, first);

        return true;
    }

    /// <summary>
    /// Все подпапки %SystemRoot%\Web\Wallpaper для combo (как в Проводнике).
    /// Имя — shell LocalizedResourceName (Восход/Свечение…), путь — FS (ThemeC/ThemeA…).
    /// </summary>
    public static IReadOnlyList<(string Path, string Name)> EnumerateWindowsWallpaperSubfolders()
    {
        var root = DesktopWallpaperCom.GetWindowsDesktopBackgroundsRoot();
        if (!Directory.Exists(root))
            return Array.Empty<(string, string)>();

        try
        {
            return Directory.EnumerateDirectories(root)
                .Select(static dir =>
                {
                    var full = Path.GetFullPath(dir);
                    var name = DesktopWallpaperCom.TryGetShellFolderDisplayName(full, out var shellName)
                        && !string.IsNullOrWhiteSpace(shellName)
                        ? shellName
                        : Path.GetFileName(
                            full.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar));
                    return (full, name);
                })
                .OrderBy(static t => t.name, StringComparer.CurrentCultureIgnoreCase)
                .ToList();
        }
        catch
        {
            return Array.Empty<(string, string)>();
        }
    }

    /// <summary>
    /// Изображения альбома «Фоны…»: все подпапки Web\Wallpaper (как пункты combo).
    /// </summary>
    public static IReadOnlyList<string> EnumerateWindowsDesktopBackgroundsAlbumImages()
    {
        var list = new List<string>();
        foreach (var (folderPath, _) in EnumerateWindowsWallpaperSubfolders())
            list.AddRange(EnumerateSlideshowAlbumImages(folderPath, recursive: true));

        list.Sort(StringComparer.OrdinalIgnoreCase);
        return list;
    }

    private static bool IsFullWindowsAlbumSelection(IReadOnlyList<string> selected)
    {
        var all = EnumerateWindowsDesktopBackgroundsAlbumImages();
        if (all.Count == 0 || selected.Count != all.Count)
            return false;

        var set = new HashSet<string>(
            selected.Where(File.Exists).Select(Path.GetFullPath),
            StringComparer.OrdinalIgnoreCase);
        if (set.Count != all.Count)
            return false;

        foreach (var path in all)
        {
            if (!set.Contains(Path.GetFullPath(path)))
                return false;
        }

        return true;
    }

    /// <summary>Снимок слайдшоу для отмены правок в album-picker (Dismiss без Save).</summary>
    public readonly struct SlideshowEditSnapshot
    {
        public SlideshowEditSnapshot(
            SlideshowAlbumKind kind,
            string folderPath,
            string displayName,
            IReadOnlyList<string> imagePaths,
            bool isFileSubset,
            bool shuffle,
            uint intervalMs)
        {
            Kind = kind;
            FolderPath = folderPath;
            DisplayName = displayName;
            ImagePaths = imagePaths;
            IsFileSubset = isFileSubset;
            Shuffle = shuffle;
            IntervalMs = intervalMs;
        }

        public SlideshowAlbumKind Kind { get; }
        public string FolderPath { get; }
        public string DisplayName { get; }
        public IReadOnlyList<string> ImagePaths { get; }
        public bool IsFileSubset { get; }
        public bool Shuffle { get; }
        public uint IntervalMs { get; }
    }

    public static bool TryCaptureSlideshowEditSnapshot(out SlideshowEditSnapshot snapshot)
    {
        snapshot = default;
        if (!DesktopWallpaperCom.TryGetSlideshowAlbum(out var live) || live.Kind == SlideshowAlbumKind.None)
            return false;

        _ = TryGetSlideshowShuffle(out var shuffle);
        _ = TryGetSlideshowIntervalMs(out var intervalMs);

        snapshot = new SlideshowEditSnapshot(
            live.Kind,
            live.FolderPath,
            live.DisplayName,
            live.ImagePaths.ToList(),
            live.IsFileSubset,
            shuffle,
            intervalMs);
        return true;
    }

    public static bool TryRestoreSlideshowEditSnapshot(in SlideshowEditSnapshot snapshot)
    {
        if (snapshot.Kind == SlideshowAlbumKind.None)
            return false;

        if (snapshot.IsFileSubset && snapshot.ImagePaths.Count > 0)
        {
            if (!TryApplySlideshowImageSubset(snapshot.ImagePaths, snapshot.Kind, snapshot.DisplayName))
                return false;
        }
        else if (snapshot.Kind == SlideshowAlbumKind.WindowsDesktopBackgrounds)
        {
            if (!TryApplyWindowsDesktopBackgroundsAlbum())
                return false;
        }
        else if (snapshot.Kind == SlideshowAlbumKind.Folder
                 && !string.IsNullOrWhiteSpace(snapshot.FolderPath))
        {
            if (!TryApplySlideshowFolder(snapshot.FolderPath))
                return false;
        }
        else
        {
            return false;
        }

        SetSlideshowShuffle(snapshot.Shuffle);
        SetSlideshowIntervalMs(snapshot.IntervalMs);
        return true;
    }

    /// <summary>
    /// Кандидаты UI. Windows-альбом — рекурсивно; обычная папка — только корень (одна группа).
    /// </summary>
    public static IReadOnlyList<string> EnumerateSlideshowAlbumImages(string albumRoot, bool recursive)
    {
        if (string.IsNullOrWhiteSpace(albumRoot) || !Directory.Exists(albumRoot))
            return Array.Empty<string>();

        try
        {
            var option = recursive ? SearchOption.AllDirectories : SearchOption.TopDirectoryOnly;
            return Directory.EnumerateFiles(albumRoot, "*", option)
                .Where(IsWallpaperImagePath)
                .OrderBy(static p => p, StringComparer.OrdinalIgnoreCase)
                .ToList();
        }
        catch
        {
            return Array.Empty<string>();
        }
    }

    /// <summary>
    /// Included из live GetSlideshow. null = COM не дал file subset («все» только в Build через ??).
    /// SyncInclusion не трогает UI при null — иначе откат галочек после Windows Set.
    /// ItemNPath намеренно не читаем.
    /// </summary>
    public static HashSet<string>? TryGetSlideshowIncludedImagePaths(
        SlideshowAlbumKind kind,
        IReadOnlyList<string> candidates)
    {
        var set = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        if (!DesktopWallpaperCom.TryGetSlideshowAlbum(out var snap))
            return null;

        // Полный контейнер (search-ms / папка без FILESYS-списка) — subset COM не выражает.
        if (!snap.IsFileSubset || snap.ImagePaths.Count == 0)
            return null;

        foreach (var p in snap.ImagePaths)
        {
            if (File.Exists(p))
                set.Add(Path.GetFullPath(p));
        }

        return set.Count > 0 ? set : null;
    }

    private static bool IsWallpaperImagePath(string path)
    {
        var ext = Path.GetExtension(path);
        return ext.Equals(".jpg", StringComparison.OrdinalIgnoreCase)
            || ext.Equals(".jpeg", StringComparison.OrdinalIgnoreCase)
            || ext.Equals(".png", StringComparison.OrdinalIgnoreCase)
            || ext.Equals(".bmp", StringComparison.OrdinalIgnoreCase)
            || ext.Equals(".gif", StringComparison.OrdinalIgnoreCase)
            || ext.Equals(".jfif", StringComparison.OrdinalIgnoreCase)
            || ext.Equals(".tif", StringComparison.OrdinalIgnoreCase)
            || ext.Equals(".tiff", StringComparison.OrdinalIgnoreCase)
            || ext.Equals(".webp", StringComparison.OrdinalIgnoreCase)
            || ext.Equals(".heic", StringComparison.OrdinalIgnoreCase);
    }

    private static string? TryFindFirstWallpaperFileRecursive(string folderPath)
    {
        try
        {
            return Directory.EnumerateFiles(folderPath, "*", SearchOption.AllDirectories)
                .Where(IsWallpaperImagePath)
                .OrderBy(static path => path, StringComparer.OrdinalIgnoreCase)
                .FirstOrDefault();
        }
        catch
        {
            return null;
        }
    }

    private static string? TryFindFirstWallpaperFile(string folderPath)
    {
        try
        {
            return Directory.EnumerateFiles(folderPath)
                .Where(static path =>
                {
                    var ext = Path.GetExtension(path);
                    return ext.Equals(".jpg", StringComparison.OrdinalIgnoreCase)
                        || ext.Equals(".jpeg", StringComparison.OrdinalIgnoreCase)
                        || ext.Equals(".png", StringComparison.OrdinalIgnoreCase)
                        || ext.Equals(".bmp", StringComparison.OrdinalIgnoreCase)
                        || ext.Equals(".jfif", StringComparison.OrdinalIgnoreCase);
                })
                .OrderBy(static path => path, StringComparer.OrdinalIgnoreCase)
                .FirstOrDefault();
        }
        catch
        {
            return null;
        }
    }

    public static bool TryGetSlideshowShuffle(out bool shuffle)
    {
        if (DesktopWallpaperCom.TryGetSlideshowOptions(out shuffle, out _))
            return true;

        shuffle = TryReadDword(DesktopSlideshowRegistryPath, "Shuffle", out var value) && value != 0;
        return true;
    }

    public static void SetSlideshowShuffle(bool shuffle)
    {
        var intervalMs = TryGetSlideshowIntervalMs(out var ms) ? ms : 600_000u;
        DesktopWallpaperCom.SetSlideshowOptions(shuffle, intervalMs);
        TryWriteDword(DesktopSlideshowRegistryPath, "Shuffle", shuffle ? 1 : 0);
    }

    public static bool TryGetSlideshowIntervalMs(out uint intervalMs)
    {
        if (DesktopWallpaperCom.TryGetSlideshowOptions(out _, out intervalMs))
            return true;

        if (TryReadDword(DesktopSlideshowRegistryPath, "Interval", out var value) && value > 0)
        {
            intervalMs = (uint)value;
            return true;
        }

        intervalMs = 600_000;
        return false;
    }

    public static void SetSlideshowIntervalMs(uint intervalMs)
    {
        if (intervalMs == 0)
            intervalMs = 600_000;

        var shuffle = TryGetSlideshowShuffle(out var s) && s;
        DesktopWallpaperCom.SetSlideshowOptions(shuffle, intervalMs);
        TryWriteDword(DesktopSlideshowRegistryPath, "Interval", (int)intervalMs);
    }

    public static int FindSlideshowIntervalIndex(uint intervalMs)
    {
        var best = 1;
        var bestDelta = uint.MaxValue;
        for (var i = 0; i < SlideshowIntervalMs.Length; i++)
        {
            var delta = intervalMs > SlideshowIntervalMs[i]
                ? intervalMs - SlideshowIntervalMs[i]
                : SlideshowIntervalMs[i] - intervalMs;
            if (delta < bestDelta)
            {
                bestDelta = delta;
                best = i;
            }
        }

        return best;
    }

    /// <summary>Разрешить слайдшоу на батарее (DC power setting Available=0 / Paused=1).</summary>
    public static bool GetAllowSlideshowOnBattery()
    {
        try
        {
            if (PowerGetActiveScheme(IntPtr.Zero, out var schemePtr) != 0 || schemePtr == IntPtr.Zero)
                return false;

            try
            {
                var scheme = Marshal.PtrToStructure<Guid>(schemePtr);
                var subgroup = SlideshowPowerSubgroupGuid;
                var setting = SlideshowPowerSettingGuid;
                if (PowerReadDCValueIndex(IntPtr.Zero, ref scheme, ref subgroup, ref setting, out var value) != 0)
                    return false;

                // 0 = Available (вкл), 1 = Paused (выкл на батарее).
                return value == 0;
            }
            finally
            {
                LocalFree(schemePtr);
            }
        }
        catch
        {
            return false;
        }
    }

    public static void SetAllowSlideshowOnBattery(bool allow)
    {
        try
        {
            if (PowerGetActiveScheme(IntPtr.Zero, out var schemePtr) != 0 || schemePtr == IntPtr.Zero)
                return;

            try
            {
                var scheme = Marshal.PtrToStructure<Guid>(schemePtr);
                var subgroup = SlideshowPowerSubgroupGuid;
                var setting = SlideshowPowerSettingGuid;
                var value = allow ? 0u : 1u;
                _ = PowerWriteDCValueIndex(IntPtr.Zero, ref scheme, ref subgroup, ref setting, value);
                _ = PowerSetActiveScheme(IntPtr.Zero, ref scheme);
            }
            finally
            {
                LocalFree(schemePtr);
            }
        }
        catch
        {
            // ignore
        }
    }

    private static readonly Guid SlideshowPowerSubgroupGuid =
        new("0d7dbae2-4294-402a-ba8e-26777e8488cd");
    private static readonly Guid SlideshowPowerSettingGuid =
        new("309dce9b-bef4-4119-9921-a851fb12f0f4");

    private static bool TryReadDword(string subKeyPath, string valueName, out int value)
    {
        value = 0;
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(subKeyPath);
            switch (key?.GetValue(valueName))
            {
                case int i:
                    value = i;
                    return true;
                case long l:
                    value = (int)l;
                    return true;
                default:
                    return false;
            }
        }
        catch
        {
            return false;
        }
    }

    private static void PushHistoryPath(string fullPath)
    {
        var existing = new List<string>(RecentHistoryCount);
        for (var i = 0; i < RecentHistoryCount; i++)
        {
            if (TryReadHistoryPathRaw(i, out var raw) && !string.IsNullOrWhiteSpace(raw))
                existing.Add(raw);
        }

        existing.RemoveAll(p =>
            string.Equals(
                Environment.ExpandEnvironmentVariables(p.Trim().Trim('"')),
                fullPath,
                StringComparison.OrdinalIgnoreCase));

        existing.Insert(0, fullPath);
        if (existing.Count > RecentHistoryCount)
            existing.RemoveRange(RecentHistoryCount, existing.Count - RecentHistoryCount);

        try
        {
            using var key = Registry.CurrentUser.CreateSubKey(WallpaperHistoryRegistryPath);
            if (key is null)
                return;

            for (var i = 0; i < RecentHistoryCount; i++)
            {
                var name = "BackgroundHistoryPath" + i;
                if (i < existing.Count)
                    key.SetValue(name, existing[i], RegistryValueKind.String);
                else if (key.GetValue(name) is not null)
                    key.DeleteValue(name, throwOnMissingValue: false);
            }
        }
        catch
        {
            // ignore
        }
    }

    private static bool ApplySpiWallpaper(string path)
    {
        try
        {
            return SystemParametersInfoW(
                SpiSetDeskWallpaper,
                0,
                path,
                SpifUpdateIniFile | SpifSendChange);
        }
        catch
        {
            return false;
        }
    }

    private static bool TryReadHistoryPath(int index, out string path)
    {
        path = string.Empty;
        if (!TryReadHistoryPathRaw(index, out var raw) || string.IsNullOrWhiteSpace(raw))
            return false;

        try
        {
            var expanded = Path.GetFullPath(Environment.ExpandEnvironmentVariables(raw.Trim().Trim('"')));
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

    private static bool TryReadHistoryPathRaw(int index, out string raw)
    {
        raw = string.Empty;
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(WallpaperHistoryRegistryPath);
            if (key?.GetValue("BackgroundHistoryPath" + index) is not string value
                || string.IsNullOrWhiteSpace(value))
            {
                return false;
            }

            raw = value;
            return true;
        }
        catch
        {
            return false;
        }
    }

    private static string? TryReadDesktopString(string valueName)
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(DesktopRegistryPath);
            return key?.GetValue(valueName) as string;
        }
        catch
        {
            return null;
        }
    }

    private static void TryWriteDesktopString(string valueName, string value)
    {
        try
        {
            using var key = Registry.CurrentUser.CreateSubKey(DesktopRegistryPath);
            key?.SetValue(valueName, value, RegistryValueKind.String);
        }
        catch
        {
            // ignore
        }
    }

    private static void TryWriteString(string subKeyPath, string valueName, string value)
    {
        try
        {
            using var key = Registry.CurrentUser.CreateSubKey(subKeyPath);
            key?.SetValue(valueName, value, RegistryValueKind.String);
        }
        catch
        {
            // ignore
        }
    }

    private static void TryWriteDword(string subKeyPath, string valueName, int value)
    {
        try
        {
            using var key = Registry.CurrentUser.CreateSubKey(subKeyPath);
            key?.SetValue(valueName, value, RegistryValueKind.DWord);
        }
        catch
        {
            // ignore
        }
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool SystemParametersInfoW(
        uint uiAction,
        uint uiParam,
        string pvParam,
        uint fWinIni);

    [DllImport("powrprof.dll")]
    private static extern uint PowerGetActiveScheme(IntPtr userRootPowerKey, out IntPtr activePolicyGuid);

    [DllImport("powrprof.dll")]
    private static extern uint PowerReadDCValueIndex(
        IntPtr rootPowerKey,
        ref Guid schemeGuid,
        ref Guid subGroupOfPowerSettingsGuid,
        ref Guid powerSettingGuid,
        out uint acValueIndex);

    [DllImport("powrprof.dll")]
    private static extern uint PowerWriteDCValueIndex(
        IntPtr rootPowerKey,
        ref Guid schemeGuid,
        ref Guid subGroupOfPowerSettingsGuid,
        ref Guid powerSettingGuid,
        uint acValueIndex);

    [DllImport("powrprof.dll")]
    private static extern uint PowerSetActiveScheme(IntPtr userRootPowerKey, ref Guid schemeGuid);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr LocalFree(IntPtr hMem);
}
