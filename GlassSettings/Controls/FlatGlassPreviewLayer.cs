using GlassSettings.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace GlassSettings.Controls;

/// <summary>The Stylization glass recipe (blur → Win7 glow → tint → opaque overlay), without cascade chrome.</summary>
internal sealed class FlatGlassPreviewLayer : Grid, IDisposable
{
    private readonly WallpaperPreviewCrossfade _blur;
    private readonly WallpaperPreviewCrossfade _glow;
    private readonly Grid _glowHost = new();
    private readonly Border _tint = new();
    private readonly Border _opaque = new()
    {
        Background = new SolidColorBrush(Windows.UI.Color.FromArgb(0x55, 0, 0, 0))
    };
    private int _revision;
    private bool _disposed;
    private readonly int _width;
    private readonly int _height;

    public FlatGlassPreviewLayer(int monitorWidth, int monitorHeight)
    {
        _width = CompositionWallpaperPipelineBuilder.DisplayLayerWidth;
        _height = Math.Max(1, (int)Math.Round(_width * (double)monitorHeight / monitorWidth));
        var blurA = new Image { Stretch = Stretch.Fill };
        var blurB = new Image { Stretch = Stretch.Fill };
        var glowA = new Image { Stretch = Stretch.Fill };
        var glowB = new Image { Stretch = Stretch.Fill };
        _blur = new WallpaperPreviewCrossfade(blurA, blurB, new Border());
        _glow = new WallpaperPreviewCrossfade(glowA, glowB, new Border());
        _glowHost.Children.Add(glowA); _glowHost.Children.Add(glowB);
        Children.Add(blurA); Children.Add(blurB); Children.Add(_glowHost);
        Children.Add(_tint); Children.Add(_opaque);
        IsHitTestVisible = false;
    }

    public async Task RefreshAsync(string? wallpaperPath, bool animate)
    {
        if (_disposed) return;
        int revision = ++_revision;
        var state = CompositionPreviewRegistryStates.BuildRuntimeActive();
        var payload = await Task.Run(() => CompositionWallpaperPipelineBuilder.Build(
            wallpaperPath, _width, _height, state));
        if (_disposed || revision != _revision) return;
        // One snapshot and apply order, exactly as Stylization. No separate blur,
        // glass-color policy, afterglow compositor, wallpaper decoder or watcher.
        _blur.Apply(payload.BlurBgra is { } blur
            ? CompositionWallpaperPipelineBuilder.CreateBitmap(payload.Gaussian.Width, payload.Gaussian.Height, blur) : null, animate);
        bool showGlow = state.Win7Mode && state.ShowGlow && payload.GlowBgra is not null;
        _glowHost.Visibility = showGlow ? Visibility.Visible : Visibility.Collapsed;
        _glow.Apply(showGlow
            ? CompositionWallpaperPipelineBuilder.CreateBitmap(payload.Gaussian.Width, payload.Gaussian.Height, payload.GlowBgra!) : null, animate);
        uint tint = state.TintArgb;
        _tint.Background = new SolidColorBrush(Windows.UI.Color.FromArgb(
            (byte)(tint >> 24), (byte)(tint >> 16), (byte)(tint >> 8), (byte)tint));
        _opaque.Visibility = state.OpaqueBlend ? Visibility.Visible : Visibility.Collapsed;
    }

    public void Dispose()
    {
        _disposed = true; _revision++;
        _blur.Apply(null, false); _glow.Apply(null, false);
    }
}
