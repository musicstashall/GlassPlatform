using System.Runtime.InteropServices.WindowsRuntime;
using System.Numerics;
using GlassSettings.Core.Dwm;
using GlassSettings.Core.Registry;
using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;
using Microsoft.UI.Xaml.Media.Imaging;
using UiColor = Windows.UI.Color;
using Windows.Foundation;
using Windows.UI.ViewManagement;

namespace GlassSettings.Controls;

/// <summary>Режим каскадного превью: полная композиция или hero «Текст и шрифты».</summary>
public enum CompositionCascadePreviewKind
{
    Full,
    /// <summary>Каскад обои → карточка стекла (blur+glow+tint+ink внутри), как слой «Стекло» на Стилизации.</summary>
    WallpaperBlurBase,
}

/// <summary>
/// Каскадное превью страницы «Композиция»: Win7 — Обои → Размытие → Свечение → Тинт.
/// Vista inactive: два независимых tint-rect (полный размер, ThemeShadow, смещение по Y).
/// </summary>
public sealed partial class DwmCompositionCascadePreview : UserControl
{
    public readonly record struct PreviewState(
        string? WallpaperPath,
        uint TintArgb,
        bool Win7Mode,
        uint GlowArgb,
        bool ShowGlow,
        uint BlurBalancePercent,
        bool OpaqueBlend,
        int BlurRadius,
        uint Win7TintBgr,
        uint Win7ColorBalancePercent,
        uint Win7AfterglowBgr,
        uint Win7AfterglowBalancePercent,
        bool ShowInactiveSplit,
        uint InactiveTintArgb);

    private readonly record struct BlurBuffer(int Width, int Height, byte[] Pixels);

    private readonly record struct SharpSourceKey(string? WallpaperIdentity, int Width, int Height);

    private readonly record struct GaussianCacheKey(string? WallpaperIdentity, int BlurRadius, int Width, int Height);

    private const int WallpaperCrossfadeMs = WallpaperPreviewCrossfade.DefaultCrossfadeDurationMs;

    private const double BaseWidth = 340;
    private const double BaseHeight = 192;
    private const double Inset = 24;
    private const float LayerDepthStep = 8f;
    private const int MaxBlurRadius = 30;
    private const int SplitModeFadeMs = 150;
    private const int SplitBlurLayerIndex = 1;
    private const int SplitTintLayerIndex = 2;
    private const double SplitTintHalfExtraPx = 20;
    private const double SplitTintHeightBelowBlurPx = Inset * 2;
    private const double SplitInactiveTintBelowBlurTopPx = Inset;
    private const double SplitInactiveTintRightInsetFromBlurPx = Inset;

    public const int TextOverlayPadding = 16;

    private readonly ImageBrush _blurBrush = new() { Stretch = Stretch.UniformToFill };
    private readonly ImageBrush _blurBrushB = new() { Stretch = Stretch.UniformToFill };
    private readonly ImageBrush _glowBrush = new() { Stretch = Stretch.UniformToFill };
    private readonly ImageBrush _glowBrushB = new() { Stretch = Stretch.UniformToFill };
    private readonly ImageBrush _embeddedGlowBrush = new() { Stretch = Stretch.UniformToFill };
    private readonly ImageBrush _embeddedGlowBrushB = new() { Stretch = Stretch.UniformToFill };
    private readonly SolidColorBrush _tintBrush = new(UiColor.FromArgb(0, 0, 0, 0));
    private readonly SolidColorBrush _inactiveTintBrush = new(UiColor.FromArgb(0, 0, 0, 0));
    private readonly WallpaperPreviewCrossfade _wallpaperCrossfade;
    private readonly WallpaperPreviewCrossfade _splitWallpaperCrossfade;
    private readonly UiElementCrossfade _blurCrossfade;
    private readonly UiElementCrossfade _splitBlurCrossfade;
    private readonly UiElementCrossfade _glowCrossfade;
    private readonly UiElementCrossfade _embeddedGlowCrossfade;

    private bool _layoutApplied;
    private bool _splitLayoutApplied;
    private bool _splitModeVisible;
    private bool _skipBlurRecompute;
    private Storyboard? _splitModeStoryboard;

    private PreviewState _lastState;
    private bool _hasState;
    private string? _lastWallpaperPath;
    private string? _lastWallpaperIdentity;
    private bool _hasWallpaperImage;
    private bool _blurLayerAIsFront = true;
    private bool _glowLayerAIsFront = true;
    private int _wallpaperRefreshGeneration;

    private int _pipelineBusyCount;
    private TaskCompletionSource? _pipelineIdleTcs;

    private SharpSourceKey? _sharpSourceKey;
    private BlurBuffer? _sharpSourceCache;
    private int _sharpSourceLoadGeneration;

    private GaussianCacheKey? _gaussianCacheKey;
    private BlurBuffer? _gaussianCache;
    private CompositionWallpaperPipelineBuilder.BlurPipelineKey? _lastBlurPipelineKey;

    private bool _textOverlayFrontA = true;

    public static readonly DependencyProperty PreviewKindProperty =
        DependencyProperty.Register(
            nameof(PreviewKind),
            typeof(CompositionCascadePreviewKind),
            typeof(DwmCompositionCascadePreview),
            new PropertyMetadata(
                CompositionCascadePreviewKind.Full,
                static (d, _) => ((DwmCompositionCascadePreview)d).ApplyPreviewKindChrome()));

    public CompositionCascadePreviewKind PreviewKind
    {
        get => (CompositionCascadePreviewKind)GetValue(PreviewKindProperty);
        set => SetValue(PreviewKindProperty, value);
    }

    private bool IsWallpaperBlurBase => PreviewKind == CompositionCascadePreviewKind.WallpaperBlurBase;

    public DwmCompositionCascadePreview()
    {
        InitializeComponent();
        _wallpaperCrossfade = new WallpaperPreviewCrossfade(
            WallpaperImageA,
            WallpaperImageB,
            WallpaperFallback,
            WallpaperCrossfadeMs);
        _splitWallpaperCrossfade = new WallpaperPreviewCrossfade(
            SplitSharedWallpaperImageA,
            SplitSharedWallpaperImageB,
            SplitSharedWallpaperFallback,
            WallpaperCrossfadeMs);
        _blurCrossfade = new UiElementCrossfade(BlurFillA, BlurFillB, WallpaperCrossfadeMs);
        _splitBlurCrossfade = new UiElementCrossfade(SplitBlurFillA, SplitBlurFillB, WallpaperCrossfadeMs);
        _glowCrossfade = new UiElementCrossfade(GlowFillA, GlowFillB, WallpaperCrossfadeMs);
        _embeddedGlowCrossfade = new UiElementCrossfade(EmbeddedGlowFillA, EmbeddedGlowFillB, WallpaperCrossfadeMs);
        ApplyLayout();
        ApplyLocalizedLabels();
        BlurFillA.Fill = _blurBrush;
        BlurFillB.Fill = _blurBrushB;
        SplitBlurFillA.Fill = _blurBrush;
        SplitBlurFillB.Fill = _blurBrushB;
        TintFill.Fill = _tintBrush;
        EmbeddedTintFill.Fill = _tintBrush;
        SplitLeftTintFill.Fill = _tintBrush;
        SplitRightTintFill.Fill = _inactiveTintBrush;
        GlowFillA.Fill = _glowBrush;
        GlowFillB.Fill = _glowBrushB;
        EmbeddedGlowFillA.Fill = _embeddedGlowBrush;
        EmbeddedGlowFillB.Fill = _embeddedGlowBrushB;
        ApplyPreviewKindChrome();
        Loaded += (_, _) =>
        {
            if (_hasState)
                ApplyState(_lastState, _skipBlurRecompute);
        };
    }

    /// <summary>Каскад обои+стекло из runtime-effective реестра (GlassType Vista/Win7).</summary>
    public void ApplyWallpaperBlurFromRegistry()
    {
        ApplyState(ResolveTextHeroGlassPreviewState());
    }

    /// <summary>Force-refresh каскада стекла после смены wallpaper (shared pipeline + crossfade).</summary>
    public async Task RefreshWallpaperBlurFromRegistryAsync()
    {
        var load = DesktopWallpaperImageCache.LoadTask;
        if (load is not null)
            await load.ConfigureAwait(true);

        _lastWallpaperIdentity = null;
        _lastBlurPipelineKey = null;
        ApplyState(ResolveTextHeroGlassPreviewState());
    }

    /// <summary>Inner rect текстового overlay (карточка стекла − 2×padding).</summary>
    public (int Width, int Height) GetTextOverlayPaintSize()
    {
        var (blurWidth, blurHeight) = GetBlurPaintSize();
        var innerWidth = Math.Max(1, blurWidth - (TextOverlayPadding * 2));
        var innerHeight = Math.Max(1, blurHeight - (TextOverlayPadding * 2));
        return (innerWidth, innerHeight);
    }

    /// <summary>Размер карточки стекла / blur-слоя (316×168).</summary>
    public (int Width, int Height) GetBlurPaintSize()
    {
        var width = BlurLayer.ActualWidth > 0
            ? (int)Math.Round(BlurLayer.ActualWidth)
            : CompositionWallpaperPipelineBuilder.BlurLayerWidth;
        var height = BlurLayer.ActualHeight > 0
            ? (int)Math.Round(BlurLayer.ActualHeight)
            : CompositionWallpaperPipelineBuilder.BlurLayerHeight;
        return (Math.Max(1, width), Math.Max(1, height));
    }

    /// <summary>Как слой «Стекло» на Стилизации: Vista/Win7 по GlassType, без inactive-split.</summary>
    private static PreviewState ResolveTextHeroGlassPreviewState()
    {
        var (vista, win7) = CompositionPreviewRegistryStates.BuildFromRuntimeEffective();
        if ((DwmRegistryStore.ReadEffectiveDword("GlassType") ?? 0) == 1)
            return win7;

        return vista with { ShowInactiveSplit = false };
    }

    /// <summary>Double-buffer swap текстового overlay (только WallpaperBlurBase).</summary>
    public void SetTextOverlay(WriteableBitmap bitmap)
    {
        if (_textOverlayFrontA)
        {
            TextOverlayB.Source = bitmap;
            TextOverlayB.Visibility = Visibility.Visible;
            TextOverlayA.Visibility = Visibility.Collapsed;
            _textOverlayFrontA = false;
        }
        else
        {
            TextOverlayA.Source = bitmap;
            TextOverlayA.Visibility = Visibility.Visible;
            TextOverlayB.Visibility = Visibility.Collapsed;
            _textOverlayFrontA = true;
        }
    }

    private void ApplyPreviewKindChrome()
    {
        if (!IsWallpaperBlurBase)
        {
            TextOverlayHost.Visibility = Visibility.Collapsed;
            EmbeddedGlowFillA.Visibility = Visibility.Collapsed;
            EmbeddedGlowFillB.Visibility = Visibility.Collapsed;
            EmbeddedTintFill.Visibility = Visibility.Collapsed;
            EmbeddedOpaqueOverlay.Visibility = Visibility.Collapsed;
            return;
        }

        // PreviewKind с родителя часто приходит после ctor — размеры/chrome здесь, не в ApplyLayout.
        // Каскад: обои (0) → карточка стекла BlurLayer (1); glow/tint/ink внутри карточки.
        SizeLayer(WallpaperLayer, 0);
        SizeLayer(BlurLayer, 1);
        WallpaperLayer.Translation = new Vector3(0f, 0f, LayerDepthStep * 1);
        BlurLayer.Translation = new Vector3(0f, 0f, LayerDepthStep * 2);

        SplitCascadeHost.Visibility = Visibility.Collapsed;
        SingleCascadeHost.Visibility = Visibility.Visible;
        SingleCascadeHost.Opacity = 1;
        WallpaperLabelPill.Visibility = Visibility.Collapsed;
        BlurLabelPill.Visibility = Visibility.Collapsed;
        GlowLabelPill.Visibility = Visibility.Collapsed;
        TintLabelPill.Visibility = Visibility.Collapsed;

        GlowLayer.Visibility = Visibility.Collapsed;
        TintLayer.Visibility = Visibility.Collapsed;

        BlurLayer.BorderThickness = new Thickness(1);
        BlurLayer.BorderBrush = new SolidColorBrush(UiColor.FromArgb(0x40, 0xFF, 0xFF, 0xFF));
        if (BlurLayer.Shadow is null)
            BlurLayer.Shadow = new ThemeShadow();

        EmbeddedTintFill.Visibility = Visibility.Visible;
        TextOverlayHost.Visibility = Visibility.Visible;
        // EmbeddedGlow — по state (Win7); TextOverlay не скрываем при refresh обоев.
    }

    /// <summary>Дождаться завершения фонового blur/wallpaper pipeline (для первого показа страницы).</summary>
    public Task WhenPipelineIdleAsync()
    {
        if (_pipelineBusyCount > 0 && _pipelineIdleTcs is not null)
            return _pipelineIdleTcs.Task;

        return Task.CompletedTask;
    }

    private void BeginPipelineWork()
    {
        if (_pipelineBusyCount++ == 0)
        {
            _pipelineIdleTcs = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        }
    }

    private void EndPipelineWork()
    {
        if (_pipelineBusyCount <= 0)
            return;

        if (--_pipelineBusyCount == 0)
        {
            _pipelineIdleTcs?.TrySetResult();
            _pipelineIdleTcs = null;
        }
    }

    public void ApplyState(PreviewState state, bool skipBlurRecompute = false)
    {
        var previousState = _hasState ? _lastState : (PreviewState?)null;
        _lastState = state;
        _hasState = true;
        _skipBlurRecompute = skipBlurRecompute;

        if (IsWallpaperBlurBase)
            ApplyPreviewKindChrome();

        var wallpaperPath = state.WallpaperPath;
        if (string.IsNullOrWhiteSpace(wallpaperPath))
            wallpaperPath = CurrentWallpaperResolver.TryGetDesktopWallpaperPath();

        var splitMode = IsWallpaperBlurBase
            ? false
            : !state.Win7Mode && state.ShowInactiveSplit;
        UpdateSplitModeVisibility(splitMode);

        if (!splitMode)
            UpdateSingleTintLayerLayout(state.Win7Mode);

        var wallpaperIdentity = ResolveWallpaperIdentity(wallpaperPath);
        var identityChanged = !string.Equals(_lastWallpaperIdentity, wallpaperIdentity, StringComparison.OrdinalIgnoreCase);
        if (identityChanged)
        {
            if (skipBlurRecompute)
            {
                ApplyWallpaperCrossfade(wallpaperPath, animate: _hasWallpaperImage);
                _lastWallpaperIdentity = wallpaperIdentity;
                _lastWallpaperPath = wallpaperPath;
            }
            else
            {
                var generation = ++_wallpaperRefreshGeneration;
                _ = RefreshWallpaperPipelineAsync(wallpaperPath, wallpaperIdentity, generation);
            }
        }

        var opaqueVisibility = state.OpaqueBlend ? Visibility.Visible : Visibility.Collapsed;
        OpaqueOverlay.Visibility = opaqueVisibility;
        SplitLeftOpaqueOverlay.Visibility = opaqueVisibility;
        SplitRightOpaqueOverlay.Visibility = opaqueVisibility;
        EmbeddedOpaqueOverlay.Visibility = IsWallpaperBlurBase ? opaqueVisibility : Visibility.Collapsed;

        SetBrushColor(_tintBrush, ToColor(state.TintArgb));
        SetBrushColor(_inactiveTintBrush, ToColor(state.InactiveTintArgb));
        TintFill.Fill = _tintBrush;
        EmbeddedTintFill.Fill = _tintBrush;
        BlurVisualHost.Opacity = 1;

        if (IsWallpaperBlurBase)
        {
            // Отдельные Glow/Tint-карточки скрыты; состав внутри BlurLayer.
            GlowLayer.Visibility = Visibility.Collapsed;
            TintLayer.Visibility = Visibility.Collapsed;
            EmbeddedTintFill.Visibility = Visibility.Visible;
            var showEmbeddedGlow = state.Win7Mode && state.ShowGlow;
            EmbeddedGlowFillA.Visibility = showEmbeddedGlow ? Visibility.Visible : Visibility.Collapsed;
            EmbeddedGlowFillB.Visibility = showEmbeddedGlow ? Visibility.Visible : Visibility.Collapsed;
            if (!showEmbeddedGlow)
            {
                _embeddedGlowBrush.ImageSource = null;
                _embeddedGlowBrushB.ImageSource = null;
            }

            if (!identityChanged && ShouldRunBlurPipeline(state, wallpaperPath, skipBlurRecompute))
                EnsureBlurPipeline(wallpaperPath);
            else if (!identityChanged && !skipBlurRecompute && _sharpSourceCache is not null)
            {
                if (state.Win7Mode && state.ShowGlow && Win7AfterglowParamsChanged(previousState, state))
                    ApplyBlurLayerSync(state, wallpaperPath);
                else
                    ApplyBlurLayerSync(_lastState, wallpaperPath);
            }

            return;
        }

        var blurRadius = Math.Clamp(state.BlurRadius, 0, MaxBlurRadius);
        ApplyLayerToolTips(state, blurRadius, splitMode);

        if (splitMode)
        {
            if (!identityChanged && ShouldRunBlurPipeline(state, wallpaperPath, skipBlurRecompute))
                EnsureBlurPipeline(wallpaperPath);
            else if (!identityChanged && !skipBlurRecompute && _sharpSourceCache is not null)
                ApplyBlurLayerSync(_lastState, wallpaperPath);
            return;
        }

        GlowLayer.Visibility = state.ShowGlow ? Visibility.Visible : Visibility.Collapsed;

        if (!identityChanged && !skipBlurRecompute)
        {
            if (ShouldRunBlurPipeline(state, wallpaperPath, skipBlurRecompute: false))
                EnsureBlurPipeline(wallpaperPath);
            else if (_sharpSourceCache is not null
                     && state.Win7Mode
                     && state.ShowGlow
                     && Win7AfterglowParamsChanged(previousState, state))
                ApplyBlurLayerSync(state, wallpaperPath);
        }
    }

    /// <summary>Gate inactive ON/OFF: только split-host и tint; обои/blur pipeline не трогаем.</summary>
    public void ApplyInactiveSplitLayout(PreviewState state)
    {
        if (IsWallpaperBlurBase)
            return;

        _lastState = state;
        _hasState = true;

        var splitMode = !state.Win7Mode && state.ShowInactiveSplit;
        UpdateSplitModeVisibility(splitMode, animate: false);

        if (!splitMode)
            UpdateSingleTintLayerLayout(state.Win7Mode);

        var opaqueVisibility = state.OpaqueBlend ? Visibility.Visible : Visibility.Collapsed;
        OpaqueOverlay.Visibility = opaqueVisibility;
        SplitLeftOpaqueOverlay.Visibility = opaqueVisibility;
        SplitRightOpaqueOverlay.Visibility = opaqueVisibility;

        var blurRadius = Math.Clamp(state.BlurRadius, 0, MaxBlurRadius);
        ApplyLayerToolTips(state, blurRadius, splitMode);

        SetBrushColor(_tintBrush, ToColor(state.TintArgb));
        SetBrushColor(_inactiveTintBrush, ToColor(state.InactiveTintArgb));

        SyncBlurCrossfadeToActiveHost(animate: false);

        if (splitMode)
            return;

        GlowLayer.Visibility = Visibility.Collapsed;
        BlurVisualHost.Opacity = 1;
        TintFill.Fill = _tintBrush;
    }

    /// <summary>Обновляет обои и blur/glow (триггер: WallpaperChanged, identity не только path).</summary>
    public void RefreshWallpaperLayer()
    {
        var identity = CurrentWallpaperResolver.TryGetWallpaperImageColorIdentity();
        if (string.Equals(_lastWallpaperIdentity, identity, StringComparison.OrdinalIgnoreCase))
            return;

        var wallpaperPath = CurrentWallpaperResolver.TryGetDesktopWallpaperPath();

        if (!_hasState || _skipBlurRecompute)
        {
            ApplyWallpaperCrossfade(wallpaperPath, animate: _hasWallpaperImage);
            _lastWallpaperIdentity = identity;
            _lastWallpaperPath = wallpaperPath;
            return;
        }

        var generation = ++_wallpaperRefreshGeneration;
        _ = RefreshWallpaperPipelineAsync(wallpaperPath, identity, generation);
    }

    private static string? ResolveWallpaperIdentity(string? wallpaperPath) =>
        CurrentWallpaperResolver.TryGetWallpaperImageColorIdentity() ?? wallpaperPath;

    private bool ShouldRunBlurPipeline(PreviewState state, string? wallpaperPath, bool skipBlurRecompute)
    {
        if (skipBlurRecompute)
            return false;

        var key = CompositionWallpaperPipelineBuilder.BuildKey(state, wallpaperPath);
        if (_lastBlurPipelineKey == key)
            return false;

        _lastBlurPipelineKey = key;
        return true;
    }

    private static BlurBuffer ToLocalBuffer(CompositionWallpaperPipelineBuilder.BlurBuffer buffer) =>
        new(buffer.Width, buffer.Height, buffer.Pixels);

    private static CompositionWallpaperPipelineBuilder.BlurBuffer ToBuilderBuffer(BlurBuffer buffer) =>
        new(buffer.Width, buffer.Height, buffer.Pixels);

    private void EnsureBlurPipeline(string? wallpaperPath)
    {
        var (width, height) = ResolvePipelinePaintSize();
        EnsureSharpSourceCache(wallpaperPath, width, height);
        if (_sharpSourceCache is not null)
            ApplyBlurLayerSync(_lastState, wallpaperPath);
    }

    private (int Width, int Height) ResolvePipelinePaintSize()
    {
        var blurLayer = _splitModeVisible ? SplitBlurLayer : BlurLayer;
        var width = Math.Max(
            1,
            (int)Math.Round(
                blurLayer.ActualWidth > 0
                    ? blurLayer.ActualWidth
                    : CompositionWallpaperPipelineBuilder.BlurLayerWidth));
        var height = Math.Max(
            1,
            (int)Math.Round(
                blurLayer.ActualHeight > 0
                    ? blurLayer.ActualHeight
                    : CompositionWallpaperPipelineBuilder.BlurLayerHeight));
        return (width, height);
    }

    private void UpdateSplitModeVisibility(bool splitMode, bool animate = true)
    {
        if (_splitModeVisible == splitMode)
            return;

        StopSplitModeTransition();

        _splitModeVisible = splitMode;
        if (splitMode)
            ApplySplitLayout();

        SyncBlurCrossfadeToActiveHost(animate: false);

        if (!animate || !ShouldAnimate())
        {
            SnapSplitModeHosts(splitMode);
            return;
        }

        var fadeOut = splitMode ? SingleCascadeHost : SplitCascadeHost;
        var fadeIn = splitMode ? SplitCascadeHost : SingleCascadeHost;
        fadeIn.Visibility = Visibility.Visible;
        fadeIn.Opacity = 0;

        var storyboard = new Storyboard();
        var ease = new CubicEase { EasingMode = EasingMode.EaseInOut };
        var outAnim = new DoubleAnimation
        {
            From = 1,
            To = 0,
            Duration = TimeSpan.FromMilliseconds(SplitModeFadeMs),
            EasingFunction = ease
        };
        var inAnim = new DoubleAnimation
        {
            From = 0,
            To = 1,
            Duration = TimeSpan.FromMilliseconds(SplitModeFadeMs),
            EasingFunction = ease
        };
        Storyboard.SetTarget(outAnim, fadeOut);
        Storyboard.SetTargetProperty(outAnim, nameof(UIElement.Opacity));
        Storyboard.SetTarget(inAnim, fadeIn);
        Storyboard.SetTargetProperty(inAnim, nameof(UIElement.Opacity));
        storyboard.Children.Add(outAnim);
        storyboard.Children.Add(inAnim);
        storyboard.Completed += (_, _) =>
        {
            if (!ReferenceEquals(_splitModeStoryboard, storyboard))
                return;

            fadeOut.Visibility = Visibility.Collapsed;
            fadeIn.Opacity = 1;
            _splitModeStoryboard = null;
        };
        _splitModeStoryboard = storyboard;
        storyboard.Begin();
    }

    private void StopSplitModeTransition()
    {
        if (_splitModeStoryboard is null)
            return;

        _splitModeStoryboard.Stop();
        _splitModeStoryboard = null;
        SnapSplitModeHosts(_splitModeVisible);
    }

    private void SnapSplitModeHosts(bool splitMode)
    {
        SingleCascadeHost.Visibility = splitMode ? Visibility.Collapsed : Visibility.Visible;
        SingleCascadeHost.Opacity = splitMode ? 0 : 1;
        SplitCascadeHost.Visibility = splitMode ? Visibility.Visible : Visibility.Collapsed;
        SplitCascadeHost.Opacity = splitMode ? 1 : 0;
    }

    private static bool ShouldAnimate()
    {
        try
        {
            return new UISettings().AnimationsEnabled;
        }
        catch
        {
            return true;
        }
    }

    private void ApplyLayout()
    {
        if (_layoutApplied)
            return;

        _layoutApplied = true;
        CascadeRoot.Width = BaseWidth;
        CascadeRoot.Height = BaseHeight;

        // Каскад Full; WallpaperBlurBase переопределяет размеры в ApplyPreviewKindChrome
        // (PreviewKind с родителя часто приходит после ctor).
        SizeLayer(WallpaperLayer, 0);
        SizeLayer(BlurLayer, 1);
        SizeLayer(GlowLayer, 2);
        SizeLayer(TintLayer, 3);

        WallpaperLayer.Translation = new Vector3(0f, 0f, LayerDepthStep * 1);
        BlurLayer.Translation = new Vector3(0f, 0f, LayerDepthStep * 2);
        GlowLayer.Translation = new Vector3(0f, 0f, LayerDepthStep * 3);
        TintLayer.Translation = new Vector3(0f, 0f, LayerDepthStep * 4);
    }

    private void ApplySplitLayout()
    {
        if (_splitLayoutApplied)
            return;

        _splitLayoutApplied = true;

        SizeLayer(SplitSharedWallpaperLayer, 0);
        SplitSharedWallpaperLayer.Translation = new Vector3(0f, 0f, LayerDepthStep * 1);

        SizeLayer(SplitBlurLayer, SplitBlurLayerIndex);

        var blurHeight = BaseHeight - (Inset * SplitBlurLayerIndex);
        var blurWidth = BaseWidth - (Inset * SplitBlurLayerIndex);
        var tintWidth = BaseWidth - (Inset * SplitTintLayerIndex);
        var splitTintHeight = blurHeight - SplitTintHeightBelowBlurPx;

        SplitBlurLayer.Translation = new Vector3(0f, 0f, LayerDepthStep * 2);
        SplitRightTintLayer.Translation = new Vector3(0f, 0f, LayerDepthStep * 3);
        SplitLeftTintLayer.Translation = new Vector3(0f, 0f, LayerDepthStep * 4);

        var splitTintHalfWidth = GetSplitTintHalfWidth(tintWidth);
        var splitInactiveTintOffsetX = blurWidth - SplitInactiveTintRightInsetFromBlurPx - splitTintHalfWidth;

        const double cornerRadius = 6;
        var fullCorners = new CornerRadius(cornerRadius);

        SplitLeftTintLayer.Width = splitTintHalfWidth;
        SplitLeftTintLayer.Height = splitTintHeight;
        SplitLeftTintLayer.CornerRadius = fullCorners;
        SplitLeftTintLayer.Clip = null;
        SplitLeftTintLayer.RenderTransform = null;

        SplitRightTintLayer.Width = splitTintHalfWidth;
        SplitRightTintLayer.Height = splitTintHeight;
        SplitRightTintLayer.CornerRadius = fullCorners;
        SplitRightTintLayer.Clip = null;
        SplitRightTintLayer.RenderTransform = new TranslateTransform
        {
            X = splitInactiveTintOffsetX,
            Y = SplitInactiveTintBelowBlurTopPx,
        };
    }

    /// <summary>Ширина каждого split-tint: половина слоя + overlap для пересечения по центру.</summary>
    private static double GetSplitTintHalfWidth(double tintWidth) => (tintWidth / 2) + SplitTintHalfExtraPx;

    private static void SizeLayer(FrameworkElement element, int index)
    {
        element.Width = BaseWidth - (Inset * index);
        element.Height = BaseHeight - (Inset * index);
    }

    /// <summary>Vista без split: tint = blur − Inset (24 px) справа/снизу; Win7 — каскад index 3 (после glow).</summary>
    private void UpdateSingleTintLayerLayout(bool win7Mode)
    {
        if (IsWallpaperBlurBase)
        {
            // Tint встроен в карточку стекла; отдельный TintLayer скрыт.
            SizeLayer(BlurLayer, 1);
            return;
        }

        if (win7Mode)
        {
            SizeLayer(TintLayer, 3);
            return;
        }

        var blurWidth = BaseWidth - Inset;
        var blurHeight = BaseHeight - Inset;
        TintLayer.Width = blurWidth - Inset;
        TintLayer.Height = blurHeight - Inset;
    }

    private void ApplyLocalizedLabels()
    {
        var wallpaper = GlassSettingsResources.Get("Stylization_Layer_Wallpaper");
        var blur = GlassSettingsResources.Get("Dwm_CompositionPreviewLayerBlur");
        var tint = GlassSettingsResources.Get("Dwm_CompositionPreviewLayerTint");
        var glow = GlassSettingsResources.Get("Dwm_CompositionPreviewLayerGlow");
        var tintActive = GlassSettingsResources.Get("Dwm_CompositionPreviewLayerTintActive");
        var tintInactive = GlassSettingsResources.Get("Dwm_CompositionPreviewLayerTintInactive");

        WallpaperLabel.Text = wallpaper;
        BlurLabel.Text = blur;
        TintLabel.Text = tint;
        GlowLabel.Text = glow;

        SplitSharedWallpaperLabel.Text = wallpaper;
        SplitBlurLabel.Text = blur;
        SplitLeftTintLabel.Text = tintActive;
        SplitRightTintLabel.Text = tintInactive;
    }

    private void ApplyLayerToolTips(PreviewState state, int blurRadius, bool splitMode)
    {
        if (splitMode)
        {
            ApplyVistaToolTips(state, blurRadius, BlurLayer: SplitBlurLayer, TintLayer: SplitLeftTintLayer, blurLabel: SplitBlurLabel, tintLabel: SplitLeftTintLabel);
            ApplyVistaToolTips(state, blurRadius, BlurLayer: SplitBlurLayer, TintLayer: SplitRightTintLayer, blurLabel: SplitBlurLabel, tintLabel: SplitRightTintLabel, inactiveTint: true);
            return;
        }

        if (state.Win7Mode)
        {
            ToolTipService.SetToolTip(
                BlurLayer,
                GlassSettingsResources.Format(
                    "Dwm_CompositionLayerTooltipFormat",
                    GlassSettingsResources.Get("Dwm_BlurLevelTitle"),
                    GlassSettingsResources.Format(
                        "Dwm_ReflectionPercentFormat",
                        Math.Min(state.BlurBalancePercent, 100u))));

            ToolTipService.SetToolTip(
                TintLayer,
                GlassSettingsResources.Format(
                    "Dwm_CompositionLayerTooltipFormat",
                    TintLabel.Text,
                    GlassSettingsResources.Format(
                        "Dwm_ReflectionPercentFormat",
                        AlphaToPercent((byte)((state.TintArgb >> 24) & 0xFF)))));

            if (state.ShowGlow)
            {
                ToolTipService.SetToolTip(
                    GlowLayer,
                    GlassSettingsResources.Format(
                        "Dwm_CompositionLayerTooltipFormat",
                        GlowLabel.Text,
                        GlassSettingsResources.Format(
                            "Dwm_ReflectionPercentFormat",
                            Math.Min(state.Win7AfterglowBalancePercent, 100u))));
            }
            else
            {
                ToolTipService.SetToolTip(GlowLayer, null);
            }

            return;
        }

        ApplyVistaToolTips(state, blurRadius, BlurLayer, TintLayer, BlurLabel, TintLabel);
    }

    private static void ApplyVistaToolTips(
        PreviewState state,
        int blurRadius,
        FrameworkElement BlurLayer,
        FrameworkElement TintLayer,
        TextBlock blurLabel,
        TextBlock tintLabel,
        bool inactiveTint = false)
    {
        ToolTipService.SetToolTip(
            BlurLayer,
            GlassSettingsResources.Format("Dwm_CompositionLayerTooltipFormat", blurLabel.Text, blurRadius));

        var tintArgb = inactiveTint ? state.InactiveTintArgb : state.TintArgb;
        var tintPercent = AlphaToPercent((byte)((tintArgb >> 24) & 0xFF));
        ToolTipService.SetToolTip(
            TintLayer,
            GlassSettingsResources.Format(
                "Dwm_CompositionLayerTooltipFormat",
                tintLabel.Text,
                GlassSettingsResources.Format("Dwm_ReflectionPercentFormat", tintPercent)));
    }

    private static uint AlphaToPercent(byte alpha) => (uint)((alpha * 100u + 127u) / 255u);

    private static void SetBrushColor(SolidColorBrush brush, UiColor color)
    {
        if (brush.Color != color)
            brush.Color = color;
    }

    private void ApplyWallpaperCrossfade(string? wallpaperPath, bool animate)
    {
        var source = DesktopWallpaperImageCache.GetDisplaySourceOrLoad(wallpaperPath);
        ApplyWallpaperCrossfade(source, animate);
    }

    private void ApplyWallpaperCrossfade(ImageSource? source, bool animate)
    {
        _wallpaperCrossfade.Apply(source, animate);
        _splitWallpaperCrossfade.Apply(source, animate);
        _hasWallpaperImage = source is not null;
    }

    private async Task RefreshWallpaperPipelineAsync(string? wallpaperPath, string? wallpaperIdentity, int generation)
    {
        BeginPipelineWork();
        try
        {
            var (width, height) = ResolvePipelinePaintSize();

            var state = _lastState;
            var blurRadius = Math.Clamp(state.BlurRadius, 0, MaxBlurRadius);
            var buildKey = CompositionWallpaperPipelineBuilder.BuildKey(state, wallpaperPath);

            if (TryApplyFromLayersCache(buildKey, width, height, state, wallpaperIdentity, wallpaperPath, generation))
                return;

            CompositionWallpaperPipelineBuilder.PipelineSnapshot payload;
            try
            {
                payload = await Task.Run(() =>
                    CompositionWallpaperPipelineBuilder.Build(wallpaperPath, width, height, state)).ConfigureAwait(true);
            }
            catch
            {
                return;
            }

            if (generation != _wallpaperRefreshGeneration)
                return;

            ApplyPipelineSnapshot(payload, buildKey, wallpaperIdentity, wallpaperPath, state, blurRadius);
        }
        finally
        {
            EndPipelineWork();
        }
    }

    private bool TryApplyFromLayersCache(
        CompositionWallpaperPipelineBuilder.BlurPipelineKey buildKey,
        int width,
        int height,
        PreviewState state,
        string? wallpaperIdentity,
        string? wallpaperPath,
        int? expectedGeneration = null)
    {
        if (!CompositionWallpaperLayersCache.TryGetPipeline(buildKey, width, height, out var snap))
            return false;

        if (expectedGeneration.HasValue && expectedGeneration.Value != _wallpaperRefreshGeneration)
            return false;

        var blurRadius = Math.Clamp(state.BlurRadius, 0, MaxBlurRadius);
        ApplyPipelineSnapshot(snap, buildKey, wallpaperIdentity, wallpaperPath, state, blurRadius);
        return true;
    }

    private void ApplyPipelineSnapshot(
        CompositionWallpaperPipelineBuilder.PipelineSnapshot payload,
        CompositionWallpaperPipelineBuilder.BlurPipelineKey buildKey,
        string? wallpaperIdentity,
        string? wallpaperPath,
        PreviewState state,
        int blurRadius)
    {
        _sharpSourceKey = new SharpSourceKey(wallpaperIdentity, payload.Sharp.Width, payload.Sharp.Height);
        _sharpSourceCache = ToLocalBuffer(payload.Sharp);
        _gaussianCacheKey = new GaussianCacheKey(
            wallpaperIdentity,
            blurRadius,
            payload.Gaussian.Width,
            payload.Gaussian.Height);
        _gaussianCache = ToLocalBuffer(payload.Gaussian);
        _lastBlurPipelineKey = buildKey;
        _lastWallpaperIdentity = wallpaperIdentity;
        _lastWallpaperPath = wallpaperPath;

        var animateBlur = _blurBrush.ImageSource is not null || _blurBrushB.ImageSource is not null;
        if (payload.BlurBgra is not null)
        {
            var blurBitmap = CreateWriteableBitmap(
                new BlurBuffer(payload.Gaussian.Width, payload.Gaussian.Height, payload.BlurBgra));
            CrossfadeBlurLayers(blurBitmap, animateBlur);
        }

        if (payload.GlowBgra is not null && state.Win7Mode)
        {
            var animateGlow = _glowBrush.ImageSource is not null || _glowBrushB.ImageSource is not null;
            var glowBitmap = CreateWriteableBitmap(
                new BlurBuffer(payload.Gaussian.Width, payload.Gaussian.Height, payload.GlowBgra));
            CrossfadeGlowLayers(glowBitmap, animateGlow);
        }

        var displayBitmap = CreateWriteableBitmap(ToLocalBuffer(payload.DisplayWallpaper));
        ApplyWallpaperCrossfade(displayBitmap, animate: _hasWallpaperImage);
    }

    private ImageBrush GetFrontBlurBrush() => _blurLayerAIsFront ? _blurBrush : _blurBrushB;

    private ImageBrush GetFrontGlowBrush() =>
        IsWallpaperBlurBase
            ? (_glowLayerAIsFront ? _embeddedGlowBrush : _embeddedGlowBrushB)
            : (_glowLayerAIsFront ? _glowBrush : _glowBrushB);

    private void CrossfadeBlurLayers(ImageSource bitmap, bool animate)
    {
        var incomingIsA = !_blurLayerAIsFront;
        var incomingBrush = incomingIsA ? _blurBrush : _blurBrushB;
        incomingBrush.ImageSource = bitmap;

        if (_splitModeVisible)
            _splitBlurCrossfade.ShowFront(incomingIsA, animate);
        else
            _blurCrossfade.ShowFront(incomingIsA, animate);

        _blurLayerAIsFront = incomingIsA;
    }

    /// <summary>Single/split делят ImageBrush, но разные пары Rectangle — синхрон front при смене host.</summary>
    private void SyncBlurCrossfadeToActiveHost(bool animate)
    {
        if (_splitModeVisible)
            _splitBlurCrossfade.ShowFront(_blurLayerAIsFront, animate);
        else
            _blurCrossfade.ShowFront(_blurLayerAIsFront, animate);
    }

    private void CrossfadeGlowLayers(ImageSource bitmap, bool animate)
    {
        var incomingIsA = !_glowLayerAIsFront;
        var incomingBrush = IsWallpaperBlurBase
            ? (incomingIsA ? _embeddedGlowBrush : _embeddedGlowBrushB)
            : (incomingIsA ? _glowBrush : _glowBrushB);
        incomingBrush.ImageSource = bitmap;
        if (IsWallpaperBlurBase)
            _embeddedGlowCrossfade.ShowFront(incomingIsA, animate);
        else
            _glowCrossfade.ShowFront(incomingIsA, animate);
        _glowLayerAIsFront = incomingIsA;
    }

    private void EnsureSharpSourceCache(string? wallpaperPath, int width, int height)
    {
        var identity = ResolveWallpaperIdentity(wallpaperPath);
        var key = new SharpSourceKey(identity, width, height);
        if (_sharpSourceKey == key && _sharpSourceCache is not null)
            return;

        _sharpSourceKey = key;
        _sharpSourceCache = null;
        _gaussianCacheKey = null;
        _gaussianCache = null;

        var generation = ++_sharpSourceLoadGeneration;
        _ = LoadSharpSourceCacheAsync(wallpaperPath, width, height, generation);
    }

    private async Task LoadSharpSourceCacheAsync(
        string? wallpaperPath,
        int width,
        int height,
        int generation)
    {
        BeginPipelineWork();
        try
        {
            var payload = await Task.Run(() =>
                ToLocalBuffer(CompositionWallpaperPipelineBuilder.BuildSharpBuffer(wallpaperPath, width, height)))
                .ConfigureAwait(true);

            if (generation != _sharpSourceLoadGeneration)
                return;

            _sharpSourceCache = payload;

            if (_hasState && !_skipBlurRecompute)
                ApplyBlurLayerSync(_lastState, wallpaperPath);
        }
        finally
        {
            EndPipelineWork();
        }
    }

    private void ApplyBlurLayerSync(PreviewState state, string? wallpaperPath)
    {
        if (_sharpSourceCache is null)
            return;

        var sharp = _sharpSourceCache.Value;
        var blurRadius = Math.Clamp(state.BlurRadius, 0, MaxBlurRadius);
        var buildKey = CompositionWallpaperPipelineBuilder.BuildKey(state, wallpaperPath);
        if (TryApplyFromLayersCache(
                buildKey,
                sharp.Width,
                sharp.Height,
                state,
                ResolveWallpaperIdentity(wallpaperPath),
                wallpaperPath))
        {
            return;
        }

        var gaussianKey = new GaussianCacheKey(
            ResolveWallpaperIdentity(wallpaperPath),
            blurRadius,
            sharp.Width,
            sharp.Height);

        if (_gaussianCacheKey != gaussianKey || _gaussianCache is null)
        {
            _gaussianCache = ToLocalBuffer(
                CompositionWallpaperPipelineBuilder.BuildGaussianFromSharp(
                    ToBuilderBuffer(sharp),
                    blurRadius));
            _gaussianCacheKey = gaussianKey;
        }

        var gaussian = _gaussianCache!.Value;
        ImageSource bitmap;

        if (state.Win7Mode)
        {
            var balance = Math.Min(state.BlurBalancePercent, 100u);
            var blurBalance = balance / 100f;
            var blurBgra = Win7AeroPreviewCompositor.BuildBlurStage(
                gaussian.Pixels,
                gaussian.Width,
                gaussian.Height,
                blurBalance,
                afterglowBalance: 0f);

            bitmap = CreateWriteableBitmap(new BlurBuffer(gaussian.Width, gaussian.Height, blurBgra));

            var (afterglowR, afterglowG, afterglowB) = ResolveWin7AfterglowRgb(state);
            var glowBgra = Win7AeroPreviewCompositor.BuildAfterglowStage(
                gaussian.Pixels,
                gaussian.Width,
                gaussian.Height,
                afterglowR,
                afterglowG,
                afterglowB);
            GetFrontGlowBrush().ImageSource = CreateWriteableBitmap(new BlurBuffer(gaussian.Width, gaussian.Height, glowBgra));
        }
        else
        {
            bitmap = CreateWriteableBitmap(gaussian);
        }

        GetFrontBlurBrush().ImageSource = bitmap;
    }

    private static bool Win7AfterglowParamsChanged(PreviewState? previous, PreviewState current) =>
        previous is null
        || !previous.Value.Win7Mode
        || previous.Value.Win7AfterglowBgr != current.Win7AfterglowBgr
        || previous.Value.Win7AfterglowBalancePercent != current.Win7AfterglowBalancePercent;

    private static (float R, float G, float B) ResolveWin7AfterglowRgb(PreviewState state)
    {
        var balance = Math.Min(state.Win7AfterglowBalancePercent, 100u) / 100f;
        var bgr = state.Win7AfterglowBgr & 0x00FFFFFFu;
        var b = (bgr & 0xFF) / 255f * balance;
        var g = ((bgr >> 8) & 0xFF) / 255f * balance;
        var r = ((bgr >> 16) & 0xFF) / 255f * balance;
        return (r, g, b);
    }

    private static WriteableBitmap CreateWriteableBitmap(BlurBuffer buffer)
    {
        var bitmap = new WriteableBitmap(buffer.Width, buffer.Height);
        using var stream = bitmap.PixelBuffer.AsStream();
        stream.Seek(0, SeekOrigin.Begin);
        stream.Write(buffer.Pixels, 0, buffer.Pixels.Length);
        return bitmap;
    }

    private static UiColor ToColor(uint argb) =>
        UiColor.FromArgb(
            (byte)((argb >> 24) & 0xFF),
            (byte)((argb >> 16) & 0xFF),
            (byte)((argb >> 8) & 0xFF),
            (byte)(argb & 0xFF));
}
