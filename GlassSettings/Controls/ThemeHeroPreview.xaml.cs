using GlassSettings.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Controls;

public sealed partial class ThemeHeroPreview : UserControl
{
    public const double DesignInnerWidth = DesktopWallpaperImageCache.DisplayDecodeWidth;
    public const double DesignInnerHeight = 192;

    private readonly WallpaperPreviewCrossfade _crossfade;
    private string? _lastIdentity;

    public ThemeHeroPreview()
    {
        InitializeComponent();
        _crossfade = new WallpaperPreviewCrossfade(
            PreviewImageA,
            PreviewImageB,
            PreviewFallback,
            WallpaperPreviewCrossfade.DefaultCrossfadeDurationMs);
    }

    /// <summary>Зарезервировано: превью фиксированного размера (340×192 с бордюром).</summary>
    public void SetMaxPreviewWidth(double maxWidth)
    {
    }

    public void ApplyThemeChrome(VisualStyleThemeEntry? entry) =>
        PreviewChrome.ApplyTheme(entry);

    public void RefreshPreview(string? wallpaperPath = null)
    {
        var identity = ResolveIdentity(wallpaperPath);
        if (string.Equals(_lastIdentity, identity, StringComparison.OrdinalIgnoreCase))
            return;

        var animate = _lastIdentity is not null;
        _lastIdentity = identity;
        ApplyPreviewImage(wallpaperPath, animate);
    }

    public void ForceRefreshPreview(string? wallpaperPath = null)
    {
        var identity = ResolveIdentity(wallpaperPath);
        var animate = _lastIdentity is not null;
        _lastIdentity = identity;
        ApplyPreviewImage(wallpaperPath, animate);
    }

    private static string? ResolveIdentity(string? wallpaperPath)
    {
        if (!string.IsNullOrWhiteSpace(wallpaperPath))
        {
            if (wallpaperPath.Contains("TranscodedWallpaper", StringComparison.OrdinalIgnoreCase))
                return CurrentWallpaperResolver.TryGetWallpaperImageColorIdentity() ?? wallpaperPath;

            if (File.Exists(wallpaperPath))
                return $"{wallpaperPath}|{File.GetLastWriteTimeUtc(wallpaperPath).Ticks}";

            return wallpaperPath;
        }

        return CurrentWallpaperResolver.TryGetWallpaperImageColorIdentity()
            ?? CurrentWallpaperResolver.TryGetDesktopWallpaperPath();
    }

    private void ApplyPreviewImage(string? path, bool animate)
    {
        var bitmap = DesktopWallpaperImageCache.GetDisplaySourceOrLoad(path);
        _crossfade.Apply(bitmap, animate);
    }
}
