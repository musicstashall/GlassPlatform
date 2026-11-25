using System.Runtime.InteropServices.WindowsRuntime;
using GlassSettings.Core.Platform;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;

namespace GlassSettings.Services;

internal static class AppIconSourceFactory
{
    public const string RelativeIconPath = "Assets\\app.ico";

    public static string ResolveIconPath()
    {
        var direct = Path.GetFullPath(Path.Combine(PlatformPaths.AppBase, RelativeIconPath));
        if (File.Exists(direct))
            return direct;

        var legacy = Path.GetFullPath(Path.Combine(PlatformPaths.AppBase, "Assets", "AppIcon.ico"));
        return File.Exists(legacy) ? legacy : direct;
    }

    /// <summary>Синхронно декодирует иконку до показа окна, без отложенного UriSource.</summary>
    public static ImageSource? CreateSplashImageSource()
    {
        try
        {
            using var file = File.OpenRead(ResolveIconPath());
            using var stream = file.AsRandomAccessStream();
            // Явный размер отключает отложенный decode до расчёта размера Image (decode-to-render-size).
            var bitmap = new BitmapImage { DecodePixelWidth = 256 };
            bitmap.SetSource(stream);
            return bitmap.PixelWidth > 0 ? bitmap : null;
        }
        catch
        {
            return null;
        }
    }

    public static Uri ToFileUri(string path)
    {
        path = Path.GetFullPath(path);
        return new Uri($"file:///{path.Replace('\\', '/')}");
    }
}
