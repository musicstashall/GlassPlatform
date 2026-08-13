using GlassSettings.Services;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Controls;

public sealed partial class DesktopWallpaperPreviewTile : UserControl
{
    public const double DesignWidth = 380;
    public const double DesignHeight = 214;
    /// <summary>Ширина левой колонки карточки (слот под превью).</summary>
    public const double LeadSlotWidth = 150;
    public const double LeadInset = 1;
    public const double FixedWidth = LeadSlotWidth - LeadInset * 2;
    public const double FixedHeight = 86;

    private readonly WallpaperPreviewCrossfade _crossfade;
    private string? _lastIdentity;

    public DesktopWallpaperPreviewTile()
    {
        InitializeComponent();
        _crossfade = new WallpaperPreviewCrossfade(
            PreviewImageA,
            PreviewImageB,
            PreviewFallback,
            WallpaperPreviewCrossfade.DefaultCrossfadeDurationMs);
        ApplyFixedSize();
    }

    private void ApplyFixedSize()
    {
        Width = FixedWidth;
        Height = FixedHeight;
        RootBorder.Width = FixedWidth;
        RootBorder.Height = FixedHeight;
    }

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
