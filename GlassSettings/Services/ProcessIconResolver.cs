using System.Collections.Concurrent;
using System.Diagnostics;
using System.Runtime.InteropServices.WindowsRuntime;
using GlassSettings.Core.Registry;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.Storage.Streams;

namespace GlassSettings.Services;

/// <summary>Асинхронное разрешение иконок и описаний exe для AttachList.</summary>
public static class ProcessIconResolver
{
    private static readonly ConcurrentDictionary<string, byte[]> PngCache = new(StringComparer.OrdinalIgnoreCase);
    private static readonly ConcurrentDictionary<string, ImageSource> BitmapCache = new(StringComparer.OrdinalIgnoreCase);
    private static readonly ConcurrentDictionary<string, ProcessFileMetadata> MetadataCache = new(StringComparer.OrdinalIgnoreCase);
    private static readonly ConcurrentDictionary<string, Task<byte[]?>> PngInFlight = new(StringComparer.OrdinalIgnoreCase);
    private static List<ProcessEntryModel>? _prebuiltEntries;
    /// <summary>x86: стоп native I/O после AV в полном PreloadAttachListAsync (splash warm).</summary>
    private static bool _nativeIconIoDisabled;

    public sealed record ProcessFileMetadata(
        string? ResolvedPath,
        string? ProductName,
        string? FileDescription,
        string? CompanyName,
        string? FileVersion,
        string? FileModifiedLabel);

    public static ImageSource? TryGetCachedIcon(string exeName) =>
        BitmapCache.TryGetValue(exeName, out var icon) ? icon : null;

    /// <summary>Иконка зарегистрированного типа по расширению файла — fallback вместо generic glyph.</summary>
    public static ImageSource? TryGetDefaultFileTypeIcon(string fileName)
    {
        var cacheKey = GetFileTypeIconCacheKey(fileName);
        return BitmapCache.TryGetValue(cacheKey, out var icon) ? icon : null;
    }

    public static bool TryGetCachedMetadata(string exeName, out ProcessFileMetadata metadata) =>
        MetadataCache.TryGetValue(exeName, out metadata!);

    public static ProcessFileMetadata ResolveMetadata(string exeName)
    {
        if (MetadataCache.TryGetValue(exeName, out var cached))
            return cached;

        if (_nativeIconIoDisabled)
        {
            return MetadataCache.GetOrAdd(
                exeName,
                static _ => new ProcessFileMetadata(null, null, null, null, null, null));
        }

        return MetadataCache.GetOrAdd(exeName, static name =>
        {
            try
            {
                var path = NativeShellIconInterop.ResolveExecutablePath(name);
                return new ProcessFileMetadata(path, null, null, null, null, null);
            }
            catch
            {
                return new ProcessFileMetadata(null, null, null, null, null, null);
            }
        });
    }

    public enum ManualProcessNameResolveStatus
    {
        Empty,
        Success,
        InvalidExtension,
        ExtensionRequired
    }

    public sealed record ManualProcessNameResolveResult(ManualProcessNameResolveStatus Status, string? ExeName);

    /// <summary>Разбор имени процесса из ручного ввода; путь — через <see cref="ResolveMetadata"/>.</summary>
    public static ManualProcessNameResolveResult TryResolveManualProcessName(string input)
    {
        if (string.IsNullOrWhiteSpace(input))
            return new ManualProcessNameResolveResult(ManualProcessNameResolveStatus.Empty, null);

        var fileName = Path.GetFileName(input.Trim());
        if (string.IsNullOrWhiteSpace(fileName))
            return new ManualProcessNameResolveResult(ManualProcessNameResolveStatus.Empty, null);

        var extension = Path.GetExtension(fileName);
        if (!string.IsNullOrEmpty(extension))
        {
            if (!ExecutableFilePickerCatalog.IsExecutableExtension(fileName))
                return new ManualProcessNameResolveResult(ManualProcessNameResolveStatus.InvalidExtension, null);

            return new ManualProcessNameResolveResult(
                ManualProcessNameResolveStatus.Success,
                AttachListRegistry.NormalizeExeName(fileName));
        }

        foreach (var ext in ExecutableFilePickerCatalog.SupportedExecutableExtensions)
        {
            var candidate = AttachListRegistry.NormalizeExeName(fileName + ext);
            if (ResolveMetadata(candidate).ResolvedPath is not null)
            {
                return new ManualProcessNameResolveResult(
                    ManualProcessNameResolveStatus.Success,
                    candidate);
            }
        }

        return new ManualProcessNameResolveResult(ManualProcessNameResolveStatus.ExtensionRequired, null);
    }

    /// <summary>Зарегистрировать полный путь exe в общем path-cache (MuiCache/AppCompat + in-memory).</summary>
    public static void RegisterKnownExecutablePath(string fullPath)
    {
        NativeShellIconInterop.EnsureRegistryPathCacheEntry(fullPath);

        var exeName = AttachListRegistry.NormalizeExeName(Path.GetFileName(fullPath));
        if (!string.IsNullOrEmpty(exeName))
            MetadataCache.TryRemove(exeName, out _);
    }

    /// <summary>Метаданные + PNG иконки для одного exe — тот же pipeline, что при preload AttachList.</summary>
    public static async Task PreloadExeAssetsAsync(string exeName)
    {
        if (_nativeIconIoDisabled)
            return;

        await Task.Run(() =>
        {
            NativeShellIconInterop.WarmPathCaches([exeName]);
            ResolveMetadata(exeName);
            TryEnrichMetadata(exeName);
        }).ConfigureAwait(false);

        await EnsureIconsAsync([exeName]).ConfigureAwait(false);
    }

    private static void TryEnrichMetadata(string exeName, int timeoutMs = 500)
    {
        if (!MetadataCache.TryGetValue(exeName, out var meta) || meta.ResolvedPath is null)
            return;

        // FileVersionInfo не требует STA; отдельный 1MB-поток RunWithTimeout
        // давал GS cookie на powershell_ise.exe (dump line 70).
        _ = timeoutMs;
        var details = ReadVersionDetails(meta.ResolvedPath!);
        if (details is null)
            return;

        MetadataCache[exeName] = new ProcessFileMetadata(
            meta.ResolvedPath,
            details.Value.ProductName,
            details.Value.FileDescription,
            details.Value.CompanyName,
            details.Value.FileVersion,
            details.Value.FileModifiedLabel);
    }

    private static (string? ProductName, string? FileDescription, string? CompanyName, string? FileVersion, string? FileModifiedLabel)?
        ReadVersionDetails(string path)
    {
        string? product = null;
        string? description = null;
        string? company = null;
        string? version = null;
        string? modified = null;

        try
        {
            var info = FileVersionInfo.GetVersionInfo(path);
            product = string.IsNullOrWhiteSpace(info.ProductName) ? null : info.ProductName.Trim();
            description = string.IsNullOrWhiteSpace(info.FileDescription) ? null : info.FileDescription.Trim();
            company = string.IsNullOrWhiteSpace(info.CompanyName) ? null : info.CompanyName.Trim();
            version = string.IsNullOrWhiteSpace(info.FileVersion) ? null : info.FileVersion.Trim();
        }
        catch
        {
            // exe без version resource
        }

        try
        {
            if (File.Exists(path))
                modified = File.GetLastWriteTime(path).ToString("dd.MM.yyyy");
        }
        catch
        {
            // дата недоступна
        }

        return (product, description, company, version, modified);
    }

    /// <summary>
    /// Лёгкий preload под заставкой: только имена AttachList, без WarmPathCaches/SHGetFileInfo.
    /// Wow64 (GS x86 на OS x64): native I/O глушим — AV в SHGetFileInfo при догрузке.
    /// GS x86 на OS x86 и GS x64: флаг не ставим — LoadMissingIconsAsync грузит иконки.
    /// </summary>
    public static Task PreloadAttachListNamesOnlyAsync()
    {
        if (!Environment.Is64BitProcess && Environment.Is64BitOperatingSystem)
            _nativeIconIoDisabled = true;

        _prebuiltEntries = BuildFallbackEntryModels();
        return Task.CompletedTask;
    }

    /// <summary>PNG + метаданные + BitmapImage — во время заставки.</summary>
    public static async Task PreloadAttachListAsync(DispatcherQueue dispatcherQueue)
    {
        if (_nativeIconIoDisabled)
        {
            await PreloadAttachListNamesOnlyAsync().ConfigureAwait(false);
            return;
        }

        var names = AttachListRegistry.GetEntries()
            .Select(static e => e.ExeName)
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToList();

        try
        {
            if (names.Count > 0)
            {
                await Task.Run(() =>
                {
                    NativeShellIconInterop.WarmPathCaches(names);
                    foreach (var name in names)
                        ResolveMetadata(name);
                    foreach (var name in names)
                        TryEnrichMetadata(name);
                }).ConfigureAwait(false);

                await EnsureFileTypeIconsAsync(names).ConfigureAwait(false);
                await EnsureIconsAsync(names).ConfigureAwait(false);
                await PrewarmBitmapCacheAsync(dispatcherQueue, names).ConfigureAwait(true);
                await PrewarmFileTypeIconsAsync(dispatcherQueue, names).ConfigureAwait(true);
            }
        }
        catch
        {
            // x86: AV в WarmPathCaches/SHGetFileInfo — дальше только names-only / без native I/O.
            _nativeIconIoDisabled = true;
        }
        finally
        {
            try
            {
                CachePrebuiltEntries();
            }
            catch
            {
                _prebuiltEntries = BuildFallbackEntryModels();
            }
        }
    }

    private static List<ProcessEntryModel> BuildFallbackEntryModels()
    {
        var models = new List<ProcessEntryModel>();
        foreach (var entry in AttachListRegistry.GetEntries())
        {
            models.Add(new ProcessEntryModel(
                entry.ExeName,
                entry.Mode,
                null,
                null,
                null,
                null,
                null,
                null));
        }

        return models;
    }

    /// <summary>Список AttachList из кэша метаданных и BitmapImage — синхронно, без I/O иконок.</summary>
    public static List<ProcessEntryModel> CreateAttachListModels() => BuildEntryModels();

    /// <summary>Список BlockList (встроенный + реестр).</summary>
    public static List<ProcessEntryModel> CreateBlockListModels() => BuildBlockListEntryModels();

    /// <summary>
    /// UI-поток до завершения preload: без ResolveExecutablePath/SHGetFileInfo
    /// (на x86 sync path в EnsureListLoaded давал AV — лог: NavFrame done, нет EnsureListLoaded.done).
    /// </summary>
    public static List<ProcessEntryModel> CreateAttachListModelsSafe() =>
        IsPreloadComplete ? BuildEntryModels() : BuildFallbackEntryModels();

    public static List<ProcessEntryModel> CreateBlockListModelsSafe()
    {
        if (IsPreloadComplete)
            return BuildBlockListEntryModels();

        var models = new List<ProcessEntryModel>();
        foreach (var entry in BlockListRegistry.GetUserEntries())
        {
            models.Add(new ProcessEntryModel(
                entry.ExeName,
                AttachProcessMode.Forbidden,
                null,
                null,
                null,
                null,
                null,
                null)
            {
                Section = ProcessSection.BlockList,
                IsBuiltInBlockList = entry.IsBuiltIn
            });
        }

        return models;
    }

    public static bool IsPreloadComplete => _prebuiltEntries is not null;

    public static bool TryClonePrebuiltEntries(out List<ProcessEntryModel> entries)
    {
        if (_prebuiltEntries is null || _prebuiltEntries.Count == 0)
        {
            entries = [];
            return false;
        }

        entries = new List<ProcessEntryModel>(_prebuiltEntries.Count);
        foreach (var src in _prebuiltEntries)
        {
            var model = new ProcessEntryModel(
                src.ExeName,
                src.Mode,
                src.ProductName,
                src.FileDescription,
                src.ResolvedPath,
                src.CompanyName,
                src.FileVersion,
                src.FileModifiedLabel);
            if (src.Icon is not null)
                model.Icon = src.Icon;
            entries.Add(model);
        }

        return true;
    }

    public static bool IsAttachListReady => IsPreloadComplete || !BitmapCache.IsEmpty;

    public static async Task EnsureIconsAsync(IEnumerable<string> exeNames)
    {
        if (_nativeIconIoDisabled)
            return;

        var pending = exeNames
            .Where(static name => !string.IsNullOrWhiteSpace(name))
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .Where(static name => !PngCache.ContainsKey(name))
            .ToList();

        if (pending.Count == 0)
            return;

        foreach (var name in pending)
            await GetOrLoadPngBytesAsync(name).ConfigureAwait(false);
    }

    public static async Task<ImageSource?> GetIconAsync(string exeName, DispatcherQueue dispatcherQueue)
    {
        if (BitmapCache.TryGetValue(exeName, out var cached))
            return cached;

        if (_nativeIconIoDisabled)
            return null;

        var pngBytes = await GetOrLoadPngBytesAsync(exeName).ConfigureAwait(false);
        if (pngBytes is null)
            pngBytes = await GetOrLoadDefaultFileTypePngAsync(exeName).ConfigureAwait(false);
        if (pngBytes is null)
            return null;

        var image = await CreateBitmapImageAsync(dispatcherQueue, pngBytes).ConfigureAwait(true);
        if (image is not null)
            BitmapCache[exeName] = image;
        return image;
    }

    /// <summary>
    /// Иконка зарегистрированного типа файла по расширению имени
    /// (<see cref="NativeShellIconInterop.GetRegisteredFileTypeIconFromFileName"/>).
    /// </summary>
    public static async Task<ImageSource?> GetFileTypeIconAsync(string fileName, DispatcherQueue dispatcherQueue)
    {
        if (string.IsNullOrWhiteSpace(fileName))
            return null;

        var cacheKey = GetFileTypeIconCacheKey(fileName);
        if (BitmapCache.TryGetValue(cacheKey, out var cached))
            return cached;

        if (_nativeIconIoDisabled)
            return null;

        var pngBytes = await GetOrLoadDefaultFileTypePngAsync(fileName).ConfigureAwait(false);
        if (pngBytes is null)
            return null;

        var image = await CreateBitmapImageAsync(dispatcherQueue, pngBytes).ConfigureAwait(true);
        if (image is not null)
            BitmapCache[cacheKey] = image;
        return image;
    }

    /// <summary>
    /// Новая копия BitmapImage из PNG-кэша — для таблиц, где нельзя шарить ImageSource
    /// с плитками AttachList (иначе Source «отбирается» у других Image).
    /// </summary>
    public static async Task<ImageSource?> GetIconCopyAsync(string fileName, DispatcherQueue dispatcherQueue)
    {
        if (string.IsNullOrWhiteSpace(fileName) || _nativeIconIoDisabled)
            return null;

        var pngBytes = await GetOrLoadPngBytesAsync(fileName).ConfigureAwait(false);
        if (pngBytes is null)
            pngBytes = await GetOrLoadDefaultFileTypePngAsync(fileName).ConfigureAwait(false);
        if (pngBytes is null)
            return null;

        // Не пишем в BitmapCache — каждый вызов даёт отдельный ImageSource.
        return await CreateBitmapImageAsync(dispatcherQueue, pngBytes).ConfigureAwait(true);
    }

    /// <summary>Копия иконки зарегистрированного типа (отдельный BitmapImage на каждый вызов).</summary>
    public static async Task<ImageSource?> GetFileTypeIconCopyAsync(string fileName, DispatcherQueue dispatcherQueue)
    {
        if (string.IsNullOrWhiteSpace(fileName) || _nativeIconIoDisabled)
            return null;

        var pngBytes = await GetOrLoadDefaultFileTypePngAsync(fileName).ConfigureAwait(false);
        if (pngBytes is null)
            return null;

        return await CreateBitmapImageAsync(dispatcherQueue, pngBytes).ConfigureAwait(true);
    }

    public static void ApplyCachedIcons(IEnumerable<ProcessEntryModel> entries)
    {
        foreach (var entry in entries)
        {
            if (BitmapCache.TryGetValue(entry.ExeName, out var icon))
                entry.Icon = icon;
        }
    }

    public static void Invalidate(string exeName)
    {
        BitmapCache.TryRemove(exeName, out _);
        PngCache.TryRemove(exeName, out _);
        MetadataCache.TryRemove(exeName, out _);
        NativeShellIconInterop.InvalidatePathCaches();
        _prebuiltEntries = null;
    }

    public static void ClearCache()
    {
        BitmapCache.Clear();
        PngCache.Clear();
        MetadataCache.Clear();
        PngInFlight.Clear();
        NativeShellIconInterop.InvalidatePathCaches();
        _prebuiltEntries = null;
    }

    private static void CachePrebuiltEntries() =>
        _prebuiltEntries = BuildEntryModels();

    private static List<ProcessEntryModel> BuildEntryModels()
    {
        var models = new List<ProcessEntryModel>();
        foreach (var entry in AttachListRegistry.GetEntries())
        {
            if (!TryGetCachedMetadata(entry.ExeName, out var metadata))
                metadata = ResolveMetadata(entry.ExeName);

            var model = new ProcessEntryModel(
                entry.ExeName,
                entry.Mode,
                metadata.ProductName,
                metadata.FileDescription,
                metadata.ResolvedPath,
                metadata.CompanyName,
                metadata.FileVersion,
                metadata.FileModifiedLabel);

            if (BitmapCache.TryGetValue(entry.ExeName, out var icon))
                model.Icon = icon;
            else if (TryGetDefaultFileTypeIcon(entry.ExeName) is { } fallback)
                model.Icon = fallback;

            models.Add(model);
        }

        return models;
    }

    private static List<ProcessEntryModel> BuildBlockListEntryModels()
    {
        var models = new List<ProcessEntryModel>();
        foreach (var entry in BlockListRegistry.GetUserEntries())
        {
            if (!TryGetCachedMetadata(entry.ExeName, out var metadata))
                metadata = ResolveMetadata(entry.ExeName);

            var model = new ProcessEntryModel(
                entry.ExeName,
                AttachProcessMode.Forbidden,
                metadata.ProductName,
                metadata.FileDescription,
                metadata.ResolvedPath,
                metadata.CompanyName,
                metadata.FileVersion,
                metadata.FileModifiedLabel)
            {
                Section = ProcessSection.BlockList,
                IsBuiltInBlockList = entry.IsBuiltIn
            };

            if (BitmapCache.TryGetValue(entry.ExeName, out var icon))
                model.Icon = icon;
            else if (TryGetDefaultFileTypeIcon(entry.ExeName) is { } fallback)
                model.Icon = fallback;

            models.Add(model);
        }

        return models;
    }

    private static async Task PrewarmBitmapCacheAsync(DispatcherQueue dispatcherQueue, IReadOnlyList<string> names)
    {
        foreach (var name in names)
        {
            if (BitmapCache.ContainsKey(name))
                continue;

            if (!PngCache.TryGetValue(name, out var pngBytes))
                continue;

            var image = await CreateBitmapImageAsync(dispatcherQueue, pngBytes).ConfigureAwait(true);
            if (image is not null)
                BitmapCache[name] = image;
        }
    }

    private static async Task<byte[]?> GetOrLoadPngBytesAsync(string exeName)
    {
        if (PngCache.TryGetValue(exeName, out var cached))
            return cached;

        if (PngInFlight.TryGetValue(exeName, out var existing))
            return await existing.ConfigureAwait(false);

        var loadTask = StaThreadRunner.RunAsync(() => LoadIconPngBytes(exeName));
        if (!PngInFlight.TryAdd(exeName, loadTask))
            return await PngInFlight[exeName].ConfigureAwait(false);

        try
        {
            var pngBytes = await loadTask.ConfigureAwait(false);
            if (pngBytes is not null)
                PngCache[exeName] = pngBytes;
            return pngBytes;
        }
        finally
        {
            PngInFlight.TryRemove(exeName, out _);
        }
    }

    private static byte[]? LoadIconPngBytes(string exeName)
    {
        var pngBytes = TryLoadIconPngBytes(exeName);
        return pngBytes ?? LoadDefaultFileTypePngBytes(exeName);
    }

    private static byte[]? TryLoadIconPngBytes(string exeName)
    {
        try
        {
            var hIcon = NativeShellIconInterop.GetExecutableIconHandle(exeName, out var mustDestroy);
            try
            {
                if (hIcon == IntPtr.Zero)
                    return null;

                return IconHandleToPngBytes(hIcon);
            }
            finally
            {
                NativeShellIconInterop.ReleaseIcon(hIcon, mustDestroy);
            }
        }
        catch
        {
            _nativeIconIoDisabled = true;
            return null;
        }
    }

    private static byte[]? LoadDefaultFileTypePngBytes(string fileName)
    {
        var cacheKey = GetFileTypeIconCacheKey(fileName);
        if (PngCache.TryGetValue(cacheKey, out var cached))
            return cached;

        try
        {
            var hIcon = NativeShellIconInterop.GetRegisteredFileTypeIconFromFileName(fileName);
            if (hIcon == IntPtr.Zero)
                return null;

            try
            {
                var pngBytes = IconHandleToPngBytes(hIcon);
                if (pngBytes is not null)
                    PngCache[cacheKey] = pngBytes;
                return pngBytes;
            }
            finally
            {
                NativeShellIconInterop.ReleaseIcon(hIcon, mustDestroy: true);
            }
        }
        catch
        {
            _nativeIconIoDisabled = true;
            return null;
        }
    }

    private static Task<byte[]?> GetOrLoadDefaultFileTypePngAsync(string fileName)
    {
        var cacheKey = GetFileTypeIconCacheKey(fileName);
        if (PngCache.TryGetValue(cacheKey, out var cached))
            return Task.FromResult<byte[]?>(cached);

        return StaThreadRunner.RunAsync(() => LoadDefaultFileTypePngBytes(fileName));
    }

    private static async Task EnsureFileTypeIconsAsync(IEnumerable<string> fileNames)
    {
        var extensions = fileNames
            .Where(static name => !string.IsNullOrWhiteSpace(name))
            .Select(NativeShellIconInterop.GetNormalizedExtension)
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToList();

        foreach (var ext in extensions)
            await GetOrLoadDefaultFileTypePngAsync(NormalizeExtensionToFileName(ext)).ConfigureAwait(false);
    }

    private static async Task PrewarmFileTypeIconsAsync(DispatcherQueue dispatcherQueue, IEnumerable<string> fileNames)
    {
        foreach (var name in fileNames.Where(static n => !string.IsNullOrWhiteSpace(n)).Distinct(StringComparer.OrdinalIgnoreCase))
        {
            var cacheKey = GetFileTypeIconCacheKey(name);
            if (BitmapCache.ContainsKey(cacheKey))
                continue;

            if (!PngCache.TryGetValue(cacheKey, out var pngBytes))
                continue;

            var image = await CreateBitmapImageAsync(dispatcherQueue, pngBytes).ConfigureAwait(true);
            if (image is not null)
                BitmapCache[cacheKey] = image;
        }
    }

    private static string GetFileTypeIconCacheKey(string fileName) =>
        "__type__" + NativeShellIconInterop.GetNormalizedExtension(fileName).ToLowerInvariant();

    private static string NormalizeExtensionToFileName(string extension)
    {
        var ext = extension.Trim();
        if (!ext.StartsWith('.'))
            ext = "." + ext;
        return "_" + ext;
    }

    private static byte[]? IconHandleToPngBytes(IntPtr hIcon)
    {
        using var icon = (System.Drawing.Icon)System.Drawing.Icon.FromHandle(hIcon).Clone();
        using var bitmap = icon.ToBitmap();
        using var stream = new MemoryStream();
        bitmap.Save(stream, System.Drawing.Imaging.ImageFormat.Png);
        return stream.ToArray();
    }

    private static Task<ImageSource?> CreateBitmapImageAsync(DispatcherQueue dispatcherQueue, byte[] pngBytes)
    {
        var tcs = new TaskCompletionSource<ImageSource?>(TaskCreationOptions.RunContinuationsAsynchronously);

        if (!dispatcherQueue.TryEnqueue(DispatcherQueuePriority.Normal, () => _ = CreateBitmapImageCoreAsync(pngBytes, tcs)))
            tcs.TrySetResult(null);

        return tcs.Task;
    }

    private static async Task CreateBitmapImageCoreAsync(byte[] pngBytes, TaskCompletionSource<ImageSource?> tcs)
    {
        try
        {
            var image = new BitmapImage();
            using var stream = new InMemoryRandomAccessStream();
            await stream.WriteAsync(pngBytes.AsBuffer());
            stream.Seek(0);
            await image.SetSourceAsync(stream);
            tcs.TrySetResult(image);
        }
        catch
        {
            tcs.TrySetResult(null);
        }
    }
}
