using System.Runtime.InteropServices;
using DrawingBitmap = System.Drawing.Bitmap;
using DrawingCompositingQuality = System.Drawing.Drawing2D.CompositingQuality;
using DrawingImageLockMode = System.Drawing.Imaging.ImageLockMode;
using DrawingInterpolationMode = System.Drawing.Drawing2D.InterpolationMode;
using DrawingLinearGradientBrush = System.Drawing.Drawing2D.LinearGradientBrush;
using DrawingPixelFormat = System.Drawing.Imaging.PixelFormat;
using DrawingPixelOffsetMode = System.Drawing.Drawing2D.PixelOffsetMode;
using DrawingRectangle = System.Drawing.Rectangle;
using DrawingSmoothingMode = System.Drawing.Drawing2D.SmoothingMode;
using GlassSettings.Controls;
using GlassSettings.Core.Dwm;

namespace GlassSettings.Services;

/// <summary>Blur/wallpaper pipeline для превью «Композиция» — shared builder для cascade и prewarm cache.</summary>
internal static class CompositionWallpaperPipelineBuilder
{
    public readonly record struct BlurBuffer(int Width, int Height, byte[] Pixels);

    public readonly record struct BlurPipelineKey(
        string? WallpaperIdentity,
        int BlurRadius,
        bool Win7Mode,
        uint BlurBalancePercent,
        uint Win7AfterglowBgr,
        uint Win7AfterglowBalancePercent);

    public readonly record struct PipelineSnapshot(
        BlurBuffer DisplayWallpaper,
        BlurBuffer Sharp,
        BlurBuffer Gaussian,
        byte[]? BlurBgra,
        byte[]? GlowBgra);

    public const int DisplayLayerWidth = 340;
    public const int DisplayLayerHeight = 192;
    public const int BlurLayerWidth = 316;
    public const int BlurLayerHeight = 168;

    private const int MaxBlurRadius = 30;
    private const int MaxKernelRadius = 24;

    public static BlurPipelineKey BuildKey(
        DwmCompositionCascadePreview.PreviewState state,
        string? wallpaperPath) =>
        new(
            ResolveWallpaperIdentity(wallpaperPath),
            Math.Clamp(state.BlurRadius, 0, MaxBlurRadius),
            state.Win7Mode,
            state.Win7Mode ? Math.Min(state.BlurBalancePercent, 100u) : 0,
            state.Win7Mode ? state.Win7AfterglowBgr : 0,
            state.Win7Mode ? Math.Min(state.Win7AfterglowBalancePercent, 100u) : 0);

    public static BlurBuffer BuildSharpBuffer(string? wallpaperPath, int width, int height) =>
        BuildBlurBuffer(wallpaperPath, width, height, kernelRadius: 0);

    public static BlurBuffer BuildGaussianFromSharp(BlurBuffer sharp, int blurRadius)
    {
        var scratch = new byte[sharp.Pixels.Length];
        Buffer.BlockCopy(sharp.Pixels, 0, scratch, 0, scratch.Length);
        var kernelRadius = ComputeKernelRadius(blurRadius);
        if (kernelRadius > 0)
            ApplyGaussianBlur(scratch, sharp.Width, sharp.Height, kernelRadius);
        return new BlurBuffer(sharp.Width, sharp.Height, scratch);
    }

    public static PipelineSnapshot Build(
        string? wallpaperPath,
        int width,
        int height,
        DwmCompositionCascadePreview.PreviewState state)
    {
        var blurRadius = Math.Clamp(state.BlurRadius, 0, MaxBlurRadius);
        var fileBytes = ReadWallpaperFileBytes(wallpaperPath);

        var displayWallpaper = fileBytes is { Length: > 0 }
            ? BuildScaledFromBytes(fileBytes, DisplayLayerWidth, DisplayLayerHeight)
            : BuildFallbackBuffer(DisplayLayerWidth, DisplayLayerHeight);

        var sharp = fileBytes is { Length: > 0 }
            ? BuildScaledFromBytes(fileBytes, width, height)
            : BuildFallbackBuffer(width, height);

        var scratch = new byte[sharp.Pixels.Length];
        Buffer.BlockCopy(sharp.Pixels, 0, scratch, 0, scratch.Length);

        var kernelRadius = ComputeKernelRadius(blurRadius);
        if (kernelRadius > 0)
            ApplyGaussianBlur(scratch, sharp.Width, sharp.Height, kernelRadius);

        var gaussian = new BlurBuffer(sharp.Width, sharp.Height, scratch);

        byte[]? blurBgra;
        byte[]? glowBgra = null;

        if (state.Win7Mode)
        {
            var balance = Math.Min(state.BlurBalancePercent, 100u);
            var blurBalance = balance / 100f;
            blurBgra = Win7AeroPreviewCompositor.BuildBlurStage(
                gaussian.Pixels,
                gaussian.Width,
                gaussian.Height,
                blurBalance,
                afterglowBalance: 0f);

            var (afterglowR, afterglowG, afterglowB) = ResolveWin7AfterglowRgb(state);
            glowBgra = Win7AeroPreviewCompositor.BuildAfterglowStage(
                gaussian.Pixels,
                gaussian.Width,
                gaussian.Height,
                afterglowR,
                afterglowG,
                afterglowB);
        }
        else
        {
            blurBgra = gaussian.Pixels;
        }

        return new PipelineSnapshot(displayWallpaper, sharp, gaussian, blurBgra, glowBgra);
    }

    private static string? ResolveWallpaperIdentity(string? wallpaperPath) =>
        CurrentWallpaperResolver.TryGetWallpaperImageColorIdentity() ?? wallpaperPath;

    private static (float R, float G, float B) ResolveWin7AfterglowRgb(
        DwmCompositionCascadePreview.PreviewState state)
    {
        var balance = Math.Min(state.Win7AfterglowBalancePercent, 100u) / 100f;
        var bgr = state.Win7AfterglowBgr & 0x00FFFFFFu;
        var b = (bgr & 0xFF) / 255f * balance;
        var g = ((bgr >> 8) & 0xFF) / 255f * balance;
        var r = ((bgr >> 16) & 0xFF) / 255f * balance;
        return (r, g, b);
    }

    private static int ComputeKernelRadius(int sliderRadius)
    {
        var radius = Math.Clamp(sliderRadius, 0, MaxBlurRadius);
        if (radius <= 0)
            return 0;

        var deviation = Math.Clamp((radius * 10 + 2) / 3, 0, 100);
        var sigma = deviation / 10.0;
        return Math.Clamp((int)Math.Ceiling(sigma * 3.0), 1, MaxKernelRadius);
    }

    internal static byte[]? ReadWallpaperFileBytes(string? wallpaperPath)
    {
        if (string.IsNullOrWhiteSpace(wallpaperPath) || !File.Exists(wallpaperPath))
            return null;

        try
        {
            var fullPath = Path.GetFullPath(wallpaperPath);
            using var stream = new FileStream(fullPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
            using var memory = new MemoryStream();
            stream.CopyTo(memory);
            return memory.ToArray();
        }
        catch
        {
            return null;
        }
    }

    private static BlurBuffer BuildScaledFromBytes(byte[] fileBytes, int width, int height)
    {
        using var stream = new MemoryStream(fileBytes);
        using var surface = CreateSourceBitmapFromStream(stream, width, height);
        return new BlurBuffer(width, height, ReadBgraPixels(surface));
    }

    private static BlurBuffer BuildFallbackBuffer(int width, int height)
    {
        using var surface = CreateFallbackBitmap(width, height);
        return new BlurBuffer(width, height, ReadBgraPixels(surface));
    }

    private static DrawingBitmap CreateSourceBitmapFromStream(Stream stream, int width, int height)
    {
        var target = new DrawingBitmap(width, height, DrawingPixelFormat.Format32bppArgb);
        using var graphics = System.Drawing.Graphics.FromImage(target);
        ApplyHighQualityGraphicsSettings(graphics);

        using var fileSource = new DrawingBitmap(stream);
        graphics.DrawImage(fileSource, new DrawingRectangle(0, 0, width, height));
        return target;
    }

    private static DrawingBitmap CreateFallbackBitmap(int width, int height)
    {
        var target = new DrawingBitmap(width, height, DrawingPixelFormat.Format32bppArgb);
        using var graphics = System.Drawing.Graphics.FromImage(target);
        ApplyHighQualityGraphicsSettings(graphics);

        using var fallback = new DrawingLinearGradientBrush(
            new DrawingRectangle(0, 0, width, height),
            System.Drawing.Color.FromArgb(0xFF, 0x39, 0x57, 0x72),
            System.Drawing.Color.FromArgb(0xFF, 0x25, 0x2F, 0x3A),
            42f);
        graphics.FillRectangle(fallback, 0, 0, width, height);
        return target;
    }

    private static BlurBuffer BuildBlurBuffer(string? wallpaperPath, int width, int height, int kernelRadius)
    {
        using var surface = CreateSourceBitmap(wallpaperPath, width, height);
        var pixels = ReadBgraPixels(surface);
        if (kernelRadius > 0)
            ApplyGaussianBlur(pixels, width, height, kernelRadius);
        return new BlurBuffer(width, height, pixels);
    }

    private static DrawingBitmap CreateSourceBitmap(string? wallpaperPath, int width, int height)
    {
        if (!string.IsNullOrWhiteSpace(wallpaperPath) && File.Exists(wallpaperPath))
        {
            var fileBytes = ReadWallpaperFileBytes(wallpaperPath);
            if (fileBytes is { Length: > 0 })
            {
                using var stream = new MemoryStream(fileBytes);
                return CreateSourceBitmapFromStream(stream, width, height);
            }

            var target = new DrawingBitmap(width, height, DrawingPixelFormat.Format32bppArgb);
            using var graphics = System.Drawing.Graphics.FromImage(target);
            ApplyHighQualityGraphicsSettings(graphics);
            using var fileSource = new DrawingBitmap(wallpaperPath);
            graphics.DrawImage(fileSource, new DrawingRectangle(0, 0, width, height));
            return target;
        }

        return CreateFallbackBitmap(width, height);
    }

    private static void ApplyHighQualityGraphicsSettings(System.Drawing.Graphics graphics)
    {
        graphics.CompositingQuality = DrawingCompositingQuality.HighQuality;
        graphics.InterpolationMode = DrawingInterpolationMode.HighQualityBicubic;
        graphics.PixelOffsetMode = DrawingPixelOffsetMode.HighQuality;
        graphics.SmoothingMode = DrawingSmoothingMode.HighQuality;
    }

    private static byte[] ReadBgraPixels(DrawingBitmap bitmap)
    {
        var rect = new DrawingRectangle(0, 0, bitmap.Width, bitmap.Height);
        var data = bitmap.LockBits(rect, DrawingImageLockMode.ReadOnly, DrawingPixelFormat.Format32bppArgb);
        try
        {
            var rowWidth = bitmap.Width * 4;
            var output = new byte[rowWidth * bitmap.Height];
            for (var y = 0; y < bitmap.Height; y++)
            {
                var src = data.Stride >= 0
                    ? IntPtr.Add(data.Scan0, y * data.Stride)
                    : IntPtr.Add(data.Scan0, (bitmap.Height - 1 - y) * Math.Abs(data.Stride));
                Marshal.Copy(src, output, y * rowWidth, rowWidth);
            }

            return output;
        }
        finally
        {
            bitmap.UnlockBits(data);
        }
    }

    private static void ApplyGaussianBlur(byte[] pixels, int width, int height, int kernelRadius)
    {
        var radius = Math.Clamp(kernelRadius, 0, MaxKernelRadius);
        if (radius <= 0)
            return;

        var sigma = Math.Max(radius / 3.0, 0.85);
        var kernel = BuildKernel(radius, sigma);
        var temp = new byte[pixels.Length];
        var output = new byte[pixels.Length];

        ConvolveHorizontal(pixels, temp, width, height, kernel, radius);
        ConvolveVertical(temp, output, width, height, kernel, radius);
        Buffer.BlockCopy(output, 0, pixels, 0, pixels.Length);
    }

    private static double[] BuildKernel(int radius, double sigma)
    {
        var size = radius * 2 + 1;
        var kernel = new double[size];
        var sigma2 = sigma * sigma;
        var sum = 0d;

        for (var i = -radius; i <= radius; i++)
        {
            var value = Math.Exp(-(i * i) / (2d * sigma2));
            kernel[i + radius] = value;
            sum += value;
        }

        if (sum <= 0d)
            return kernel;

        for (var i = 0; i < kernel.Length; i++)
            kernel[i] /= sum;

        return kernel;
    }

    private static void ConvolveHorizontal(
        byte[] source,
        byte[] destination,
        int width,
        int height,
        double[] kernel,
        int radius)
    {
        for (var y = 0; y < height; y++)
        {
            for (var x = 0; x < width; x++)
            {
                for (var channel = 0; channel < 4; channel++)
                {
                    var sum = 0d;
                    for (var k = -radius; k <= radius; k++)
                    {
                        var sampleX = Math.Clamp(x + k, 0, width - 1);
                        var sampleIndex = ((y * width + sampleX) * 4) + channel;
                        sum += source[sampleIndex] * kernel[k + radius];
                    }

                    destination[((y * width + x) * 4) + channel] = ClampToByte(sum);
                }
            }
        }
    }

    private static void ConvolveVertical(
        byte[] source,
        byte[] destination,
        int width,
        int height,
        double[] kernel,
        int radius)
    {
        for (var y = 0; y < height; y++)
        {
            for (var x = 0; x < width; x++)
            {
                for (var channel = 0; channel < 4; channel++)
                {
                    var sum = 0d;
                    for (var k = -radius; k <= radius; k++)
                    {
                        var sampleY = Math.Clamp(y + k, 0, height - 1);
                        var sampleIndex = ((sampleY * width + x) * 4) + channel;
                        sum += source[sampleIndex] * kernel[k + radius];
                    }

                    destination[((y * width + x) * 4) + channel] = ClampToByte(sum);
                }
            }
        }
    }

    private static byte ClampToByte(double value)
    {
        var rounded = (int)Math.Round(value);
        if (rounded < 0)
            return 0;
        if (rounded > 255)
            return 255;
        return (byte)rounded;
    }
}
