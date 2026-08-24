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

    /// <summary>
    /// Синхронный decode в UI-потоке — иконка на сплэше до async UriSource (x86/x64).
    /// </summary>
    public static ImageSource? CreateImageSource()
    {
        var path = ResolveIconPath();
        if (File.Exists(path))
        {
            try
            {
                using var file = File.OpenRead(path);
                using var ra = file.AsRandomAccessStream();
                var bitmap = new BitmapImage();
                bitmap.SetSource(ra);
                return bitmap;
            }
            catch
            {
                try
                {
                    return new BitmapImage(ToFileUri(path));
                }
                catch
                {
                    // fallback ниже
                }
            }
        }

        try
        {
            return new BitmapImage(new Uri("ms-appx:///Assets/app.ico"));
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
