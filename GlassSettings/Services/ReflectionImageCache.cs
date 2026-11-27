using System.Runtime.InteropServices.WindowsRuntime;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;

namespace GlassSettings.Services;

/// <summary>
/// Единый кеш изображений отражений, материалов и атласов.
///
/// Файл декодируется из байтов в памяти, а не через <c>BitmapImage.UriSource</c>: так
/// файл не удерживается открытым (поэтому удаление из галереи больше не падает), а
/// WinUI не подменяет изображение прежним содержимым по тому же пути. Опубликованное
/// полотно профиля всегда имеет одно имя файла, поэтому URI-кеш возвращал старую
/// картинку — и слой превью не обновлялся после выбора другой текстуры.
///
/// Ключ включает время записи и размер файла: перезапись полотна даёт новый ключ,
/// и изображение перечитывается без ручной инвалидации.
/// </summary>
internal static class ReflectionImageCache
{
    /// <summary>Верхняя граница декодирования: превью не требует файлов больше этого.</summary>
    private const long MaximumSourceBytes = 64L * 1024 * 1024;

    /// <summary>Сколько записей держим в памяти, чтобы кеш не рос бесконечно.</summary>
    private const int MaximumEntries = 96;

    private const string ReflectionRegistryKey = "CustomThemeReflection";

    private readonly record struct EntryKey(string FullPath, long Length, long WriteTicks);

    private static readonly object Gate = new();
    private static readonly Dictionary<EntryKey, ImageSource?> Sources = [];
    private static readonly Dictionary<EntryKey, Task<ImageSource?>> PendingTiles = [];
    private static readonly Dictionary<EntryKey, ImageSource?> Tiles = [];

    private static EntryKey KeyOf(string path)
    {
        var full = Path.GetFullPath(path);
        var info = new FileInfo(full);
        return info.Exists
            ? new EntryKey(full, info.Length, info.LastWriteTimeUtc.Ticks)
            : new EntryKey(full, -1, 0);
    }

    /// <summary>Изображение из кеша без обращения к диску.</summary>
    public static ImageSource? TryGet(string? path)
    {
        if (string.IsNullOrWhiteSpace(path)) return null;
        try
        {
            var key = KeyOf(path);
            lock (Gate)
                return Sources.TryGetValue(key, out var source) ? source : null;
        }
        catch { return null; }
    }

    private static bool IsUsable(EntryKey key) =>
        key.Length > 0 && key.Length <= MaximumSourceBytes && File.Exists(key.FullPath);

    /// <summary>
    /// Синхронно декодирует файл из памяти и запоминает результат. Вызывать на UI-потоке:
    /// создание BitmapImage привязано к нему.
    /// </summary>
    public static ImageSource? GetOrLoad(string? path)
    {
        if (string.IsNullOrWhiteSpace(path)) return null;
        try
        {
            var key = KeyOf(path);
            lock (Gate)
                if (Sources.TryGetValue(key, out var cached)) return cached;

            if (!IsUsable(key)) return Remember(key, null);
            var bitmap = new BitmapImage();
            using (var memory = new MemoryStream(File.ReadAllBytes(key.FullPath), writable: false))
            using (var stream = memory.AsRandomAccessStream())
                bitmap.SetSource(stream);
            return Remember(key, bitmap);
        }
        catch (Exception error)
        {
            System.Diagnostics.Trace.TraceWarning("Reflection image decode failed for {0}: {1}", path, error);
            return null;
        }
    }

    /// <summary>
    /// Асинхронное чтение для полноразмерного полотна: байты читаются вне UI-потока,
    /// декодирование создаёт BitmapImage на UI-потоке. Вызывать на UI-потоке.
    /// </summary>
    public static async Task<ImageSource?> LoadAsync(string? path)
    {
        if (string.IsNullOrWhiteSpace(path)) return null;
        EntryKey key;
        try { key = KeyOf(path); }
        catch { return null; }
        lock (Gate)
            if (Sources.TryGetValue(key, out var cached)) return cached;
        try
        {
            if (!IsUsable(key)) return GetOrLoad(key.FullPath);
            var bytes = await File.ReadAllBytesAsync(key.FullPath).ConfigureAwait(true);
            var bitmap = new BitmapImage();
            using (var memory = new MemoryStream(bytes, writable: false))
            using (var stream = memory.AsRandomAccessStream())
                bitmap.SetSource(stream);
            return Remember(key, bitmap);
        }
        catch (Exception error)
        {
            System.Diagnostics.Trace.TraceWarning("Reflection image load failed for {0}: {1}", path, error);
            return null;
        }
    }

    /// <summary>
    /// Миниатюра плитки галереи: текстура замощается, SVG растеризуется существующими
    /// рендерерами. Кешируется отдельно от полноразмерного изображения.
    /// </summary>
    public static async Task<ImageSource?> GetTileAsync(string? path)
    {
        if (string.IsNullOrWhiteSpace(path)) return null;
        EntryKey key;
        try { key = KeyOf(path); }
        catch { return null; }

        Task<ImageSource?>? running;
        lock (Gate)
        {
            if (Tiles.TryGetValue(key, out var cached)) return cached;
            running = PendingTiles.TryGetValue(key, out var pending) ? pending : null;
        }
        if (running is not null) return await running.ConfigureAwait(true);

        var work = CreateTileAsync(key);
        lock (Gate) PendingTiles[key] = work;
        try { return await work.ConfigureAwait(true); }
        finally { lock (Gate) PendingTiles.Remove(key); }
    }

    private static async Task<ImageSource?> CreateTileAsync(EntryKey key)
    {
        try
        {
            if (!IsUsable(key)) return null;
            ImageSource? tile = SvgReflectionRenderer.IsPattern(key.FullPath)
                ? await SvgReflectionRenderer.CreatePreviewAsync(key.FullPath).ConfigureAwait(true)
                : await ReflectionTextureService.CreatePreviewAsync(key.FullPath).ConfigureAwait(true);
            lock (Gate)
            {
                Tiles[key] = tile;
                Trim(Tiles);
            }
            return tile;
        }
        catch (Exception error)
        {
            System.Diagnostics.Trace.TraceWarning("Reflection tile preview failed for {0}: {1}", key.FullPath, error);
            return null;
        }
    }

    private static ImageSource? Remember(EntryKey key, ImageSource? source)
    {
        lock (Gate)
        {
            Sources[key] = source;
            Trim(Sources);
        }
        return source;
    }

    /// <summary>Старые записи вытесняются: кеш не должен расти вместе с каталогом.</summary>
    private static void Trim<TValue>(Dictionary<EntryKey, TValue> cache)
    {
        if (cache.Count <= MaximumEntries) return;
        foreach (var key in cache.Keys.Take(cache.Count - MaximumEntries).ToArray())
            cache.Remove(key);
    }

    /// <summary>Забыть записи пути: файл удалён или перезаписан.</summary>
    public static void Invalidate(string? path)
    {
        if (string.IsNullOrWhiteSpace(path)) return;
        string full;
        try { full = Path.GetFullPath(path); }
        catch { return; }
        lock (Gate)
        {
            foreach (var key in Sources.Keys.Where(k => Same(k.FullPath, full)).ToArray())
                Sources.Remove(key);
            foreach (var key in Tiles.Keys.Where(k => Same(k.FullPath, full)).ToArray())
                Tiles.Remove(key);
        }
    }

    private static bool Same(string left, string right) =>
        string.Equals(left, right, StringComparison.OrdinalIgnoreCase);

    /// <summary>
    /// Прогрев каталога во время заставки. Декодирование PNG и растеризация SVG —
    /// самая заметная часть первого открытия страницы и переключения вкладок, поэтому
    /// они выполняются заранее и по одному за проход UI-потока.
    /// </summary>
    public static void BeginPreload(DispatcherQueue dispatcherQueue) =>
        _ = RunPreloadAsync(dispatcherQueue);

    private static async Task RunPreloadAsync(DispatcherQueue dispatcherQueue)
    {
        try
        {
            // Каталог читает каталоги ресурсов (файловый I/O) — выносим его с UI-потока.
            var paths = await Task.Run(() => CollectCatalogPaths()).ConfigureAwait(true);
            if (paths.Length == 0) return;

            // Системное отражение (ресурс OpenGlass) и полотно профиля — то, что превью
            // нужно первым. Ресурс извлекается на UI-потоке, как и любое создание bitmap.
            if (dispatcherQueue.TryEnqueue(DispatcherQueuePriority.Low,
                    () => WarmDefaultReflection()))
                await Task.Delay(1).ConfigureAwait(true);
            await WarmFullSizeAsync();

            foreach (var path in paths)
            {
                var captured = path;
                if (!dispatcherQueue.TryEnqueue(DispatcherQueuePriority.Low,
                        () => _ = GetTileAsync(captured)))
                    return;
                if (!dispatcherQueue.TryEnqueue(DispatcherQueuePriority.Low,
                        () => GetOrLoad(captured)))
                    return;
                // Yield the UI thread between items: the splash stays responsive.
                await Task.Delay(1).ConfigureAwait(true);
            }
        }
        catch (Exception error)
        {
            System.Diagnostics.Trace.TraceWarning("Reflection image preload: {0}", error);
        }
    }

    private static void WarmDefaultReflection() => ThemeImageCatalog.TryGetDefaultReflectionImage();

    private static string[] CollectCatalogPaths()
    {
        try
        {
            var paths = new List<string>();
            paths.AddRange(ThemeImageCatalog.LoadReflectionImages().Select(item => item.FilePath));
            paths.AddRange(ThemeImageCatalog.LoadMaterialImages().Select(item => item.FilePath));
            paths.AddRange(ThemeImageCatalog.LoadAtlasImages().Select(item => item.FilePath));
            return paths.Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
        }
        catch (Exception error)
        {
            System.Diagnostics.Trace.TraceWarning("Reflection preload catalog: {0}", error);
            return [];
        }
    }

    /// <summary>
    /// Только текущее полотно профиля грузится полноразмерно: остальным хватает миниатюр.
    /// </summary>
    private static async Task WarmFullSizeAsync()
    {
        var applied = Core.Registry.DwmRegistryStore.ReadEffectiveString(ReflectionRegistryKey);
        if (string.IsNullOrWhiteSpace(applied)) return;
        try
        {
            if (SvgReflectionRenderer.IsPattern(applied)) return;
            await LoadAsync(applied).ConfigureAwait(true);
        }
        catch (Exception error)
        {
            System.Diagnostics.Trace.TraceWarning("Reflection preload canvas: {0}", error);
        }
    }
}
