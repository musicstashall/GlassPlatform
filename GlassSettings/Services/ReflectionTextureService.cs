namespace GlassSettings.Services;

using System.Runtime.InteropServices.WindowsRuntime;
using System.Security.Cryptography;
using Microsoft.UI.Xaml.Media.Imaging;
using GlassSettings.Core.Platform;
using Windows.Graphics.Imaging;

/// <summary>Renders PNG tiles/SVG to one published canvas per display profile; native tile pixels unless explicitly scaled.</summary>
public static class ReflectionTextureService
{
    private static readonly SemaphoreSlim Gate = new(1, 1);

    public static bool IsTexture(string path) => string.Equals(
        Path.GetFileName(Path.GetDirectoryName(path)), "Textured", StringComparison.OrdinalIgnoreCase);

    public static string ResolveSource(string path)
    {
        if (!Path.IsPathFullyQualified(path)) return path;
        var full = Path.GetFullPath(path);
        var directory = Path.GetFullPath(PlatformPaths.ReflectionAppliedDirectory);
        if (!string.Equals(Path.GetDirectoryName(full), directory, StringComparison.OrdinalIgnoreCase))
            return path;
        var state = ReflectionRenderState.Read();
        if (state is null || !string.Equals(Path.GetFullPath(state.Output), full, StringComparison.OrdinalIgnoreCase))
            return path;
        // Only the current publication's explicit source; no legacy paths or filename aliases.
        return ThemeImageCatalog.LoadReflectionImages().FirstOrDefault(item =>
            string.Equals(Path.GetFullPath(item.FilePath), Path.GetFullPath(state.Source),
                StringComparison.OrdinalIgnoreCase))?.FilePath ?? path;
    }

    public static async Task<string> PrepareAsync(string source, ReflectionRenderSettings? settings = null, string? expectedProfile = null)
    {
        // Resolve the current published canvas back to its recorded source.
        source = ResolveSource(source);
        var isSvg = SvgReflectionRenderer.IsPattern(source);
        if (!IsTexture(source) && !isSvg) return source;
        settings = (settings ?? ReflectionRenderState.Settings).Normalize();
        await Gate.WaitAsync();
        string? stagingDirectory = null;
        try
        {
            var profile = ReflectionDisplayProfile.Capture();
            if (expectedProfile is not null && expectedProfile != profile.Key)
                throw new IOException("Display configuration changed; reopen reflection settings.");
            int width = profile.Width, height = profile.Height;
            if (width <= 0 || height <= 0 || width > 16384 || height > 16384 ||
                (long)width * height > 33554432)
                throw new InvalidOperationException("Unsupported reflection canvas size.");
            var sourceHash = Convert.ToHexString(SHA256.HashData(await File.ReadAllBytesAsync(source)));
            var saved = ReflectionRenderState.Read();
            if (saved is not null && saved.Profile == profile.Key && saved.Source == source && saved.SourceHash == sourceHash
                && saved.Width == width && saved.Height == height && saved.Settings.Normalize() == settings)
                return saved.Output;
            var png = await RenderConfiguredAsync(source, width, height, settings);
            if (sourceHash != Convert.ToHexString(SHA256.HashData(await File.ReadAllBytesAsync(source))))
                throw new IOException("Reflection source changed during generation.");
            var directory = Directory.CreateTempSubdirectory("GlassPlatform-Reflection-").FullName;
            stagingDirectory = directory;
            var target = Path.Combine(directory, profile.Key + ".png");
            var temporary = target + "." + Guid.NewGuid().ToString("N") + ".tmp";
            try
            {
                await File.WriteAllBytesAsync(temporary, png);
                File.Move(temporary, target, overwrite: true);
            }
            finally { if (File.Exists(temporary)) File.Delete(temporary); }
            // Cross-process exclusion with host reconcile; acquire/release on
            // this thread only, after all asynchronous generation has completed.
            using var publication = new Mutex(false, ReflectionDisplayProfile.PublicationMutex);
            bool held;
            try { held = publication.WaitOne(TimeSpan.FromSeconds(10)); }
            catch (AbandonedMutexException) { held = true; }
            if (!held) throw new IOException("Reflection profile publication is busy.");
            try
            {
                if (ReflectionDisplayProfile.Capture().Key != profile.Key)
                    throw new IOException("Display configuration changed during reflection generation.");
                ReflectionRenderState.Invalidate(profile.Key);
                var output = Publish(target);
                ReflectionRenderState.Save(source, output, width, height, sourceHash,
                    Convert.ToHexString(SHA256.HashData(png)), settings, profile.Key);
                return output;
            }
            finally { publication.ReleaseMutex(); }
        }
        finally
        {
            try
            {
                if (stagingDirectory is not null && Directory.Exists(stagingDirectory))
                    Directory.Delete(stagingDirectory, recursive: true);
            }
            catch (IOException error) { System.Diagnostics.Trace.TraceWarning("Reflection staging cleanup: {0}", error); }
            catch (UnauthorizedAccessException error) { System.Diagnostics.Trace.TraceWarning("Reflection staging cleanup: {0}", error); }
            finally { Gate.Release(); }
        }
    }

    public static async Task<byte[]> RenderConfiguredAsync(string source, int width, int height,
        ReflectionRenderSettings settings)
    {
        settings = settings.Normalize();
        // Keep the disabled recipe in HKCU, but generate a neutral native canvas.
        if (!settings.Enabled) settings = new ReflectionRenderSettings();
        bool isSvg = SvgReflectionRenderer.IsPattern(source);
        if (!IsTexture(source) && !isSvg) throw new InvalidOperationException("Not a generated reflection.");
        if (width <= 0 || height <= 0 || width > 16384 || height > 16384 || (long)width * height > 33554432)
            throw new InvalidOperationException("Unsupported reflection canvas size.");
        if (isSvg && new FileInfo(source).Length > 262144)
            throw new InvalidDataException("SVG source exceeds 256 KiB.");
        var bytes = await File.ReadAllBytesAsync(source);
        if (isSvg) bytes = await Task.Run(() => SvgReflectionRenderer.RenderPng(bytes, width, height));
        using var memory = new MemoryStream(bytes);
        using var input = memory.AsRandomAccessStream();
        var decoder = await BitmapDecoder.CreateAsync(input);
        if (!isSvg && (decoder.PixelWidth > 1024 || decoder.PixelHeight > 1024))
            throw new InvalidDataException("Reflection tile exceeds 1024 pixels.");
        int sw = checked((int)decoder.PixelWidth), sh = checked((int)decoder.PixelHeight);
        if (sw <= 0 || sh <= 0) throw new InvalidDataException("Empty reflection image.");
        var pixels = (await decoder.GetPixelDataAsync(BitmapPixelFormat.Bgra8, BitmapAlphaMode.Straight,
            new BitmapTransform(), ExifOrientationMode.IgnoreExifOrientation,
            ColorManagementMode.DoNotColorManage)).DetachPixelData();
        var canvas = await Task.Run(() =>
        {
            ReflectionPixelEffects.Colorize(pixels, settings.ColorRgb);
            if (isSvg)
            {
                if (settings.ScalePercent != 100)
                    pixels = ReflectionPixelEffects.Resize(pixels, sw, sh, width, height, settings.ScalePercent / 100.0);
                ReflectionPixelEffects.ApplyAlpha(pixels, width, height, settings, wrap: false);
                return pixels;
            }
            int tw = Math.Max(1, (int)Math.Round(sw * settings.ScalePercent / 100.0));
            int th = Math.Max(1, (int)Math.Round(sh * settings.ScalePercent / 100.0));
            var tile = ReflectionPixelEffects.Resize(pixels, sw, sh, tw, th);
            // Blur the periodic source cell first, but add noise to the full canvas.
            ReflectionPixelEffects.ApplyAlpha(tile, tw, th,
                settings with { NoisePercent = 0, OpacityPercent = 100 }, wrap: true);
            var repeated = RepeatPixels(tile, tw, th, width, height);
            ReflectionPixelEffects.ApplyAlpha(repeated, width, height,
                settings with { Softness = 0 }, wrap: false);
            return repeated;
        });
        using var encoded = new Windows.Storage.Streams.InMemoryRandomAccessStream();
        var encoder = await BitmapEncoder.CreateAsync(BitmapEncoder.PngEncoderId, encoded);
        encoder.SetPixelData(BitmapPixelFormat.Bgra8, BitmapAlphaMode.Straight,
            (uint)width, (uint)height, 96, 96, canvas);
        await encoder.FlushAsync();
        encoded.Seek(0);
        using var reader = new Windows.Storage.Streams.DataReader(encoded.GetInputStreamAt(0));
        await reader.LoadAsync(checked((uint)encoded.Size));
        var result = new byte[checked((int)encoded.Size)];
        reader.ReadBytes(result);
        return result;
    }

    /// <summary>Preview uses the same native-pixel repetition as the applied canvas.</summary>
    public static async Task<WriteableBitmap> CreatePreviewAsync(string source)
    {
        var file = await Windows.Storage.StorageFile.GetFileFromPathAsync(Path.GetFullPath(source));
        using var input = await file.OpenReadAsync();
        var decoder = await BitmapDecoder.CreateAsync(input);
        if (decoder.PixelWidth == 0 || decoder.PixelHeight == 0 ||
            decoder.PixelWidth > 1024 || decoder.PixelHeight > 1024)
            throw new InvalidOperationException("Unsupported reflection preview tile size.");
        // XAML WriteableBitmap expects premultiplied BGRA; PNG generation uses straight alpha.
        var pixels = await decoder.GetPixelDataAsync(BitmapPixelFormat.Bgra8, BitmapAlphaMode.Premultiplied,
            new BitmapTransform(), ExifOrientationMode.IgnoreExifOrientation, ColorManagementMode.DoNotColorManage);
        const int width = 148, height = 92;
        var repeated = RepeatPixels(pixels.DetachPixelData(), (int)decoder.PixelWidth,
            (int)decoder.PixelHeight, width, height);
        var bitmap = new WriteableBitmap(width, height);
        using (var stream = bitmap.PixelBuffer.AsStream())
            await stream.WriteAsync(repeated, 0, repeated.Length);
        bitmap.Invalidate();
        return bitmap;
    }

    private static byte[] RepeatPixels(byte[] tile, int tw, int th, int width, int height)
    {
        var result = new byte[checked(width * height * 4)];
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x += tw)
                Buffer.BlockCopy(tile, (y % th) * tw * 4, result, (y * width + x) * 4,
                    Math.Min(tw, width - x) * 4);
        return result;
    }

    private static string Publish(string cachedImage)
    {
        // Check the encoded size before replacing this profile’s canvas.
        if (new FileInfo(cachedImage).Length is <= 0 or > 67108864)
            throw new InvalidOperationException("Reflection PNG exceeds the OpenGlass file size limit.");
        var directory = Path.GetFullPath(PlatformPaths.ReflectionAppliedDirectory);
        var target = Path.Combine(directory, Path.GetFileName(cachedImage));
        if (target.Length >= 260)
            throw new IOException("Reflection output path exceeds the OpenGlass MAX_PATH limit.");
        // Never follow junctions/symlinks in a shared, writable directory.
        for (var parent = new DirectoryInfo(directory); parent is not null; parent = parent.Parent)
            if (parent.Exists && (parent.Attributes & FileAttributes.ReparsePoint) != 0)
                throw new IOException("Reflection output directory must not be a reparse point.");
        Directory.CreateDirectory(directory);
        // Other profiles own their own PNGs. Never inventory/delete them here.
        if (File.Exists(target) && (File.GetAttributes(target) & FileAttributes.ReparsePoint) != 0)
            throw new IOException("Reflection target must not be a reparse point.");
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

}
