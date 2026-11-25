using GlassSettings.Services;
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
    private readonly string?[] _glassWallpaperPaths;
    private readonly List<WallpaperPreviewCrossfade> _wallpapers = [];
    private readonly DesktopEnvironmentWatcher? _watcher;
    private bool _disposed;
    private readonly Viewbox _desktopView;
    private readonly TextBlock _sourceName = new() { FontSize = 12, TextTrimming = TextTrimming.CharacterEllipsis, MaxWidth = 320 };
    private readonly Border _sourceBadge;
    private readonly ProgressRing _loading = new() { IsActive = true, Width = 32, Height = 32 };
    private bool _wallpaperReady, _reflectionReady;
    private void UpdateReadyState()
    {
        if (!_wallpaperReady || !_reflectionReady || _disposed) return;
        _desktopView.Opacity = 1;
        _sourceBadge.Opacity = 1;
        _loading.IsActive = false;
        _loading.Visibility = Visibility.Collapsed;
    }
    private int _wallpaperRevision;
    private int _glassRevision;
    private readonly SemaphoreSlim _glassRefreshGate = new(1, 1);
    private DwmCompositionCascadePreview.PreviewState? _lastGlassState;
    public string ProfileKey { get; }
    public int DesktopWidth { get; }
    public int DesktopHeight { get; }
    public int MonitorCount => _bounds.Length;
    // Multi-display layouts start narrower; the Viewbox still grows to 280 DIP when room allows.

    public ReflectionDesktopPreview()
    {
        System.Diagnostics.Debug.WriteLine("[ReflectionConfigure] preview: EnumDisplayMonitors");
        var profile = ReflectionDisplayProfile.Capture();
        ProfileKey = profile.Key;
        var bounds = profile.Monitors.Select(m => new Windows.Graphics.RectInt32(m.X, m.Y, m.Width, m.Height)).ToArray();
        System.Diagnostics.Debug.WriteLine($"[ReflectionConfigure] preview: monitor snapshot ready, count={bounds.Length}");
        _bounds = bounds;
        _glassWallpaperPaths = new string?[bounds.Length];
        if (bounds.Length == 0) throw new InvalidOperationException("No desktop displays.");
        int left = bounds.Min(b => b.X), top = bounds.Min(b => b.Y);
        DesktopWidth = bounds.Max(b => b.X + b.Width) - left;
        DesktopHeight = bounds.Max(b => b.Y + b.Height) - top;
        var desktop = new Canvas { Width = DesktopWidth, Height = DesktopHeight };
        // Physical desktop coordinates are retained inside one proportionally scaled Viewbox.
        _desktopView = new Viewbox { Child = desktop, Stretch = Stretch.Uniform, MaxHeight = 280, Opacity = 0 };
        // Labels occupy their own rows, so neither name nor resolution can ever
        // sit on top of a monitor rectangle. Opacity (not Visibility) is toggled
        // when ready, so the preview does not jump when the name appears.
        RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        var frame = new Border
        {
            Child = _desktopView, CornerRadius = new CornerRadius(10),
            BorderThickness = new Thickness(1),
            BorderBrush = (Brush)Application.Current.Resources["ControlStrongStrokeColorDefaultBrush"],
            Shadow = new ThemeShadow(), Translation = new System.Numerics.Vector3(0, 0, 16)
        };
        Grid.SetRow(frame, 1);
        Children.Add(frame);
        _sourceBadge = new Border
        {
            Child = _sourceName, HorizontalAlignment = HorizontalAlignment.Left,
            VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 0, 8),
            Padding = new Thickness(10, 4, 10, 4), CornerRadius = new CornerRadius(6),
            BorderThickness = new Thickness(1),
            Background = (Brush)Application.Current.Resources["CardBackgroundFillColorDefaultBrush"],
            BorderBrush = (Brush)Application.Current.Resources["CardStrokeColorDefaultBrush"],
            Opacity = 0, IsHitTestVisible = false
        };
        Grid.SetRow(_sourceBadge, 0);
        Children.Add(_sourceBadge);
        var displayBadge = new Border
        {
            HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Center,
            Margin = new Thickness(0, 8, 0, 0), Padding = new Thickness(6, 2, 6, 2), CornerRadius = new CornerRadius(4),
            Background = (Brush)Application.Current.Resources["CardBackgroundFillColorDefaultBrush"],
            IsHitTestVisible = false, Opacity = .65,
            Child = new TextBlock { Text = profile.Label, FontSize = 10 }
        };
        Grid.SetRow(displayBadge, 2);
        Children.Add(displayBadge);
        Grid.SetRow(_loading, 1);
        Children.Add(_loading);
        Margin = new Thickness(8, 8, 8, 16);
        foreach (var monitor in bounds)
        {
            System.Diagnostics.Debug.WriteLine($"[ReflectionConfigure] preview: create monitor {monitor.X},{monitor.Y} {monitor.Width}x{monitor.Height}");
            var fallback = new Border
            {
                Background = (Brush)Application.Current.Resources["CardBackgroundFillColorDefaultBrush"]
            };
            var a = new Image { Stretch = Stretch.UniformToFill };
            var b = new Image { Stretch = Stretch.UniformToFill };
            var layers = new Grid { Width = monitor.Width, Height = monitor.Height };
            layers.Children.Add(fallback); layers.Children.Add(a); layers.Children.Add(b);
            var glass = new FlatGlassPreviewLayer(monitor.Width, monitor.Height);
            // Flat, co-located layers: wallpaper → factual glass.
            layers.Children.Add(glass);
            _glassLayers.Add(glass); _fallbacks.Add(fallback);
            // No per-monitor clip: a clipped, fractionally scaled edge leaves a dark
            // fringe exactly on the seam between neighbouring displays, which reads
            // as a divider. Each layer covers its own rect and nothing overflows.
            Canvas.SetLeft(layers, monitor.X - left); Canvas.SetTop(layers, monitor.Y - top);
            desktop.Children.Add(layers);
            _wallpapers.Add(new WallpaperPreviewCrossfade(a, b, fallback));
        }
        // One reflection canvas for the whole virtual desktop: no per-monitor
        // copies and no clipping, so the desktop picture stays continuous.
        var reflection = new Image
        {
            Width = DesktopWidth, Height = DesktopHeight, Stretch = Stretch.Fill
        };
        desktop.Children.Add(reflection);
        _reflections.Add(reflection);
        double unit = Math.Max(1, DesktopWidth / 740.0);
        // One stroke per monitor, inset inside its footprint. Nothing is drawn on
        // the shared edge between two monitors and no border sits next to it, so
        // no vertical line can appear at the seam; the gap shows the dialog
        // background. The inset is uniform, so screen proportions are preserved.
        double inset = Math.Max(8 * unit, bounds.Min(m => Math.Min(m.Width, m.Height)) * 0.06);
        var stroke = (Brush)Application.Current.Resources["ControlStrongStrokeColorDefaultBrush"];
        var scrim = (Brush)Application.Current.Resources["LayerFillColorDefaultBrush"];
        for (int i = 0; i < bounds.Length; i++)
        {
            var monitor = bounds[i];
            double width = Math.Max(1, monitor.Width - inset * 2), height = Math.Max(1, monitor.Height - inset * 2);
            var monitorFrame = new Border
            {
                Width = width, Height = height, CornerRadius = new CornerRadius(8 * unit),
                BorderThickness = new Thickness(unit), BorderBrush = stroke, IsHitTestVisible = false
            };
            Canvas.SetLeft(monitorFrame, monitor.X - left + inset); Canvas.SetTop(monitorFrame, monitor.Y - top + inset);
            desktop.Children.Add(monitorFrame);
            var number = new Border
            {
                CornerRadius = new CornerRadius(10 * unit),
                Padding = new Thickness(14 * unit, 2 * unit, 14 * unit, 4 * unit),
                Background = scrim, IsHitTestVisible = false,
                Child = new TextBlock
                {
                    Text = ReflectionDisplayProfile.GetDisplayNumber(profile.Monitors[i], i + 1).ToString(),
                    FontSize = 32 * unit, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
                    Foreground = (Brush)Application.Current.Resources["TextFillColorPrimaryBrush"]
                }
            };
            var numberHost = new Grid { Width = width, Height = height, IsHitTestVisible = false };
            number.HorizontalAlignment = HorizontalAlignment.Center;
            number.VerticalAlignment = VerticalAlignment.Center;
            numberHost.Children.Add(number);
            Canvas.SetLeft(numberHost, monitor.X - left + inset); Canvas.SetTop(numberHost, monitor.Y - top + inset);
            desktop.Children.Add(numberHost);
        }
        _watcher = DesktopEnvironmentWatcher.Current;
        if (_watcher is not null) _watcher.Changed += OnEnvironmentChanged;
        SettingsSession.Current.DirtyChanged += OnSessionChanged;
        Loaded += OnLoaded;
    }

    public void SetSourceLabel(string name) => _sourceName.Text = name;

    public void ShowReflectionFailure()
    {
        // The dialog displays the error text; do not leave an endless spinner.
        _reflectionReady = true;
        UpdateReadyState();
    }

    public void SetReflection(ImageSource source)
    {
        if (_disposed) return;
        foreach (var image in _reflections)
        {
            image.Source = source;
            image.Opacity = CompositionPreviewRegistryStates.RuntimeReflectionOpacity;
        }
        _reflectionReady = true;
        UpdateReadyState();
    }

    private async void OnLoaded(object sender, RoutedEventArgs e) => await RefreshWallpaperAsync(false);

    private async void OnEnvironmentChanged(object? sender, DesktopEnvironmentChangedEventArgs e)
    {
        if (e.WallpaperChanged || e.ThemeChanged || e.AccentChanged)
            await RefreshWallpaperAsync(true);
    }

    private async void OnSessionChanged(object? sender, EventArgs e)
    {
        int revision = ++_glassRevision;
        try
        {
            // Reuse the session notification produced by page operations and the
            // existing debounced HKCU/HKLM registry snapshot watcher.
            await Task.Delay(150);
            if (_disposed || revision != _glassRevision) return;
            await _glassRefreshGate.WaitAsync();
            try
            {
                if (_disposed || revision != _glassRevision) return;
                var state = CompositionPreviewRegistryStates.BuildRuntimeActive();
                foreach (var image in _reflections)
                    image.Opacity = CompositionPreviewRegistryStates.RuntimeReflectionOpacity;
                if (_lastGlassState == state) return;
                for (int i = 0; i < _bounds.Length; i++)
                {
                    await _glassLayers[i].RefreshAsync(_glassWallpaperPaths[i] ?? state.WallpaperPath, true);
                    if (_disposed || revision != _glassRevision) return;
                }
                _lastGlassState = state;
            }
            finally { _glassRefreshGate.Release(); }
        }
        catch (Exception error)
        {
            System.Diagnostics.Debug.WriteLine($"[ReflectionConfigure] live glass refresh failed: {error}");
            System.Diagnostics.Trace.TraceWarning("Reflection live glass: {0}", error);
        }
    }

    private async Task RefreshWallpaperAsync(bool animate)
    {
        int revision = ++_wallpaperRevision;
        try
        {
            // MainWindow owns invalidation on wallpaper changes. Never create a
            // second watcher, read/decode pipeline or registry wallpaper resolver here.
            System.Diagnostics.Debug.WriteLine("[ReflectionConfigure] wallpaper: begin shared load");
            DesktopWallpaperImageCache.BeginLoad(DispatcherQueue);
            if (DesktopWallpaperImageCache.LoadTask is { } task) await task;
            if (_disposed || revision != _wallpaperRevision) return;
            System.Diagnostics.Debug.WriteLine("[ReflectionConfigure] wallpaper: read monitor sources");
            var sources = await DesktopWallpaperImageCache.GetMonitorDisplaySourcesAsync(DispatcherQueue);
            System.Diagnostics.Debug.WriteLine("[ReflectionConfigure] wallpaper: monitor sources ready");
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
                _glassWallpaperPaths[i] = monitor?.Monitor.Path ?? CurrentWallpaperResolver.TryGetDesktopWallpaperPath();
                await _glassLayers[i].RefreshAsync(_glassWallpaperPaths[i], animate);
                if (_disposed || revision != _wallpaperRevision) return;
                // Empty per-monitor path is a real solid background, not a reason
                // to resurrect the cached photo from another monitor.
                _wallpapers[i].Apply(monitor is null ? shared : monitor.Source, animate);
            }
        }
        catch (Exception error)
        {
            System.Diagnostics.Debug.WriteLine($"[ReflectionConfigure] wallpaper failed: {error}");
            System.Diagnostics.Trace.TraceWarning("Reflection wallpaper preview: {0}", error);
        }
        finally
        {
            if (!_disposed && revision == _wallpaperRevision)
            {
                _wallpaperReady = true;
                UpdateReadyState();
            }
        }
    }

    public void Dispose()
    {
        _disposed = true; _wallpaperRevision++; _glassRevision++;
        SettingsSession.Current.DirtyChanged -= OnSessionChanged;
        _loading.IsActive = false;
        Loaded -= OnLoaded;
        if (_watcher is not null) _watcher.Changed -= OnEnvironmentChanged;
        foreach (var transition in _wallpapers) transition.Apply(null, false);
        foreach (var glass in _glassLayers) glass.Dispose();
        foreach (var image in _reflections) image.Source = null;
    }
}
