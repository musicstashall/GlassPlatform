using System.Runtime.InteropServices.WindowsRuntime;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.Graphics.Imaging;
using Windows.Storage.Streams;

namespace GlassSettings.Services;

internal static class ThemeImageUriHelper
{
    public static BitmapImage? TryCreateBitmap(string? path, int decodePixelWidth = 0)
    {
        if (string.IsNullOrWhiteSpace(path) || !File.Exists(path))
            return null;

        try
        {
            var fullPath = Path.GetFullPath(path);
            var liveTranscoded = IsLiveTranscodedWallpaper(fullPath);

            var bitmap = new BitmapImage();
            if (liveTranscoded)
                bitmap.CreateOptions = BitmapCreateOptions.IgnoreImageCache;

            if (decodePixelWidth > 0)
                bitmap.DecodePixelWidth = decodePixelWidth;

            bitmap.UriSource = new Uri(fullPath);
            return bitmap;
        }
        catch
        {
            return null;
        }
    }

    /// <summary>
    /// Гарантированный thumbnail (BitmapDecoder scale) — PixelWidth BitmapImage+DecodePixel*
    /// на WinUI часто остаётся full-res и даёт дрожь при Opacity-анимации.
    /// </summary>
    public static async Task<ImageSource?> TryCreateThumbnailAsync(string? path, int maxEdgePx)
    {
        if (string.IsNullOrWhiteSpace(path) || !File.Exists(path) || maxEdgePx <= 0)
            return null;

        try
        {
            var fullPath = Path.GetFullPath(path);
            SoftwareBitmap? soft = await Task.Run(async () =>
            {
                using var file = File.OpenRead(fullPath);
                using var ras = file.AsRandomAccessStream();
                var decoder = await BitmapDecoder.CreateAsync(ras);
                var srcW = decoder.PixelWidth;
                var srcH = decoder.PixelHeight;
                if (srcW == 0 || srcH == 0)
                    return null;

                var scale = maxEdgePx / (double)Math.Max(srcW, srcH);
                if (scale > 1.0)
                    scale = 1.0;

                var tw = Math.Max(1u, (uint)Math.Round(srcW * scale));
                var th = Math.Max(1u, (uint)Math.Round(srcH * scale));
                var transform = new BitmapTransform
                {
                    ScaledWidth = tw,
                    ScaledHeight = th,
                    InterpolationMode = BitmapInterpolationMode.Linear
                };

                return await decoder.GetSoftwareBitmapAsync(
                    BitmapPixelFormat.Bgra8,
                    BitmapAlphaMode.Premultiplied,
                    transform,
                    ExifOrientationMode.RespectExifOrientation,
                    ColorManagementMode.DoNotColorManage);
            }).ConfigureAwait(true);

            if (soft is null)
                return null;

            var source = new SoftwareBitmapSource();
            await source.SetBitmapAsync(soft);
            return source;
        }
        catch
        {
            return null;
        }
    }

    internal static bool IsLiveTranscodedWallpaper(string fullPath) =>
        fullPath.Contains("TranscodedWallpaper", StringComparison.OrdinalIgnoreCase);
}
