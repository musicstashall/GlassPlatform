namespace GlassSettings.Services;

using System.Runtime.InteropServices;
using System.Security.Cryptography;
using GlassSettings.Core.Platform;
using Windows.Graphics.Imaging;

/// <summary>Builds a native-pixel reflection canvas; never scales the source tile.</summary>
public static class ReflectionTextureService
{
    private static readonly SemaphoreSlim Gate = new(1, 1);

    public static bool IsTexture(string path) => string.Equals(
        Path.GetFileName(Path.GetDirectoryName(path)), "Textured", StringComparison.OrdinalIgnoreCase);

    public static string ResolveSource(string path)
    {
        if (!Path.IsPathFullyQualified(path)) return path;
        var full = Path.GetFullPath(path);
        var roots = new[] { PlatformPaths.ReflectionCacheDirectory, PlatformPaths.ReflectionAppliedDirectory };
        if (!roots.Any(root => full.StartsWith(Path.GetFullPath(root) + Path.DirectorySeparatorChar,
            StringComparison.OrdinalIgnoreCase))) return path;
        return ThemeImageCatalog.LoadReflectionImages().FirstOrDefault(item =>
            IsTexture(item.FilePath) && string.Equals(Path.GetFileName(item.FilePath),
                Path.GetFileName(path), StringComparison.OrdinalIgnoreCase))?.FilePath ?? path;
    }

    public static async Task<string> PrepareAsync(string source)
    {
        if (!IsTexture(source)) return source;
        await Gate.WaitAsync();
        try
        {
            // GetSystemMetrics returns the entire virtual desktop, not just the primary monitor.
            int width = GetSystemMetrics(78), height = GetSystemMetrics(79);
            if (width <= 0 || height <= 0 || width > 16384 || height > 16384 ||
                (long)width * height > 33554432)
                throw new InvalidOperationException("Unsupported reflection canvas size.");
            var directory = Path.Combine(PlatformPaths.ReflectionCacheDirectory, $"{width}x{height}");
            Directory.CreateDirectory(directory);
            var target = Path.Combine(directory, Path.GetFileName(source));
            var sourceBytes = await File.ReadAllBytesAsync(source);
            var fingerprint = "tile-v1:" + Convert.ToHexString(SHA256.HashData(sourceBytes));
            var stamp = target + ".sha256";
            if (File.Exists(target) && File.Exists(stamp) && await File.ReadAllTextAsync(stamp) == fingerprint)
                return Publish(target);

            using var input = new Windows.Storage.Streams.InMemoryRandomAccessStream();
            using (var writer = new Windows.Storage.Streams.DataWriter(input.GetOutputStreamAt(0)))
            {
                writer.WriteBytes(sourceBytes);
                await writer.StoreAsync();
                writer.DetachStream();
            }
            var decoder = await BitmapDecoder.CreateAsync(input);
            if (decoder.PixelWidth == 0 || decoder.PixelHeight == 0 ||
                decoder.PixelWidth > 1024 || decoder.PixelHeight > 1024)
                throw new InvalidOperationException("Reflection tiles must be at most 1024 x 1024 pixels.");
            var pixels = await decoder.GetPixelDataAsync(BitmapPixelFormat.Bgra8, BitmapAlphaMode.Straight,
                new BitmapTransform(), ExifOrientationMode.IgnoreExifOrientation, ColorManagementMode.DoNotColorManage);
            var tile = pixels.DetachPixelData();
            int tw = checked((int)decoder.PixelWidth), th = checked((int)decoder.PixelHeight);
            var canvas = await Task.Run(() =>
            {
                var result = new byte[checked(width * height * 4)];
                for (int y = 0; y < height; y++)
                    for (int x = 0; x < width; x += tw)
                        Buffer.BlockCopy(tile, (y % th) * tw * 4, result, (y * width + x) * 4,
                            Math.Min(tw, width - x) * 4);
                return result;
            });
            var temporary = target + "." + Guid.NewGuid().ToString("N") + ".tmp";
            try
            {
                using (var stream = new FileStream(temporary, FileMode.CreateNew, FileAccess.ReadWrite))
                using (var random = stream.AsRandomAccessStream())
                {
                    var encoder = await BitmapEncoder.CreateAsync(BitmapEncoder.PngEncoderId, random);
                    encoder.SetPixelData(BitmapPixelFormat.Bgra8, BitmapAlphaMode.Straight,
                        (uint)width, (uint)height, 96, 96, canvas);
                    await encoder.FlushAsync();
                }
                File.Move(temporary, target, overwrite: true);
                await File.WriteAllTextAsync(stamp, fingerprint);
            }
            finally
            {
                if (File.Exists(temporary)) File.Delete(temporary);
            }
            return Publish(target);
        }
        finally { Gate.Release(); }
    }

    private static string Publish(string cachedImage)
    {
        // Check the encoded size before removing the previous applied canvas.
        if (new FileInfo(cachedImage).Length is <= 0 or > 67108864)
            throw new InvalidOperationException("Reflection PNG exceeds the OpenGlass file size limit.");
        var directory = Path.GetFullPath(PlatformPaths.ReflectionAppliedDirectory);
        var target = Path.Combine(directory, Path.GetFileName(cachedImage));
        if (target.Length >= 260)
            throw new IOException("Reflection output path exceeds the OpenGlass MAX_PATH limit.");
        // Never follow junctions/symlinks while cleaning a shared, writable directory.
        for (var parent = new DirectoryInfo(directory); parent is not null; parent = parent.Parent)
            if (parent.Exists && (parent.Attributes & FileAttributes.ReparsePoint) != 0)
                throw new IOException("Reflection output directory must not be a reparse point.");
        Directory.CreateDirectory(directory);
        var files = new List<string>();
        var directories = new List<string>();
        void Inventory(string path)
        {
            foreach (var entry in Directory.EnumerateFileSystemEntries(path))
            {
                var attributes = File.GetAttributes(entry);
                if ((attributes & FileAttributes.ReparsePoint) != 0)
                    throw new IOException("Reflection output contains a reparse point; cleanup refused.");
                if ((attributes & FileAttributes.Directory) != 0)
                {
                    Inventory(entry);
                    directories.Add(entry);
                }
                else files.Add(entry);
            }
        }
        Inventory(directory);
        // Only Presets/Textured is owned by this feature; never clean Presets itself.
        foreach (var file in files) File.Delete(file);
        foreach (var child in directories) Directory.Delete(child);
        var temporary = target + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            File.Copy(cachedImage, temporary, overwrite: false);
            File.Move(temporary, target, overwrite: true);
        }
        finally
        {
            if (File.Exists(temporary)) File.Delete(temporary);
        }
        return target;
    }

    [DllImport("user32.dll")]
    private static extern int GetSystemMetrics(int index);
}
