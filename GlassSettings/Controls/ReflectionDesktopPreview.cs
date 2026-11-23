using GlassSettings.Services;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.Foundation;

namespace GlassSettings.Controls;

/// <summary>Read-only monitor layout. Wallpaper decode, observation and transitions use shared services.</summary>
internal sealed class ReflectionDesktopPreview : Grid, IDisposable
{
    private readonly List<Image> _reflections = [];
    private readonly List<FlatGlassPreviewLayer> _glassLayers = [];
    private readonly List<Border> _fallbacks = [];
    private readonly Windows.Graphics.RectInt32[] _bounds;
    private readonly List<WallpaperPreviewCrossfade> _wallpapers = [];
    private readonly DesktopEnvironmentWatcher? _watcher;
    private bool _disposed;
    private int _wallpaperRevision;
    public int DesktopWidth { get; }
    public int DesktopHeight { get; }

    public ReflectionDesktopPreview()
    {
        // FindAll projects an IReadOnlyList whose WinRT IEnumerable QI can fail
        // on some Windows App SDK runtimes. Use Count/indexer, then LINQ only
        // on the managed snapshot (never on the projected collection).
        var displays = DisplayArea.FindAll();
        var bounds = new Windows.Graphics.RectInt32[displays.Count];
        for (int i = 0; i < bounds.Length; i++)
            bounds[i] = displays[i].OuterBounds;
        _bounds = bounds;
        if (bounds.Length == 0) throw new InvalidOperationException("No desktop displays.");
        int left = bounds.Min(b => b.X), top = bounds.Min(b => b.Y);
        DesktopWidth = bounds.Max(b => b.X + b.Width) - left;
        DesktopHeight = bounds.Max(b => b.Y + b.Height) - top;
        var desktop = new Canvas { Width = DesktopWidth, Height = DesktopHeight };
        // Physical desktop coordinates are retained inside one proportionally scaled Viewbox.
        Children.Add(new Viewbox { Child = desktop, Stretch = Stretch.Uniform, MaxHeight = 240 });
        Margin = new Thickness(8, 8, 8, 16);
        foreach (var monitor in bounds)
        {
            var fallback = new Border
            {
                Background = (Brush)Application.Current.Resources["CardBackgroundFillColorDefaultBrush"]
            };
            var a = new Image { Stretch = Stretch.UniformToFill };
            var b = new Image { Stretch = Stretch.UniformToFill };
            var reflection = new Image
            {
                Width = DesktopWidth, Height = DesktopHeight, Stretch = Stretch.Fill
            };
            var reflectionCanvas = new Canvas();
            Canvas.SetLeft(reflection, -(monitor.X - left));
            Canvas.SetTop(reflection, -(monitor.Y - top));
            reflectionCanvas.Children.Add(reflection);
            var layers = new Grid { Width = monitor.Width, Height = monitor.Height };
            layers.Children.Add(fallback); layers.Children.Add(a); layers.Children.Add(b);
            var glass = new FlatGlassPreviewLayer(monitor.Width, monitor.Height);
            // Flat, co-located layers: wallpaper → factual glass → edited reflection.
            layers.Children.Add(glass);
            layers.Children.Add(reflectionCanvas);
            _glassLayers.Add(glass); _fallbacks.Add(fallback);
            // Clip each monitor, including the reflection's viewport into the full canvas.
            layers.Clip = new RectangleGeometry { Rect = new Rect(0, 0, monitor.Width, monitor.Height) };
            double radius = Math.Max(8, DesktopWidth / 600.0 * 8);
            layers.Loaded += (_, _) =>
            {
                var visual = Microsoft.UI.Xaml.Hosting.ElementCompositionPreview.GetElementVisual(layers);
                var rounded = visual.Compositor.CreateRoundedRectangleGeometry();
                rounded.Size = new System.Numerics.Vector2(monitor.Width, monitor.Height);
                rounded.CornerRadius = new System.Numerics.Vector2((float)radius);
                visual.Clip = visual.Compositor.CreateGeometricClip(rounded);
            };
            var card = new Border
            {
                Width = monitor.Width, Height = monitor.Height,
                BorderThickness = new Thickness(Math.Max(1, DesktopWidth / 600.0)),
                BorderBrush = (Brush)Application.Current.Resources["ControlStrongStrokeColorDefaultBrush"],
                CornerRadius = new CornerRadius(radius), Child = layers,
                Shadow = new ThemeShadow(), Translation = new System.Numerics.Vector3(0, 0, 16)
            };
            Canvas.SetLeft(card, monitor.X - left); Canvas.SetTop(card, monitor.Y - top);
            desktop.Children.Add(card);
            _reflections.Add(reflection);
            _wallpapers.Add(new WallpaperPreviewCrossfade(a, b, fallback));
        }
        _watcher = DesktopEnvironmentWatcher.Current;
        if (_watcher is not null) _watcher.Changed += OnEnvironmentChanged;
        Loaded += OnLoaded;
    }

    public void SetReflection(ImageSource source)
    {
        if (_disposed) return;
        foreach (var image in _reflections)
        {
            image.Source = source;
            image.Opacity = CompositionPreviewRegistryStates.RuntimeReflectionOpacity;
        }
    }

    private async void OnLoaded(object sender, RoutedEventArgs e) => await RefreshWallpaperAsync(false);

    private async void OnEnvironmentChanged(object? sender, DesktopEnvironmentChangedEventArgs e)
    {
        if (e.WallpaperChanged || e.ThemeChanged || e.AccentChanged)
            await RefreshWallpaperAsync(true);
    }

    private async Task RefreshWallpaperAsync(bool animate)
    {
        int revision = ++_wallpaperRevision;
        try
        {
            // MainWindow owns invalidation on wallpaper changes. Never create a
            // second watcher, read/decode pipeline or registry wallpaper resolver here.
            DesktopWallpaperImageCache.BeginLoad(DispatcherQueue);
            if (DesktopWallpaperImageCache.LoadTask is { } task) await task;
            if (_disposed || revision != _wallpaperRevision) return;
            var sources = await DesktopWallpaperImageCache.GetMonitorDisplaySourcesAsync(DispatcherQueue);
            if (_disposed || revision != _wallpaperRevision) return;
            var shared = DesktopWallpaperImageCache.GetDisplaySourceOrLoad();
            for (int i = 0; i < _bounds.Length; i++)
            {
                var rect = _bounds[i];
                var monitor = sources.FirstOrDefault(item => item.Monitor.X == rect.X && item.Monitor.Y == rect.Y
                    && item.Monitor.Width == rect.Width && item.Monitor.Height == rect.Height);
                uint background = monitor?.Monitor.BackgroundColorRef ?? 0;
                _fallbacks[i].Background = new SolidColorBrush(Windows.UI.Color.FromArgb(
                    255, (byte)background, (byte)(background >> 8), (byte)(background >> 16)));
                await _glassLayers[i].RefreshAsync(monitor?.Monitor.Path
                    ?? CurrentWallpaperResolver.TryGetDesktopWallpaperPath(), animate);
                if (_disposed || revision != _wallpaperRevision) return;
                // Empty per-monitor path is a real solid background, not a reason
                // to resurrect the cached photo from another monitor.
                _wallpapers[i].Apply(monitor is null ? shared : monitor.Source, animate);
            }
        }
        catch (Exception error)
        {
            System.Diagnostics.Trace.TraceWarning("Reflection wallpaper preview: {0}", error.Message);
        }
    }

    public void Dispose()
    {
        _disposed = true; _wallpaperRevision++;
        Loaded -= OnLoaded;
        if (_watcher is not null) _watcher.Changed -= OnEnvironmentChanged;
        foreach (var transition in _wallpapers) transition.Apply(null, false);
        foreach (var glass in _glassLayers) glass.Dispose();
        foreach (var image in _reflections) image.Source = null;
    }
}
