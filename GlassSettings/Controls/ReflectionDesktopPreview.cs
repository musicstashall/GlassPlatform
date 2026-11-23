using GlassSettings.Services;
using System.Runtime.InteropServices;
using System.ComponentModel;
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
        _sourceBadge.Visibility = Visibility.Visible;
        _loading.IsActive = false;
        _loading.Visibility = Visibility.Collapsed;
    }
    private int _wallpaperRevision;
    private int _glassRevision;
    private readonly SemaphoreSlim _glassRefreshGate = new(1, 1);
    private DwmCompositionCascadePreview.PreviewState? _lastGlassState;
    public int DesktopWidth { get; }
    public int DesktopHeight { get; }

    public ReflectionDesktopPreview()
    {
        System.Diagnostics.Debug.WriteLine("[ReflectionConfigure] preview: EnumDisplayMonitors");
        var bounds = ReadMonitorBounds();
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
        Children.Add(_desktopView);
        _sourceBadge = new Border
        {
            Child = _sourceName, HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(12),
            Padding = new Thickness(10, 4, 10, 4), CornerRadius = new CornerRadius(6),
            BorderThickness = new Thickness(1),
            Background = (Brush)Application.Current.Resources["CardBackgroundFillColorDefaultBrush"],
            BorderBrush = (Brush)Application.Current.Resources["CardStrokeColorDefaultBrush"],
            Visibility = Visibility.Collapsed, IsHitTestVisible = false
        };
        Children.Add(_sourceBadge);
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
            // Do not assign CompositionGeometricClip to a XAML-owned visual.
            // WinUI processes that clip later during rendering and can fail QI
            // with E_NOINTERFACE in UpdateVisualClipAndTransform (stowed failfast).
            // The XAML RectangleGeometry above owns content clipping instead.
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
        SettingsSession.Current.DirtyChanged += OnSessionChanged;
        Loaded += OnLoaded;
    }

    // Avoid DisplayArea.FindAll and its failing WinRT collection projection.
    // EnumDisplayMonitors supplies full monitor rectangles, not working areas.
    private static Windows.Graphics.RectInt32[] ReadMonitorBounds()
    {
        var bounds = new List<Windows.Graphics.RectInt32>();
        MonitorEnumProc callback = (IntPtr monitor, IntPtr hdc, ref MonitorRect rect, IntPtr data) =>
        {
            if (rect.Right > rect.Left && rect.Bottom > rect.Top)
                bounds.Add(new Windows.Graphics.RectInt32(rect.Left, rect.Top,
                    rect.Right - rect.Left, rect.Bottom - rect.Top));
            return true;
        };
        if (!EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, callback, IntPtr.Zero))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot enumerate desktop monitors.");
        return bounds.ToArray();
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct MonitorRect
    {
        public int Left, Top, Right, Bottom;
    }

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private delegate bool MonitorEnumProc(IntPtr monitor, IntPtr hdc, ref MonitorRect rect, IntPtr data);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool EnumDisplayMonitors(IntPtr hdc, IntPtr clip,
        MonitorEnumProc callback, IntPtr data);

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
