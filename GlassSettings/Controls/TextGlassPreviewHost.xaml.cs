using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.WindowsRuntime;
using GlassSettings.Core.Layout;
using GlassSettings.Core.Registry;
using GlassSettings.Core.Text;
using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media.Imaging;

namespace GlassSettings.Controls;

/// <summary>Hero-превью «Текст и шрифты»: каскад обои + стекло Композиции, ink внутри карточки стекла.</summary>
public sealed partial class TextGlassPreviewHost : UserControl
{
    private int _textPaintGeneration;
    private CancellationTokenSource? _textPaintCts;
    private DispatcherQueueTimer? _wallpaperDebounceTimer;
    private GlassInkCapsuleModel? _lastInkCapsule;

    public TextGlassPreviewHost()
    {
        InitializeComponent();
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
    }

    public int PreviewWidth => (int)HeroPreviewLayoutMetrics.OuterWidth;

    public int PreviewHeight => (int)HeroPreviewLayoutMetrics.OuterHeight;

    /// <summary>model — для совместимости с TextAndFontsPage; hero рисует только ink (без glow).</summary>
    public void ApplyGlowState(
        GlobalTextGlowModel model,
        GlassInkCapsuleModel? inkCapsule,
        TextPreviewRefreshScope scope)
    {
        _lastInkCapsule = inkCapsule;
        _ = RepaintTextLayerAsync(scope, inkCapsule);
    }

    private async Task RepaintTextLayerAsync(TextPreviewRefreshScope scope, GlassInkCapsuleModel? inkCapsule)
    {
        _textPaintCts?.Cancel();
        _textPaintCts?.Dispose();
        _textPaintCts = new CancellationTokenSource();
        var token = _textPaintCts.Token;
        var generation = ++_textPaintGeneration;

        var sampleText = GlassSettingsResources.Get("Text_PreviewSample");
        var isDark = ThemeRegistry.IsDarkMode();
        var interactive = scope == TextPreviewRefreshScope.LiveSlider;
        var (width, height) = Cascade.GetTextOverlayPaintSize();
        var buffer = new byte[width * height * 4];

        // Как в бэкапе: paint на пуле; stdcall-экспорт GlassCore уже WINAPI.
        var hr = await Task.Run(() =>
            TextPreviewNative.PaintToBuffer(
                buffer,
                width,
                height,
                sampleText,
                isDark,
                interactive,
                inkCapsule), token).ConfigureAwait(true);

        if (token.IsCancellationRequested || generation != _textPaintGeneration)
            return;

        if (hr < 0)
            return;

        var bitmap = new WriteableBitmap(width, height);
        using (var stream = bitmap.PixelBuffer.AsStream())
        {
            stream.Seek(0, SeekOrigin.Begin);
            stream.Write(buffer, 0, buffer.Length);
        }

        bitmap.Invalidate();
        Cascade.SetTextOverlay(bitmap);
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        Cascade.Width = PreviewWidth;
        Cascade.Height = PreviewHeight;
        DesktopWallpaperImageCache.BeginLoad(DispatcherQueue);
        HookEnvironmentWatcher();
        ScheduleWallpaperRefresh(debounce: false);
    }

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        _textPaintCts?.Cancel();
        _textPaintCts?.Dispose();
        _textPaintCts = null;
        UnhookEnvironmentWatcher();
        _wallpaperDebounceTimer?.Stop();
    }

    private EventHandler<DesktopEnvironmentChangedEventArgs>? _environmentChangedHandler;

    private void HookEnvironmentWatcher()
    {
        if (_environmentChangedHandler is not null)
            return;

        if (DesktopEnvironmentWatcher.Current is not { } watcher)
        {
            DispatcherQueue.TryEnqueue(DispatcherQueuePriority.Low, HookEnvironmentWatcher);
            return;
        }

        _environmentChangedHandler = (_, e) =>
        {
            if (!IsLoaded)
                return;

            if (e.WallpaperChanged)
                ScheduleWallpaperRefresh(debounce: true);
            else if (e.ThemeChanged || e.AccentChanged)
            {
                ScheduleWallpaperRefresh(debounce: false);
                _ = RepaintTextLayerAsync(TextPreviewRefreshScope.Full, _lastInkCapsule);
            }
        };
        watcher.Changed += _environmentChangedHandler;
    }

    private void UnhookEnvironmentWatcher()
    {
        if (_environmentChangedHandler is null ||
            DesktopEnvironmentWatcher.Current is not { } watcher)
        {
            return;
        }

        watcher.Changed -= _environmentChangedHandler;
        _environmentChangedHandler = null;
    }

    private void ScheduleWallpaperRefresh(bool debounce)
    {
        if (!debounce)
        {
            _ = RefreshBackdropAsync();
            return;
        }

        _wallpaperDebounceTimer ??= DispatcherQueue.CreateTimer();
        _wallpaperDebounceTimer.Interval = TimeSpan.FromMilliseconds(120);
        _wallpaperDebounceTimer.IsRepeating = false;
        _wallpaperDebounceTimer.Tick -= WallpaperDebounceTimer_OnTick;
        _wallpaperDebounceTimer.Tick += WallpaperDebounceTimer_OnTick;
        _wallpaperDebounceTimer.Stop();
        _wallpaperDebounceTimer.Start();
    }

    private void WallpaperDebounceTimer_OnTick(DispatcherQueueTimer sender, object args)
    {
        sender.Tick -= WallpaperDebounceTimer_OnTick;
        _ = RefreshBackdropAsync();
    }

    private async Task RefreshBackdropAsync()
    {
        await Cascade.RefreshWallpaperBlurFromRegistryAsync().ConfigureAwait(true);
    }
}
