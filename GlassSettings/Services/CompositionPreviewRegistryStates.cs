using GlassSettings.Controls;
using GlassSettings.Core.Dwm;
using GlassSettings.Core.Registry;

namespace GlassSettings.Services;

/// <summary>PreviewState Vista/Win7 из runtime-effective реестра (HKCU → HKLM) — prewarm и refresh превью.</summary>
internal static class CompositionPreviewRegistryStates
{
    private const uint ThemeColorSentinel = 0xFFFFFFFFu;
    private const uint AutoColorSentinel = 0xFFFFFFFEu;
    private const uint BlurDeviationDefault = 30;
    private const int BlurRadiusMin = 0;
    private const int BlurRadiusMax = 30;
    private const int BlurRadiusD3DValue = 3;

    public static (DwmCompositionCascadePreview.PreviewState Vista, DwmCompositionCascadePreview.PreviewState Win7)
        BuildFromRuntimeEffective()
    {
        var opaque = (DwmRegistryStore.ReadEffectiveDword("ColorizationOpaqueBlend") ?? 0) != 0;
        var blurBalancePercent = Win7GlassColorPolicy.ReadRuntimeFactualBlurBalancePercent();
        var d3d = (DwmRegistryStore.ReadEffectiveDword("UseDirect3DRendering") ?? 0) != 0;
        var deviation = DwmRegistryStore.ReadEffectiveDword("BlurDeviation") ?? BlurDeviationDefault;
        var blurRadius = d3d
            ? BlurRadiusD3DValue
            : ClampBlurRadius(BlurDeviationToRadius(Math.Min(deviation, 100u)));
        var wallpaper = CurrentWallpaperResolver.TryGetDesktopWallpaperPath();

        var vistaActiveColor = VistaGlassColorPolicy.ReadRuntimeFactualActiveColorRgb();
        var vistaActivePercent = VistaGlassColorPolicy.ReadRuntimeFactualGlassOpacityPercent();
        var vistaActiveTint = DwmCaptionColorHelper.ComposePickerArgbFromDwmBgr(vistaActiveColor, vistaActivePercent);
        var showSplit = VistaGlassColorPolicy.HasRuntimeInactiveOverrideKey();
        var vistaInactiveTint = ResolveVistaInactiveTintArgb(opaque);

        var vistaState = new DwmCompositionCascadePreview.PreviewState(
            wallpaper,
            vistaActiveTint,
            Win7Mode: false,
            GlowArgb: 0,
            ShowGlow: false,
            blurBalancePercent,
            opaque,
            blurRadius,
            Win7TintBgr: 0,
            Win7ColorBalancePercent: 0,
            Win7AfterglowBgr: 0,
            Win7AfterglowBalancePercent: 0,
            showSplit,
            vistaInactiveTint);

        var activeColor = Win7GlassColorPolicy.ReadRuntimeFactualActiveColorRgb();
        var activePercent = Win7GlassColorPolicy.ReadRuntimeFactualActiveBalancePercent();
        var win7Tint = DwmCaptionColorHelper.ComposePickerArgbFromDwmBgr(activeColor, activePercent);
        var (glowColor, glowPercent) = Win7GlassColorPolicy.ResolveRuntimePreviewAfterglow();
        var glowArgb = Win7GlassColorPolicy.ToPickerArgbFromBgr(glowColor, glowPercent);

        var win7State = new DwmCompositionCascadePreview.PreviewState(
            wallpaper,
            win7Tint,
            Win7Mode: true,
            glowArgb,
            ShowGlow: true,
            blurBalancePercent,
            opaque,
            blurRadius,
            activeColor & 0x00FFFFFFu,
            Math.Min(activePercent, 100u),
            glowColor & 0x00FFFFFFu,
            Math.Min(glowPercent, 100u),
            ShowInactiveSplit: false,
            InactiveTintArgb: 0);

        return (vistaState, win7State);
    }

    private static uint ResolveVistaInactiveTintArgb(bool opaqueBlend)
    {
        var inactiveValue = DwmRegistryStore.ReadEffectiveDword("ColorizationColorInactive");
        var hasFactual = inactiveValue is { } v && v != AutoColorSentinel && v != ThemeColorSentinel;
        if (hasFactual)
            return DwmCaptionColorHelper.ComposePickerArgbFromDwmBgr(
                inactiveValue!.Value,
                ReadInactiveOpacityPercent(hasFactual));

        return VistaGlassPreviewCompositor.BuildInactiveTintArgb(DwmRegistryHive.Hkcu, opaqueBlend);
    }

    private static uint ReadInactiveOpacityPercent(bool hasInactive)
    {
        if (hasInactive && DwmRegistryStore.ReadEffectiveDword("GlassOpacityInactive") is { } inactive)
            return Math.Min(inactive, 100u);

        return VistaGlassColorPolicy.HasRuntimeGlassOpacityKey()
            ? VistaGlassColorPolicy.ReadRuntimeFactualGlassOpacityPercent()
            : DwmColorRegistry.DefaultGlassOpacityPercent;
    }

    private static int BlurDeviationToRadius(uint deviation)
    {
        var value = ((int)deviation * 3 + 5) / 10;
        return ClampBlurRadius(value);
    }

    private static int ClampBlurRadius(int radius) => Math.Clamp(radius, BlurRadiusMin, BlurRadiusMax);
}
