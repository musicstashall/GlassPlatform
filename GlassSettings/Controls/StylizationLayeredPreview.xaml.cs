using System.IO;
using System.Runtime.InteropServices.WindowsRuntime;
using System.Threading.Tasks;
using GlassSettings.Core.Dwm;
using GlassSettings.Core.Registry;
using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using UiColor = Windows.UI.Color;

namespace GlassSettings.Controls;

/// <summary>
/// Каскадное превью страницы «Стилизация»: 5 слоёв (обои → стекло → отражение →
/// материал → окно). Стекло — blur+glow+tint из shared pipeline «Композиции».
/// </summary>
public sealed partial class StylizationLayeredPreview : UserControl
{
    private const string ReflectionRegistryKey = "CustomThemeReflection";
    private const string MaterialRegistryKey = "CustomThemeMaterial";
    private const string AtlasRegistryKey = "CustomThemeAtlas";
    private const string LegacyAtlasRegistryKey = "AtlasThemeImage";
    private const string ReflectionIntensityKey = "ColorizationGlassReflectionIntensity";
    private const string MaterialOpacityKey = "MaterialOpacity";

    private const double BaseWidth = 340;
    private const double BaseHeight = 192;
    private const double Inset = 30;
    private const int AtlasLayerIndex = 4;
    private const int WallpaperCrossfadeMs = WallpaperPreviewCrossfade.DefaultCrossfadeDurationMs;
    private const double ReflectionBaseOpacity = 0.5;
    private const double MaterialBaseOpacity = 0.35;

    private readonly ImageBrush _glassBlurBrush = new() { Stretch = Stretch.UniformToFill };
    private readonly ImageBrush _glassBlurBrushB = new() { Stretch = Stretch.UniformToFill };
    private readonly ImageBrush _glassGlowBrush = new() { Stretch = Stretch.UniformToFill };
    private readonly ImageBrush _glassGlowBrushB = new() { Stretch = Stretch.UniformToFill };
    private readonly SolidColorBrush _glassTintBrush = new(UiColor.FromArgb(0, 0, 0, 0));
    private readonly WallpaperPreviewCrossfade _wallpaperCrossfade;
    private readonly UiElementCrossfade _glassBlurCrossfade;
    private readonly UiElementCrossfade _glassGlowCrossfade;

    private static double AtlasLayerWidth => BaseWidth - (Inset * AtlasLayerIndex);
    private static double AtlasLayerHeight => BaseHeight - (Inset * AtlasLayerIndex);
    private static int GlassPaintWidth => CompositionWallpaperPipelineBuilder.BlurLayerWidth;
    private static int GlassPaintHeight => CompositionWallpaperPipelineBuilder.BlurLayerHeight;

    private int _refreshGeneration;
    private int _wallpaperPipelineGeneration;
    private string? _lastWallpaperIdentity;
    private string? _lastThemeFilePath;
    private CompositionWallpaperPipelineBuilder.BlurPipelineKey? _lastGlassPipelineKey;
    private bool _hasWallpaperImage;
    private bool _glassBlurLayerAIsFront = true;
    private bool _glassGlowLayerAIsFront = true;

    public StylizationLayeredPreview()
    {
        InitializeComponent();
        _wallpaperCrossfade = new WallpaperPreviewCrossfade(
            WallpaperImageA,
            WallpaperImageB,
            WallpaperFallback,
            WallpaperCrossfadeMs);
        _glassBlurCrossfade = new UiElementCrossfade(GlassBlurFillA, GlassBlurFillB, WallpaperCrossfadeMs);
        _glassGlowCrossfade = new UiElementCrossfade(GlassGlowFillA, GlassGlowFillB, WallpaperCrossfadeMs);
        GlassBlurFillA.Fill = _glassBlurBrush;
        GlassBlurFillB.Fill = _glassBlurBrushB;
        GlassGlowFillA.Fill = _glassGlowBrush;
        GlassGlowFillB.Fill = _glassGlowBrushB;
        GlassTintFill.Fill = _glassTintBrush;
        ApplyLayout();
        ApplyStaticVisuals();
        Loaded += (_, _) => RefreshLayers();
    }

    private void ApplyLayout()
    {
        CascadeRoot.Width = BaseWidth;
        CascadeRoot.Height = BaseHeight;
        SizeLayer(WallpaperLayer, 0);
        SizeLayer(GlassLayer, 1);
        SizeLayer(ReflectionLayer, 2);
        SizeLayer(MaterialLayer, 3);
        SizeLayer(WindowLayer, AtlasLayerIndex);

        // Текстуры: ImageBrush/Image Stretch=None, clip = габарит слоя каскада (верхний левый угол, 1:1).
        SizeTextureClipHost(ReflectionClip, 2);
        SizeTextureClipHost(MaterialClip, 3);
        ApplyClip(WindowClip, AtlasLayerIndex);
    }

    private static void SizeTextureClipHost(Grid host, int index)
    {
        var w = BaseWidth - (Inset * index);
        var h = BaseHeight - (Inset * index);
        host.Width = w;
        host.Height = h;
        ApplyClip(host, index);
    }

    private static void SizeLayer(FrameworkElement layer, int index)
    {
        layer.Width = BaseWidth - (Inset * index);
        layer.Height = BaseHeight - (Inset * index);
    }

    private static void ApplyClip(FrameworkElement host, int index)
    {
        host.Clip = new Microsoft.UI.Xaml.Media.RectangleGeometry
        {
            Rect = new Windows.Foundation.Rect(0, 0, BaseWidth - (Inset * index), BaseHeight - (Inset * index))
        };
    }

    private void ApplyStaticVisuals()
    {
        WallpaperLabel.Text = GlassSettingsResources.Get("Stylization_Layer_Wallpaper");
        GlassLabel.Text = GlassSettingsResources.Get("Stylization_Layer_Glass");
        ReflectionLabel.Text = GlassSettingsResources.Get("Stylization_Layer_Reflection");
        MaterialLabel.Text = GlassSettingsResources.Get("Stylization_Layer_Material");
        WindowLabel.Text = GlassSettingsResources.Get("Stylization_Layer_Atlas");
        GlassSettingsToolTipService.SetToolTip(GlassLayer, GlassSettingsResources.Get("Stylization_Layer_Glass"));
    }

    /// <summary>Пересобирает источники слоёв из текущего реестра DWM и обоев рабочего стола.</summary>
    public async void RefreshLayers()
    {
        _lastThemeFilePath = TryGetCurrentThemeRegistryPathNormalized();

        var generation = ++_refreshGeneration;

        var wallpaperPath = CurrentWallpaperResolver.TryGetDesktopWallpaperPath();
        var wallpaperIdentity = CurrentWallpaperResolver.TryGetWallpaperImageColorIdentity();
        var wallpaperIdentityChanged = !string.Equals(
            _lastWallpaperIdentity,
            wallpaperIdentity,
            StringComparison.OrdinalIgnoreCase);

        if (wallpaperIdentityChanged || !_hasWallpaperImage)
        {
            SetLayerTooltip(WallpaperLayer, "Stylization_Layer_Wallpaper", wallpaperPath);
            _lastGlassPipelineKey = null;
            var pipelineGeneration = ++_wallpaperPipelineGeneration;
            _ = RefreshWallpaperPipelineAsync(
                wallpaperPath,
                wallpaperIdentity,
                pipelineGeneration,
                applyWallpaper: true);
        }
        else
        {
            RefreshGlassComposition();
        }

        var reflectionGateOn = IsReflectionCollectionGateOn();
        string? reflectionPath = null;
        ImageSource? reflection = null;
        if (reflectionGateOn)
        {
            reflectionPath = NormalizeCustom(DwmRegistryStore.ReadEffectiveString(ReflectionRegistryKey));
            reflection = reflectionPath is not null
                ? ThemeImageCatalog.CreateImageSource(reflectionPath)
                : ThemeImageCatalog.TryGetDefaultReflectionImage();
        }

        SetReflectionTextureLayer(reflection);
        SetLayerTooltip(ReflectionLayer, "Stylization_Layer_Reflection", reflectionPath);
        ApplyReflectionLayerEffects(reflectionGateOn, reflection is not null);

        var materialGateOn = IsMaterialCollectionGateOn();
        string? materialPath = null;
        ImageSource? material = null;
        if (materialGateOn)
        {
            materialPath = NormalizeCustom(DwmRegistryStore.ReadEffectiveString(MaterialRegistryKey));
            material = materialPath is not null
                ? ThemeImageCatalog.CreateImageSource(materialPath)
                : await ThemeImageCatalog.TryGetDefaultMaterialImageAsync();
        }

        SetTextureLayer(MaterialImage, MaterialFallback, material);
        SetLayerTooltip(MaterialLayer, "Stylization_Layer_Material", materialPath);
        ApplyMaterialLayerEffects(materialGateOn, material is not null);

        var atlasPath = NormalizeCustom(DwmRegistryStore.ReadEffectiveString(AtlasRegistryKey))
            ?? NormalizeCustom(DwmRegistryStore.ReadEffectiveString(LegacyAtlasRegistryKey));
        SetLayerTooltip(WindowLayer, "Stylization_Layer_Atlas", atlasPath);
        _ = GenerateWindowLayerAsync(generation, atlasPath, preferLiveThemeApi: string.IsNullOrWhiteSpace(atlasPath));
    }

    /// <summary>Обновляет только прозрачность/видимость слоёв отражения и материала (слайдеры gate).</summary>
    public void RefreshLayerEffects()
    {
        RefreshReflectionMaterialSourcesIfNeeded();
        ApplyReflectionLayerEffects(
            IsReflectionCollectionGateOn(),
            ReflectionTextureBrush.ImageSource is not null);
        ApplyMaterialLayerEffects(
            IsMaterialCollectionGateOn(),
            MaterialImage.Source is not null);
    }

    /// <summary>После gate OFF→ON без полного RefreshLayers источник мог остаться null, fallback — видим.</summary>
    private async void RefreshReflectionMaterialSourcesIfNeeded()
    {
        if (IsReflectionCollectionGateOn() && ReflectionTextureBrush.ImageSource is null)
        {
            var reflectionPath = NormalizeCustom(DwmRegistryStore.ReadEffectiveString(ReflectionRegistryKey));
            var reflection = reflectionPath is not null
                ? ThemeImageCatalog.CreateImageSource(reflectionPath)
                : ThemeImageCatalog.TryGetDefaultReflectionImage();
            SetReflectionTextureLayer(reflection);
            SetLayerTooltip(ReflectionLayer, "Stylization_Layer_Reflection", reflectionPath);
        }

        if (IsMaterialCollectionGateOn() && MaterialImage.Source is null)
        {
            var materialPath = NormalizeCustom(DwmRegistryStore.ReadEffectiveString(MaterialRegistryKey));
            var material = materialPath is not null
                ? ThemeImageCatalog.CreateImageSource(materialPath)
                : await ThemeImageCatalog.TryGetDefaultMaterialImageAsync();
            SetTextureLayer(MaterialImage, MaterialFallback, material);
            SetLayerTooltip(MaterialLayer, "Stylization_Layer_Material", materialPath);
        }
    }

    private static bool IsReflectionCollectionGateOn() =>
        DwmRegistryStore.ReadEffectiveDword(ReflectionIntensityKey).HasValue
        || !string.IsNullOrWhiteSpace(DwmRegistryStore.ReadEffectiveString(ReflectionRegistryKey));

    private static bool IsMaterialCollectionGateOn() =>
        DwmRegistryStore.ReadEffectiveDword(MaterialOpacityKey).HasValue
        || !string.IsNullOrWhiteSpace(DwmRegistryStore.ReadEffectiveString(MaterialRegistryKey));

    private static uint ReadEffectPercent(string intensityKey) =>
        Math.Clamp(
            DwmRegistryStore.ReadEffectiveDword(intensityKey) ?? DwmDefaults.DefaultWhenKeyAbsent(intensityKey),
            0u,
            100u);

    private void ApplyReflectionLayerEffects(bool gateOn, bool hasTexture)
    {
        ReflectionLayer.Visibility = gateOn ? Visibility.Visible : Visibility.Collapsed;
        if (!gateOn)
        {
            ReflectionTextureFill.Opacity = 0;
            return;
        }

        if (!hasTexture)
        {
            ReflectionTextureFill.Opacity = 0;
            ReflectionFallback.Visibility = Visibility.Collapsed;
            return;
        }

        var percent = ReadEffectPercent(ReflectionIntensityKey);
        ReflectionTextureFill.Opacity = ReflectionBaseOpacity * (percent / 100.0);
    }

    private void ApplyMaterialLayerEffects(bool gateOn, bool hasTexture)
    {
        MaterialLayer.Visibility = gateOn ? Visibility.Visible : Visibility.Collapsed;
        if (!gateOn)
        {
            MaterialImage.Opacity = 0;
            return;
        }

        if (!hasTexture)
        {
            MaterialImage.Opacity = 0;
            MaterialFallback.Visibility = Visibility.Collapsed;
            return;
        }

        var percent = ReadEffectPercent(MaterialOpacityKey);
        MaterialImage.Opacity = MaterialBaseOpacity * (percent / 100.0);
    }

    /// <summary>Blur+glow+tint из превью «Композиция» (без смены обоев — только параметры стекла).</summary>
    public void RefreshGlassComposition()
    {
        var state = ResolveCompositionPreviewState();

        ApplyGlassTintAndChrome(state);
        ApplyGlassLayerTooltip(state);

        var wallpaperPath = CurrentWallpaperResolver.TryGetDesktopWallpaperPath();
        var buildKey = CompositionWallpaperPipelineBuilder.BuildKey(state, wallpaperPath);
        if (_lastGlassPipelineKey == buildKey && _glassBlurBrush.ImageSource is not null)
            return;

        var generation = ++_wallpaperPipelineGeneration;
        _ = RefreshWallpaperPipelineAsync(
            wallpaperPath,
            _lastWallpaperIdentity,
            generation,
            applyWallpaper: false);
    }

    private DwmCompositionCascadePreview.PreviewState ResolveCompositionPreviewState()
    {
        var (vista, win7) = CompositionPreviewRegistryStates.BuildFromRuntimeEffective();
        if (IsWin7GlassMode())
            return win7;

        return vista with { ShowInactiveSplit = false };
    }

    private static bool IsWin7GlassMode() =>
        (DwmRegistryStore.ReadEffectiveDword("GlassType") ?? 0) == 1;

    private void ApplyGlassTintAndChrome(DwmCompositionCascadePreview.PreviewState state)
    {
        SetBrushColor(_glassTintBrush, ToColor(state.TintArgb));
        GlassOpaqueOverlay.Visibility = state.OpaqueBlend ? Visibility.Visible : Visibility.Collapsed;
        var showGlow = state.Win7Mode && state.ShowGlow;
        GlassGlowFillA.Visibility = showGlow ? Visibility.Visible : Visibility.Collapsed;
        GlassGlowFillB.Visibility = showGlow ? Visibility.Visible : Visibility.Collapsed;
        if (!showGlow)
        {
            _glassGlowBrush.ImageSource = null;
            _glassGlowBrushB.ImageSource = null;
        }
    }

    private void ApplyGlassLayerTooltip(DwmCompositionCascadePreview.PreviewState state)
    {
        var tintPercent = AlphaToPercent((byte)((state.TintArgb >> 24) & 0xFF));
        GlassSettingsToolTipService.SetToolTip(
            GlassLayer,
            GlassSettingsResources.Format(
                "Dwm_CompositionLayerTooltipFormat",
                GlassSettingsResources.Get("Stylization_Layer_Glass"),
                GlassSettingsResources.Format("Dwm_ReflectionPercentFormat", tintPercent)));
    }

    private async Task RefreshWallpaperPipelineAsync(
        string? wallpaperPath,
        string? wallpaperIdentity,
        int generation,
        bool applyWallpaper)
    {
        var state = ResolveCompositionPreviewState();
        ApplyGlassTintAndChrome(state);
        ApplyGlassLayerTooltip(state);

        var buildKey = CompositionWallpaperPipelineBuilder.BuildKey(state, wallpaperPath);

        try
        {
            if (CompositionWallpaperLayersCache.TryGetPipeline(buildKey, GlassPaintWidth, GlassPaintHeight, out var cached))
            {
                if (generation == _wallpaperPipelineGeneration)
                {
                    ApplyCombinedPipelineSnapshot(
                        cached,
                        state,
                        buildKey,
                        wallpaperIdentity,
                        applyWallpaper);
                }

                return;
            }

            var payload = await Task.Run(() =>
                CompositionWallpaperPipelineBuilder.Build(
                    wallpaperPath,
                    GlassPaintWidth,
                    GlassPaintHeight,
                    state)).ConfigureAwait(true);

            if (generation != _wallpaperPipelineGeneration)
                return;

            ApplyCombinedPipelineSnapshot(
                payload,
                state,
                buildKey,
                wallpaperIdentity,
                applyWallpaper);
        }
        catch
        {
            if (generation != _wallpaperPipelineGeneration)
                return;

            if (applyWallpaper)
            {
                ApplyWallpaperCrossfade(null, animate: false);
            }

            _glassBlurBrush.ImageSource = null;
            _glassBlurBrushB.ImageSource = null;
            _glassGlowBrush.ImageSource = null;
            _glassGlowBrushB.ImageSource = null;
        }
    }

    /// <summary>Blur, glow и обои из одного snapshot — синхронный crossfade, как на «Композиции».</summary>
    private void ApplyCombinedPipelineSnapshot(
        CompositionWallpaperPipelineBuilder.PipelineSnapshot payload,
        DwmCompositionCascadePreview.PreviewState state,
        CompositionWallpaperPipelineBuilder.BlurPipelineKey buildKey,
        string? wallpaperIdentity,
        bool applyWallpaper)
    {
        _lastGlassPipelineKey = buildKey;

        var animateBlur = _glassBlurBrush.ImageSource is not null || _glassBlurBrushB.ImageSource is not null;
        if (payload.BlurBgra is not null)
        {
            var blurBitmap = CreateWriteableBitmap(
                payload.Gaussian.Width,
                payload.Gaussian.Height,
                payload.BlurBgra);
            CrossfadeGlassBlur(blurBitmap, animateBlur);
        }

        if (state.Win7Mode && state.ShowGlow && payload.GlowBgra is not null)
        {
            var animateGlow = _glassGlowBrush.ImageSource is not null || _glassGlowBrushB.ImageSource is not null;
            var glowBitmap = CreateWriteableBitmap(
                payload.Gaussian.Width,
                payload.Gaussian.Height,
                payload.GlowBgra);
            CrossfadeGlassGlow(glowBitmap, animateGlow);
        }
        else
        {
            _glassGlowBrush.ImageSource = null;
            _glassGlowBrushB.ImageSource = null;
        }

        if (!applyWallpaper)
            return;

        _lastWallpaperIdentity = wallpaperIdentity;
        var displayBitmap = CreateWriteableBitmap(payload.DisplayWallpaper);
        ApplyWallpaperCrossfade(displayBitmap, animate: _hasWallpaperImage);
    }

    private void CrossfadeGlassBlur(ImageSource bitmap, bool animate)
    {
        var incomingIsA = !_glassBlurLayerAIsFront;
        var incomingBrush = incomingIsA ? _glassBlurBrush : _glassBlurBrushB;
        incomingBrush.ImageSource = bitmap;
        _glassBlurCrossfade.ShowFront(incomingIsA, animate);
        _glassBlurLayerAIsFront = incomingIsA;
    }

    private void CrossfadeGlassGlow(ImageSource bitmap, bool animate)
    {
        var incomingIsA = !_glassGlowLayerAIsFront;
        var incomingBrush = incomingIsA ? _glassGlowBrush : _glassGlowBrushB;
        incomingBrush.ImageSource = bitmap;
        _glassGlowCrossfade.ShowFront(incomingIsA, animate);
        _glassGlowLayerAIsFront = incomingIsA;
    }

    private void ApplyWallpaperCrossfade(ImageSource? source, bool animate)
    {
        _wallpaperCrossfade.Apply(source, animate);
        _hasWallpaperImage = source is not null;
    }

    private static WriteableBitmap CreateWriteableBitmap(int width, int height, byte[] pixels)
    {
        var bitmap = new WriteableBitmap(width, height);
        using var stream = bitmap.PixelBuffer.AsStream();
        stream.Seek(0, SeekOrigin.Begin);
        stream.Write(pixels, 0, pixels.Length);
        return bitmap;
    }

    private static WriteableBitmap CreateWriteableBitmap(CompositionWallpaperPipelineBuilder.BlurBuffer buffer) =>
        CreateWriteableBitmap(buffer.Width, buffer.Height, buffer.Pixels);

    private static void SetBrushColor(SolidColorBrush brush, UiColor color)
    {
        if (brush.Color != color)
            brush.Color = color;
    }

    private static UiColor ToColor(uint argb) =>
        UiColor.FromArgb(
            (byte)((argb >> 24) & 0xFF),
            (byte)((argb >> 16) & 0xFF),
            (byte)((argb >> 8) & 0xFF),
            (byte)(argb & 0xFF));

    private static uint AlphaToPercent(byte alpha) => (uint)((alpha * 100u + 127u) / 255u);

    /// <summary>Обновляет слой обоев и стекло (триггер: ImageColor в реестре Desktop).</summary>
    public void RefreshWallpaperLayer()
    {
        var identity = CurrentWallpaperResolver.TryGetWallpaperImageColorIdentity();
        if (string.Equals(_lastWallpaperIdentity, identity, StringComparison.OrdinalIgnoreCase))
            return;

        var wallpaperPath = CurrentWallpaperResolver.TryGetDesktopWallpaperPath();
        SetLayerTooltip(WallpaperLayer, "Stylization_Layer_Wallpaper", wallpaperPath);

        _lastGlassPipelineKey = null;
        var generation = ++_wallpaperPipelineGeneration;
        _ = RefreshWallpaperPipelineAsync(wallpaperPath, identity, generation, applyWallpaper: true);
    }

    /// <summary>Обновляет только миниатюру атласа (триггер: смена визуального стиля).</summary>
    public void RefreshAtlasLayer(bool force = false)
    {
        var themeFilePath = TryGetCurrentThemeRegistryPathNormalized();
        if (!force
            && string.Equals(_lastThemeFilePath, themeFilePath, StringComparison.OrdinalIgnoreCase))
        {
            return;
        }

        _lastThemeFilePath = themeFilePath;

        var generation = ++_refreshGeneration;
        var atlasPath = NormalizeCustom(DwmRegistryStore.ReadEffectiveString(AtlasRegistryKey))
            ?? NormalizeCustom(DwmRegistryStore.ReadEffectiveString(LegacyAtlasRegistryKey));
        SetLayerTooltip(WindowLayer, "Stylization_Layer_Atlas", atlasPath);
        _ = GenerateWindowLayerAsync(generation, atlasPath, preferLiveThemeApi: force || string.IsNullOrWhiteSpace(atlasPath));
    }

    private static string? TryGetCurrentThemeRegistryPathNormalized()
    {
        if (!VisualStyleThemeManager.TryGetCurrentThemeRegistryFilePath(out var path)
            || string.IsNullOrWhiteSpace(path))
        {
            return null;
        }

        return path;
    }

    private async Task GenerateWindowLayerAsync(
        int generation,
        string? atlasPath,
        bool preferLiveThemeApi = false)
    {
        try
        {
            var bytes = await Task.Run(() =>
                ThemeImageCatalog.TryGenerateLayeredAtlasPreviewBytes(
                    atlasPath,
                    (uint)AtlasLayerWidth,
                    (uint)AtlasLayerHeight,
                    preferLiveThemeApi)).ConfigureAwait(true);

            if (generation != _refreshGeneration)
                return;

            var image = bytes is not null
                ? await ThemeImageCatalog.DecodeAtlasPreviewAsync(bytes).ConfigureAwait(true)
                : null;

            if (generation != _refreshGeneration)
                return;

            SetAtlasLayer(image);
        }
        catch
        {
            if (generation == _refreshGeneration)
                SetAtlasLayer(null);
        }
    }

    private void SetAtlasLayer(ImageSource? source)
    {
        WindowImage.Source = source;
        var visible = source is not null;
        WindowImage.Visibility = visible ? Visibility.Visible : Visibility.Collapsed;
        WindowFallback.Visibility = visible ? Visibility.Collapsed : Visibility.Visible;
    }

    private void SetReflectionTextureLayer(ImageSource? source)
    {
        ReflectionTextureBrush.ImageSource = source;
        var visible = source is not null;
        ReflectionTextureFill.Visibility = visible ? Visibility.Visible : Visibility.Collapsed;
        ReflectionFallback.Visibility = visible ? Visibility.Collapsed : Visibility.Visible;
    }

    private static void SetTextureLayer(Image image, FrameworkElement fallback, ImageSource? source)
    {
        image.Source = source;
        image.Visibility = source is not null ? Visibility.Visible : Visibility.Collapsed;
        fallback.Visibility = source is not null ? Visibility.Collapsed : Visibility.Visible;
    }

    private static void SetImageLayer(Image image, FrameworkElement? fallback, ImageSource? source)
    {
        image.Source = source;
        image.Visibility = source is not null ? Visibility.Visible : Visibility.Collapsed;
        if (fallback is not null)
            fallback.Visibility = source is not null ? Visibility.Collapsed : Visibility.Visible;
    }

    private static void SetLayerTooltip(FrameworkElement layer, string layerNameKey, string? customPath)
    {
        var name = GlassSettingsResources.Get(layerNameKey);
        var detail = !string.IsNullOrWhiteSpace(customPath)
            ? Path.GetFileName(customPath)
            : GlassSettingsResources.Get("Stylization_Layer_Default");
        GlassSettingsToolTipService.SetToolTip(layer, $"{name}: {detail}");
    }

    private static string? NormalizeCustom(string? raw)
    {
        if (string.IsNullOrWhiteSpace(raw))
            return null;

        try
        {
            var full = Path.GetFullPath(raw);
            return File.Exists(full) ? full : null;
        }
        catch
        {
            return null;
        }
    }
}
