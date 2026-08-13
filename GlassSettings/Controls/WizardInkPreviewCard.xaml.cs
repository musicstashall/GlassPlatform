using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices.WindowsRuntime;
using GlassSettings.Core.Layout;
using GlassSettings.Core.Text;
using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.UI;

namespace GlassSettings.Controls;

public sealed partial class WizardInkPreviewCard : UserControl
{
    public static readonly DependencyProperty IsCardSelectedProperty =
        DependencyProperty.Register(
            nameof(IsCardSelected),
            typeof(bool),
            typeof(WizardInkPreviewCard),
            new PropertyMetadata(false, OnIsCardSelectedChanged));

    public static readonly DependencyProperty RenderModeProperty =
        DependencyProperty.Register(
            nameof(RenderMode),
            typeof(uint),
            typeof(WizardInkPreviewCard),
            new PropertyMetadata(1u, OnPaintInputsChanged));

    public static readonly DependencyProperty ProfileProperty =
        DependencyProperty.Register(
            nameof(Profile),
            typeof(GlassInkProfileModel),
            typeof(WizardInkPreviewCard),
            new PropertyMetadata(default(GlassInkProfileModel), OnPaintInputsChanged));

    public static readonly DependencyProperty SampleTextProperty =
        DependencyProperty.Register(
            nameof(SampleText),
            typeof(string),
            typeof(WizardInkPreviewCard),
            new PropertyMetadata(string.Empty, OnPaintInputsChanged));

    public static readonly DependencyProperty IsDarkThemeProperty =
        DependencyProperty.Register(
            nameof(IsDarkTheme),
            typeof(bool),
            typeof(WizardInkPreviewCard),
            new PropertyMetadata(true, OnPaintInputsChanged));

    public static readonly DependencyProperty HostInListViewProperty =
        DependencyProperty.Register(
            nameof(HostInListView),
            typeof(bool),
            typeof(WizardInkPreviewCard),
            new PropertyMetadata(false, OnHostInListViewChanged));

    private int _paintGeneration;
    private CancellationTokenSource? _paintCts;
    private bool _cardPressedEnabled;
    private bool _batchPreviewApplied;
    private bool _isPointerOver;
    private TaskCompletionSource? _paintReadyTcs;
    private long _lastRaiseTicks;
    private Brush? _hoverBackgroundBrush;
    private static readonly Brush TransparentBackgroundBrush = new SolidColorBrush(Color.FromArgb(0, 0, 0, 0));

    /// <summary>Отложить автоматический repaint в Loaded (пакетная вставка в мастере).</summary>
    public bool SuppressLoadedRepaint { get; set; }

    /// <summary>Не запускать repaint при смене Profile/RenderMode (пакетная подстановка preview).</summary>
    public bool SuppressPaintInputRepaint { get; set; }

    /// <summary>Прямой callback клика (надёжнее event внутри ScrollViewer).</summary>
    public Action? PressedCallback { get; set; }

    public event EventHandler<RoutedEventArgs>? CardPressed;

    public WizardInkPreviewCard()
    {
        InitializeComponent();
        Loaded += (_, _) =>
        {
            ApplyHostingChrome();
            ApplyVisualChrome();
            if (!SuppressLoadedRepaint && !_batchPreviewApplied)
                _ = RepaintAsync();
        };
        Unloaded += (_, _) =>
        {
            _paintCts?.Cancel();
            _paintCts?.Dispose();
            _paintCts = null;
        };
    }

    public bool IsCardSelected
    {
        get => (bool)GetValue(IsCardSelectedProperty);
        set => SetValue(IsCardSelectedProperty, value);
    }

    public uint RenderMode
    {
        get => (uint)GetValue(RenderModeProperty);
        set => SetValue(RenderModeProperty, value);
    }

    public GlassInkProfileModel Profile
    {
        get => (GlassInkProfileModel)GetValue(ProfileProperty);
        set => SetValue(ProfileProperty, value);
    }

    public string SampleText
    {
        get => (string)GetValue(SampleTextProperty);
        set => SetValue(SampleTextProperty, value);
    }

    public bool IsDarkTheme
    {
        get => (bool)GetValue(IsDarkThemeProperty);
        set => SetValue(IsDarkThemeProperty, value);
    }

    public bool HostInListView
    {
        get => (bool)GetValue(HostInListViewProperty);
        set => SetValue(HostInListViewProperty, value);
    }

    private void ApplyHostingChrome()
    {
        if (CardChrome.Shadow is null)
            CardChrome.Shadow = new ThemeShadow();

        if (HostInListView)
            EnsureCardPressedHook();

        ApplyVisualChrome();
    }

    /// <summary>Карточка в строке мастера (StackPanel) или в сетке экрана 1.</summary>
    public void EnableCardPressed() => EnsureCardPressedHook();

    private void EnsureCardPressedHook()
    {
        if (_cardPressedEnabled)
            return;

        _cardPressedEnabled = true;
        CardChrome.PointerPressed += CardChrome_OnPointerPressed;
        CardChrome.PointerEntered += CardChrome_OnPointerEntered;
        CardChrome.PointerExited += CardChrome_OnPointerExited;
    }

    private void RaiseCardPressed()
    {
        var now = Stopwatch.GetTimestamp();
        if (now - _lastRaiseTicks < Stopwatch.Frequency / 15)
            return;

        _lastRaiseTicks = now;

        if (PressedCallback is { } callback)
        {
            callback();
            return;
        }

        CardPressed?.Invoke(this, new RoutedEventArgs());
    }

    private static void OnIsCardSelectedChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is WizardInkPreviewCard card)
            card.ApplyVisualChrome();
    }

    private static void OnPaintInputsChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is WizardInkPreviewCard card && !card.SuppressPaintInputRepaint)
            card.RequestRepaint();
    }

    private static void OnHostInListViewChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is WizardInkPreviewCard card)
        {
            card.ApplyHostingChrome();
            card.ApplyVisualChrome();
        }
    }

    public void SetPreviewBitmap(WriteableBitmap bitmap)
    {
        PreviewImage.Source = bitmap;
        _batchPreviewApplied = true;
        SignalPaintReady();
    }

    public void RequestRepaint()
    {
        if (_batchPreviewApplied)
            return;

        EnsurePaintReadyPending();
        _ = RepaintAsync();
    }

    /// <summary>Ожидание первого успешного preview-буфера (или уже готового).</summary>
    public Task WhenPaintedAsync()
    {
        if (PreviewImage.Source is not null)
            return Task.CompletedTask;

        EnsurePaintReadyPending();
        return _paintReadyTcs!.Task;
    }

    private void EnsurePaintReadyPending()
    {
        if (PreviewImage.Source is not null)
            return;

        _paintReadyTcs ??= new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
    }

    private void SignalPaintReady()
    {
        _paintReadyTcs?.TrySetResult();
        _paintReadyTcs = null;
    }

    private void CardChrome_OnPointerPressed(object sender, PointerRoutedEventArgs e)
    {
        RaiseCardPressed();
        e.Handled = true;
    }

    private void CardChrome_OnPointerEntered(object sender, PointerRoutedEventArgs e)
    {
        _isPointerOver = true;
        ApplyVisualChrome();
    }

    private void CardChrome_OnPointerExited(object sender, PointerRoutedEventArgs e)
    {
        _isPointerOver = false;
        ApplyVisualChrome();
    }

    private void EnsureChromeBrushes()
    {
        _hoverBackgroundBrush ??= AppThemeVisualResolver.ResolveThemeBrush("SubtleFillColorSecondaryBrush");
    }

    private void ApplyVisualChrome()
    {
        EnsureChromeBrushes();
        ApplySelectionChrome();
        CardChrome.Background = !IsCardSelected && _isPointerOver && _hoverBackgroundBrush is not null
            ? _hoverBackgroundBrush
            : TransparentBackgroundBrush;
    }

    private void ApplySelectionChrome()
    {
        if (IsCardSelected)
        {
            CardChrome.BorderBrush = AppThemeVisualResolver.ResolveThemeBrush("AccentFillColorDefaultBrush");
            CardChrome.BorderThickness = Application.Current.Resources["GlassSettingsHeroPreviewBorderThickness"] is Thickness thickness
                ? thickness
                : new Thickness(3);
            CardChrome.Translation = new System.Numerics.Vector3(0, 0, _isPointerOver ? 36 : 32);
        }
        else
        {
            CardChrome.BorderBrush = AppThemeVisualResolver.ResolveThemeBrush("CardStrokeColorDefaultBrush");
            CardChrome.BorderThickness = Application.Current.Resources["GlassSettingsTextLayerPreviewBorderThickness"] is Thickness thickness
                ? thickness
                : new Thickness(2);
            CardChrome.Translation = new System.Numerics.Vector3(0, 0, _isPointerOver ? 24 : 16);
        }
    }

    private async Task RepaintAsync()
    {
        if (_batchPreviewApplied)
            return;

        _paintCts?.Cancel();
        _paintCts?.Dispose();
        _paintCts = new CancellationTokenSource();
        var token = _paintCts.Token;
        var generation = ++_paintGeneration;

        var width = HeroPreviewLayoutMetrics.TextOverlayPaintWidth;
        var height = HeroPreviewLayoutMetrics.TextOverlayPaintHeight;
        var sample = string.IsNullOrEmpty(SampleText)
            ? GlassSettingsResources.Get("Text_PreviewSample")
            : SampleText;
        var profile = Profile;
        var renderMode = RenderMode;
        var isDark = IsDarkTheme;
        var buffer = new byte[width * height * 4];

        var hr = await Task.Run(() =>
            TextPreviewNative.PaintProfileToBuffer(
                buffer,
                width,
                height,
                sample,
                isDark,
                interactiveDuringGesture: false,
                renderMode,
                profile), token).ConfigureAwait(true);

        if (token.IsCancellationRequested || generation != _paintGeneration || _batchPreviewApplied)
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
        PreviewImage.Source = bitmap;
        SignalPaintReady();
    }
}
