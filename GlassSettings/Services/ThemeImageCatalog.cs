namespace GlassSettings.Services;

using System.Runtime.InteropServices;
using System.Runtime.InteropServices.WindowsRuntime;
using GlassSettings.Core.Platform;
using Windows.Storage.Streams;

public sealed class ThemeImageItem
{
    public required string Title { get; init; }
    public required string FilePath { get; init; }
    public Microsoft.UI.Xaml.Media.ImageSource? Image { get; init; }
}

public static class ThemeImageCatalog
{
    private const string NativeDllName = "GlassSettings.Native.dll";

    private static readonly string[] ReflectionRelativeFolders =
    [
        Path.Combine("Resources", "GlassReflection"),
        Path.Combine("Resources", "Glass Reflection"),
    ];

    private static readonly string[] MaterialRelativeFolders =
    [
        Path.Combine("Resources", "ThemeMaterial"),
        Path.Combine("Resources", "Theme Material"),
    ];

    private static readonly string[] AtlasRelativeFolders =
    [
        Path.Combine("Resources", "AtlasTheme"),
        Path.Combine("Resources", "Atlas Theme"),
    ];

    public const uint AtlasPreviewWidth = 170;
    public const uint AtlasPreviewHeight = 38;

    /// <summary>Альфа слоя отражения в миниатюрах коллекции атласов (native percent).</summary>
    public const uint CatalogAtlasReflectionAlphaPercent = 100;

    /// <summary>Без слоя отражения в layered-превью.</summary>
    public const uint LayeredAtlasReflectionAlphaPercent = 0;

    /// <summary>Каталог атласов AtlasTheme (первый существующий или создаваемый путь поиска).</summary>
    public static string? TryResolveAtlasCatalogDirectory(bool createIfMissing = true) =>
        TryResolveAtlasCatalogSaveDirectory(createIfMissing);

    /// <summary>
    /// Каталог сохранения/загрузки кастомных атласов — тот же порядок поиска, что LoadFromResourceFolders.
    /// </summary>
    public static string? TryResolveAtlasCatalogSaveDirectory(bool createIfMissing = false)
    {
        foreach (var dir in EnumerateAtlasCatalogDirectoryCandidates())
        {
            if (Directory.Exists(dir) && LoadFromDirectory(dir, includeImage: false).Count > 0)
                return dir;
        }

        if (!createIfMissing)
        {
            foreach (var dir in EnumerateAtlasCatalogDirectoryCandidates())
            {
                if (Directory.Exists(dir))
                    return dir;
            }

            return null;
        }

        foreach (var dir in EnumerateAtlasCatalogDirectoryCandidates())
        {
            try
            {
                Directory.CreateDirectory(dir);
                return dir;
            }
            catch
            {
                // Следующий кандидат.
            }
        }

        return null;
    }

    private static IEnumerable<string> EnumerateAtlasCatalogDirectoryCandidates()
    {
        foreach (var root in BuildSearchRoots())
        {
            foreach (var relative in AtlasRelativeFolders)
                yield return Path.GetFullPath(Path.Combine(root, relative));
        }
    }

    public static IReadOnlyList<ThemeImageItem> LoadReflectionImages() =>
        LoadFromResourceFolders(ReflectionRelativeFolders);

    public static IReadOnlyList<ThemeImageItem> LoadMaterialImages() =>
        LoadFromResourceFolders(MaterialRelativeFolders);

    public static IReadOnlyList<ThemeImageItem> LoadAtlasImages() =>
        LoadAtlasCatalog();

    public static bool TryDeleteReflectionImage(string filePath) =>
        TryDeleteCatalogImage(filePath);

    public static bool TryDeleteMaterialImage(string filePath) =>
        TryDeleteCatalogImage(filePath);

    public static bool TryDeleteAtlasImage(string filePath)
    {
        if (!TryDeleteCatalogImage(filePath))
            return false;

        var layoutPath = ResolveLayoutPath(filePath);
        if (layoutPath is not null)
        {
            try
            {
                File.Delete(layoutPath);
            }
            catch
            {
                // PNG уже удалён — layout best-effort.
            }
        }

        return true;
    }

    private static bool TryDeleteCatalogImage(string filePath)
    {
        if (string.IsNullOrWhiteSpace(filePath))
            return false;

        try
        {
            var fullPath = Path.GetFullPath(filePath);
            if (!File.Exists(fullPath))
                return false;

            File.Delete(fullPath);
            return true;
        }
        catch
        {
            return false;
        }
    }

    /// <summary>Каталог атласов без сырого PNG — preview генерируется асинхронно.</summary>
    public static IReadOnlyList<ThemeImageItem> LoadAtlasCatalog()
    {
        AtlasPreviewTrace.CatalogStart();
        AtlasPreviewTrace.CatalogOpenGlass(
            PlatformPaths.OpenGlassDllPath,
            PlatformPaths.OpenGlassInstallRoot);

        var nativeDllPath = ResolveNativeDllPath();
        AtlasPreviewTrace.CatalogNativeDll(nativeDllPath, nativeDllPath is not null && File.Exists(nativeDllPath));

        return LoadFromResourceFolders(AtlasRelativeFolders, includeImage: false, traceCatalog: true);
    }

    public static IReadOnlyList<ThemeImageItem> LoadFromFolder(string relativeFolder) =>
        LoadFromDirectory(Path.Combine(PlatformPaths.AppBase, relativeFolder));

    /// <summary>Генерация PNG preview в фоновом потоке. Возвращает байты или null.</summary>
    public static byte[]? TryGenerateAtlasPreviewBytes(
        string atlasPath,
        uint? targetWidth = null,
        uint? targetHeight = null,
        uint reflectionAlphaPercent = CatalogAtlasReflectionAlphaPercent)
    {
        var title = Path.GetFileNameWithoutExtension(atlasPath);
        AtlasPreviewTrace.GenerationStart(title, atlasPath);

        try
        {
            if (!File.Exists(atlasPath))
            {
                AtlasPreviewTrace.GenerationFailed(title, "atlas file not found");
                return null;
            }

            var layoutPath = ResolveLayoutPath(atlasPath);
            if (layoutPath is null)
            {
                AtlasPreviewTrace.GenerationFailed(title, "layout sidecar not found");
                return null;
            }

            AtlasPreviewTrace.GenerationStep($"layout={layoutPath}");

            var nativeDllPath = ResolveNativeDllPath();
            if (nativeDllPath is null || !File.Exists(nativeDllPath))
            {
                AtlasPreviewTrace.GenerationFailed(title, $"{NativeDllName} not found near executable");
                return null;
            }

            AtlasPreviewTrace.GenerationStep($"native dll={nativeDllPath}");

            string reflectionDllPath = string.Empty;
            if (reflectionAlphaPercent > 0)
            {
                reflectionDllPath = PlatformPaths.OpenGlassDllPath ?? string.Empty;
                if (string.IsNullOrWhiteSpace(reflectionDllPath) || !File.Exists(reflectionDllPath))
                {
                    AtlasPreviewTrace.GenerationFailed(title, "OpenGlass.dll not found (reflection resource required)");
                    return null;
                }

                AtlasPreviewTrace.GenerationStep($"reflection dll={reflectionDllPath}");
            }

            var tempPath = Path.Combine(Path.GetTempPath(), $"atlas_preview_{Guid.NewGuid()}.png");
            var previewWidth = targetWidth ?? AtlasPreviewWidth;
            var previewHeight = targetHeight ?? AtlasPreviewHeight;

            AtlasPreviewTrace.GenerationStep($"invoke native {previewWidth}x{previewHeight}, output={tempPath}");

            var hr = GlassSettings_GenerateAtlasPreview(
                atlasPath,
                layoutPath,
                tempPath,
                previewWidth,
                previewHeight,
                reflectionDllPath,
                reflectionAlphaPercent);
            AtlasPreviewTrace.GenerationHResult(hr, "GlassSettings_GenerateAtlasPreview");

            if (hr != 0)
            {
                AtlasPreviewTrace.GenerationFailed(title, $"native HRESULT=0x{hr:X8}");
                return null;
            }

            if (!File.Exists(tempPath))
            {
                AtlasPreviewTrace.GenerationFailed(title, "temp PNG not created");
                return null;
            }

            var bytes = File.ReadAllBytes(tempPath);
            AtlasPreviewTrace.GenerationBytes(title, bytes.Length, tempPath);

            try { File.Delete(tempPath); } catch { }

            if (bytes.Length == 0)
            {
                AtlasPreviewTrace.GenerationFailed(title, "temp PNG is empty");
                return null;
            }

            AtlasPreviewTrace.GenerationStep($"complete title={title}");
            return bytes;
        }
        catch (DllNotFoundException ex)
        {
            AtlasPreviewTrace.Exception("DllNotFoundException", ex);
            AtlasPreviewTrace.GenerationFailed(title, NativeDllName + " load failed");
            return null;
        }
        catch (Exception ex)
        {
            AtlasPreviewTrace.Exception("TryGenerateAtlasPreviewBytes", ex);
            AtlasPreviewTrace.GenerationFailed(title, ex.Message);
            return null;
        }
    }

    /// <summary>Декодирование PNG в ImageSource — только UI-поток (SetSourceAsync).</summary>
    public static async Task<Microsoft.UI.Xaml.Media.ImageSource?> DecodeAtlasPreviewAsync(byte[] pngBytes)
    {
        AtlasPreviewTrace.DecodeStart("<bytes>", pngBytes.Length);

        if (pngBytes.Length == 0)
        {
            AtlasPreviewTrace.DecodeComplete("<bytes>", success: false);
            return null;
        }

        try
        {
            var image = new Microsoft.UI.Xaml.Media.Imaging.BitmapImage();
            using var stream = new InMemoryRandomAccessStream();
            await stream.WriteAsync(pngBytes.AsBuffer());
            stream.Seek(0);
            await image.SetSourceAsync(stream);
            AtlasPreviewTrace.DecodeComplete("<bytes>", success: true);
            return image;
        }
        catch (Exception ex)
        {
            AtlasPreviewTrace.Exception("DecodeAtlasPreviewAsync", ex);
            AtlasPreviewTrace.DecodeComplete("<bytes>", success: false);
            return null;
        }
    }

    public static Microsoft.UI.Xaml.Media.ImageSource? GenerateAtlasPreviewPublic(string atlasPath)
    {
        var bytes = TryGenerateAtlasPreviewBytes(atlasPath);
        return bytes is null ? null : DecodeAtlasPreviewAsync(bytes).GetAwaiter().GetResult();
    }

    // RT_RCDATA в '#'-нотации FindResourceW: материал WinUI лежит как res 2000 типа RCDATA.
    private const int MaterialResourceId = 2000;
    private const string MaterialResourceType = "#10";
    private const int ReflectionResourceId = 102;
    private const string ReflectionResourceType = "PNG";
    private const string WindowsUIXamlControlsDll = "Windows.UI.Xaml.Controls.dll";

    private static Microsoft.UI.Xaml.Media.ImageSource? _defaultReflectionImage;
    private static Microsoft.UI.Xaml.Media.ImageSource? _defaultMaterialImage;
    private static bool _defaultReflectionResolved;
    private static bool _defaultMaterialResolved;

    /// <summary>Дефолтное отражение: PNG-ресурс 102 из OpenGlass.dll (живой, без копии в проекте).</summary>
    public static Microsoft.UI.Xaml.Media.ImageSource? TryGetDefaultReflectionImage()
    {
        if (_defaultReflectionResolved)
            return _defaultReflectionImage;

        _defaultReflectionResolved = true;
        var dll = PlatformPaths.OpenGlassDllPath;
        if (string.IsNullOrWhiteSpace(dll) || !File.Exists(dll))
            return null;

        var png = TryExtractResourceImageToCache(dll, ReflectionResourceId, ReflectionResourceType, "reflection_default_102");
        _defaultReflectionImage = png is null ? null : CreateImageSource(png);
        return _defaultReflectionImage;
    }

    /// <summary>Дефолтный материал: RCDATA-ресурс 2000 из системного Windows.UI.Xaml.Controls.dll.</summary>
    public static Microsoft.UI.Xaml.Media.ImageSource? TryGetDefaultMaterialImage()
    {
        if (_defaultMaterialResolved)
            return _defaultMaterialImage;

        _defaultMaterialResolved = true;
        var dll = ResolveWindowsUIXamlControlsDllPath();
        var png = TryExtractResourceImageToCache(dll, MaterialResourceId, MaterialResourceType, "material_default_2000");
        _defaultMaterialImage = png is null ? null : CreateImageSource(png);
        return _defaultMaterialImage;
    }

    /// <summary>Превью окна для текущего визуального стиля (атлас активной .msstyles через theme API).</summary>
    public static byte[]? TryGenerateCurrentThemeWindowPreviewBytes(
        uint width = AtlasPreviewWidth,
        uint height = AtlasPreviewHeight)
    {
        try
        {
            if (width == 0 || height == 0)
                return null;

            var nativeDllPath = ResolveNativeDllPath();
            if (nativeDllPath is null || !File.Exists(nativeDllPath))
                return null;

            var tempPath = Path.Combine(Path.GetTempPath(), $"theme_atlas_preview_{Guid.NewGuid()}.png");

            var hr = GlassSettings_GenerateCurrentThemeAtlasPreview(
                tempPath,
                width,
                height,
                string.Empty,
                LayeredAtlasReflectionAlphaPercent);
            if (hr != 0 || !File.Exists(tempPath))
                return null;

            var bytes = File.ReadAllBytes(tempPath);
            try { File.Delete(tempPath); } catch { }
            return bytes.Length == 0 ? null : bytes;
        }
        catch (Exception ex)
        {
            AtlasPreviewTrace.Exception("TryGenerateCurrentThemeWindowPreviewBytes", ex);
            return null;
        }
    }

    /// <summary>
    /// Миниатюра атласа для layered-превью: кастомный путь, экспорт в каталоге или theme API.
    /// Холст = габариты слоя «Атлас»; шапка прижата к верхнему краю (без центрирования как в коллекции).
    /// </summary>
    public static byte[]? TryGenerateLayeredAtlasPreviewBytes(
        string? customAtlasPath,
        uint width,
        uint height,
        bool preferLiveThemeApi = false)
    {
        if (!string.IsNullOrWhiteSpace(customAtlasPath))
            return TryGenerateAtlasPreviewBytes(
                customAtlasPath,
                width,
                height,
                LayeredAtlasReflectionAlphaPercent);

        if (!preferLiveThemeApi)
        {
            var exportedPath = TryResolveExportedThemeAtlasPath();
            if (exportedPath is not null)
                return TryGenerateAtlasPreviewBytes(
                    exportedPath,
                    width,
                    height,
                    LayeredAtlasReflectionAlphaPercent);
        }

        return TryGenerateCurrentThemeWindowPreviewBytes(width, height);
    }

    /// <summary>Экспортированный атлас активной темы в каталоге AtlasTheme (если есть .layout).</summary>
    public static string? TryResolveExportedThemeAtlasPath()
    {
        var dir = TryResolveAtlasCatalogSaveDirectory(createIfMissing: false);
        if (string.IsNullOrWhiteSpace(dir))
            return null;

        if (!VisualStyleThemeManager.TryGetCurrentMsStylesBaseName(out var baseName)
            || string.IsNullOrWhiteSpace(baseName))
        {
            return null;
        }

        var path = Path.Combine(dir, baseName + ".png");
        if (!File.Exists(path))
            return null;

        return ResolveLayoutPath(path) is not null ? path : null;
    }

    private static string? TryExtractResourceImageToCache(string dllPath, int resId, string resType, string cacheName)
    {
        try
        {
            var dir = Path.Combine(Path.GetTempPath(), "GlassSettingsPreview");
            Directory.CreateDirectory(dir);
            var outPath = Path.Combine(dir, cacheName + ".png");

            var hr = GlassSettings_ExtractResourceImage(dllPath, (uint)resId, resType, outPath);
            if (hr != 0 || !File.Exists(outPath) || new FileInfo(outPath).Length == 0)
                return null;

            return outPath;
        }
        catch (Exception ex)
        {
            AtlasPreviewTrace.Exception($"TryExtractResourceImageToCache:{cacheName}", ex);
            return null;
        }
    }

    private static string ResolveWindowsUIXamlControlsDllPath()
    {
        try
        {
            var sys = Path.Combine(Environment.SystemDirectory, WindowsUIXamlControlsDll);
            if (File.Exists(sys))
                return sys;
        }
        catch { }

        // Резерв: пусть загрузчик найдёт по стандартным путям поиска.
        return WindowsUIXamlControlsDll;
    }

    private static IReadOnlyList<ThemeImageItem> LoadFromResourceFolders(
        IReadOnlyList<string> relativeFolders,
        bool includeImage = true,
        bool traceCatalog = false)
    {
        var searchRoots = BuildSearchRoots();

        if (traceCatalog)
        {
            for (var i = 0; i < searchRoots.Count; i++)
                AtlasPreviewTrace.CatalogSearchRoot(i, searchRoots[i]);
        }

        foreach (var root in searchRoots)
        {
            foreach (var relative in relativeFolders)
            {
                var dir = Path.Combine(root, relative);
                var items = LoadFromDirectory(dir, includeImage);
                if (traceCatalog)
                    AtlasPreviewTrace.CatalogProbe(dir, Directory.Exists(dir), items.Count);

                if (items.Count > 0)
                {
                    if (traceCatalog)
                    {
                        AtlasPreviewTrace.CatalogResolved(dir, items.Count);
                        for (var i = 0; i < items.Count; i++)
                        {
                            AtlasPreviewTrace.CatalogItem(
                                i,
                                items[i].Title,
                                items[i].FilePath,
                                ResolveLayoutPath(items[i].FilePath) is not null);
                        }
                    }

                    return items;
                }
            }
        }

        if (traceCatalog)
            AtlasPreviewTrace.CatalogResolved("<none>", 0);

        return Array.Empty<ThemeImageItem>();
    }

    private static string? TryResolveResourceCatalogDirectory(
        IReadOnlyList<string> relativeFolders,
        bool createIfMissing)
    {
        foreach (var root in BuildSearchRoots())
        {
            foreach (var relative in relativeFolders)
            {
                var dir = Path.Combine(root, relative);
                if (Directory.Exists(dir))
                    return dir;

                if (!createIfMissing)
                    continue;

                try
                {
                    Directory.CreateDirectory(dir);
                    return dir;
                }
                catch
                {
                    // Следующий кандидат.
                }
            }
        }

        foreach (var dir in EnumerateAtlasCatalogDirectoryCandidates())
        {
            if (Directory.Exists(dir))
                return dir;
        }

        return null;
    }

    private static List<string> BuildSearchRoots() =>
        new(PlatformPaths.GetResourceSearchRoots());

    private static IReadOnlyList<ThemeImageItem> LoadFromDirectory(string dir, bool includeImage = true)
    {
        if (!Directory.Exists(dir))
            return Array.Empty<ThemeImageItem>();

        var list = new List<ThemeImageItem>();
        foreach (var file in Directory.EnumerateFiles(dir, "*.*", SearchOption.TopDirectoryOnly))
        {
            var ext = Path.GetExtension(file).ToLowerInvariant();
            if (ext is not ".png" and not ".jpg" and not ".jpeg" and not ".bmp" and not ".webp")
                continue;

            list.Add(new ThemeImageItem
            {
                Title = Path.GetFileNameWithoutExtension(file),
                FilePath = file,
                Image = includeImage ? CreateImageSource(file) : null
            });
        }

        return list;
    }

    internal static Microsoft.UI.Xaml.Media.ImageSource? CreateImageSource(string filePath)
    {
        if (string.IsNullOrWhiteSpace(filePath) || !File.Exists(filePath))
            return null;

        try
        {
            var fullPath = Path.GetFullPath(filePath);
            return new Microsoft.UI.Xaml.Media.Imaging.BitmapImage(new Uri(fullPath, UriKind.Absolute));
        }
        catch
        {
            return null;
        }
    }

    private static string? ResolveLayoutPath(string atlasPath)
    {
        var layoutPath = atlasPath + ".Layout";
        if (File.Exists(layoutPath))
            return layoutPath;

        layoutPath = atlasPath + ".layout";
        return File.Exists(layoutPath) ? layoutPath : null;
    }

    private static string? ResolveNativeDllPath()
    {
        var nextToExe = Path.Combine(PlatformPaths.AppBase, NativeDllName);
        if (File.Exists(nextToExe))
            return nextToExe;

        return File.Exists(NativeDllName) ? Path.GetFullPath(NativeDllName) : null;
    }

    [DllImport(NativeDllName, CharSet = CharSet.Unicode, SetLastError = false)]
    private static extern int GlassSettings_GenerateAtlasPreview(
        string atlasPath,
        string layoutPath,
        string outputPath,
        uint targetWidth,
        uint targetHeight,
        string reflectionDllPath,
        uint reflectionAlphaPercent);

    [DllImport(NativeDllName, CharSet = CharSet.Unicode, SetLastError = false)]
    private static extern int GlassSettings_ExtractResourceImage(
        string dllPath,
        uint resId,
        string resType,
        string outputPath);

    [DllImport(NativeDllName, CharSet = CharSet.Unicode, SetLastError = false)]
    private static extern int GlassSettings_GenerateCurrentThemeAtlasPreview(
        string outputPath,
        uint targetWidth,
        uint targetHeight,
        string? reflectionDllPath,
        uint reflectionAlphaPercent);

    [DllImport(NativeDllName, CharSet = CharSet.Unicode, SetLastError = false)]
    private static extern int GlassSettings_ExportCurrentThemeAtlas(string outputPngPath);

    /// <summary>
    /// Сохраняет «сырой» атлас активной темы (DISKSTREAM PNG) в <paramref name="outputPngPath"/>
    /// и рядом «&lt;outputPngPath&gt;.layout». Возвращает HRESULT (0 — успех).
    /// Логика донора OpenGlassGUI «Export current theme atlas».
    /// </summary>
    public static int ExportCurrentThemeAtlas(string outputPngPath)
    {
        try
        {
            if (string.IsNullOrWhiteSpace(outputPngPath))
                return unchecked((int)0x80004003); // E_POINTER

            var nativeDllPath = ResolveNativeDllPath();
            if (nativeDllPath is null || !File.Exists(nativeDllPath))
                return unchecked((int)0x80070002); // ERROR_FILE_NOT_FOUND

            var hr = GlassSettings_ExportCurrentThemeAtlas(outputPngPath);
            AtlasPreviewTrace.GenerationHResult(hr, "GlassSettings_ExportCurrentThemeAtlas");
            return hr;
        }
        catch (Exception ex)
        {
            AtlasPreviewTrace.Exception("ExportCurrentThemeAtlas", ex);
            return unchecked((int)0x80004005); // E_FAIL
        }
    }
}
