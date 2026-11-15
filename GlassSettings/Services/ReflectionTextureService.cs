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
        var root = Path.GetFullPath(PlatformPaths.ReflectionCacheDirectory) + Path.DirectorySeparatorChar;
        if (!full.StartsWith(root, StringComparison.OrdinalIgnoreCase)) return path;
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
                return target;

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
            return target;
        }
        finally { Gate.Release(); }
    }

    [DllImport("user32.dll")]
    private static extern int GetSystemMetrics(int index);
}
