namespace GlassSettings.Core.Dwm;

using GlassSettings.Core.Registry;

/// <summary>
/// Vista Blur tint миниатюры — порт OG RealizeWindowColorization (GlassType::Blur).
/// </summary>
public static class VistaGlassPreviewCompositor
{
    private const uint OgSentinelAutoCompat = 0xFFFFFFFDu;
    private const uint OgSentinelMsstyle = 0xFFFFFFFEu;
    private const uint OgSentinelTheme = 0xFFFFFFFFu;

    /// <summary>Effective tint ARGB для flat brush (active, !maximized, !opaque base).</summary>
    public static uint BuildActiveTintArgb(DwmRegistryHive hive, bool opaqueBlend)
    {
        var srcBgr = VistaGlassColorPolicy.ReadOgFactualActiveColorRgb(hive);
        var glassOpacity = VistaGlassColorPolicy.ReadOgFactualGlassOpacityPercent(hive) / 100f;
        var blendingOpacity = ResolveBlendingOpacityPercent(hive, active: true, maximized: false) / 100f;
        var (baseR, baseG, baseB, baseA) = ResolveBlendingBaseColor(hive, opaqueBlend, maximized: false);

        var srcR = BgrToComponent(srcBgr, 16);
        var srcG = BgrToComponent(srcBgr, 8);
        var srcB = BgrToComponent(srcBgr, 0);

        var (r, g, b, a) = RealizeVistaBlurTint(
            srcR,
            srcG,
            srcB,
            glassOpacity,
            blendingOpacity,
            baseR,
            baseG,
            baseB,
            baseA);

        return ToPickerArgb(r, g, b, a);
    }

    /// <summary>Effective tint ARGB для flat brush (inactive, !maximized, !opaque base).</summary>
    public static uint BuildInactiveTintArgb(DwmRegistryHive hive, bool opaqueBlend)
    {
        var inactiveRaw = DwmRegistryStore.ReadDwordForPage(hive, "ColorizationColorInactive");
        var srcBgr = inactiveRaw is { } inactive && IsFactualInactiveColor(inactive)
            ? inactive & 0x00FFFFFFu
            : VistaGlassColorPolicy.ReadOgFactualActiveColorRgb(hive);

        var glassOpacityPercent = DwmRegistryStore.ReadDwordForPage(hive, "GlassOpacityInactive") is { } opacityInactive
            ? Math.Min(opacityInactive, 100u)
            : VistaGlassColorPolicy.ReadOgFactualGlassOpacityPercent(hive);
        var glassOpacity = glassOpacityPercent / 100f;
        var blendingOpacity = ResolveBlendingOpacityPercent(hive, active: false, maximized: false) / 100f;
        var (baseR, baseG, baseB, baseA) = ResolveBlendingBaseColor(hive, opaqueBlend, maximized: false);

        var srcR = BgrToComponent(srcBgr, 16);
        var srcG = BgrToComponent(srcBgr, 8);
        var srcB = BgrToComponent(srcBgr, 0);

        var (r, g, b, a) = RealizeVistaBlurTint(
            srcR,
            srcG,
            srcB,
            glassOpacity,
            blendingOpacity,
            baseR,
            baseG,
            baseB,
            baseA);

        return ToPickerArgb(r, g, b, a);
    }

    private static bool IsFactualInactiveColor(uint raw) =>
        raw != 0xFFFFFFFEu && raw != 0xFFFFFFFFu;

    /// <summary>OG GlassKernel::RealizeWindowColorization, ветка Blur.</summary>
    public static (float R, float G, float B, float A) RealizeVistaBlurTint(
        float srcR,
        float srcG,
        float srcB,
        float srcGlassOpacity,
        float colorizationOpacity,
        float baseR,
        float baseG,
        float baseB,
        float baseA)
    {
        var a = Math.Clamp(srcGlassOpacity * colorizationOpacity, 0f, 1f);
        var r = srcR;
        var g = srcG;
        var b = srcB;

        var outR = (1f - a) * baseR * baseA + r * a;
        var outG = (1f - a) * baseG * baseA + g * a;
        var outB = (1f - a) * baseB * baseA + b * a;
        var outA = (1f - a) * baseA + a;

        if (outA > 0f)
        {
            outR /= outA;
            outG /= outA;
            outB /= outA;
        }

        return (outR, outG, outB, outA);
    }

    private static (float R, float G, float B, float A) ResolveBlendingBaseColor(
        DwmRegistryHive hive,
        bool opaqueBlend,
        bool maximized)
    {
        if (opaqueBlend)
        {
            var opaqueBgr = DwmRegistryStore.ReadDwordForPage(hive, "ColorizationOpaqueBlendColor") ?? 0x00DFDFDFu;
            return (BgrToComponent(opaqueBgr, 16), BgrToComponent(opaqueBgr, 8), BgrToComponent(opaqueBgr, 0), 1f);
        }

        if (maximized)
        {
            var maxBgr = DwmRegistryStore.ReadDwordForPage(hive, "ColorizationOpaqueBlendColorMaximized");
            if (maxBgr is { } value && ((value >> 24) & 0xFF) != 0)
            {
                var alpha = ((value >> 24) & 0xFF) / 255f;
                return (BgrToComponent(value, 16), BgrToComponent(value, 8), BgrToComponent(value, 0), alpha);
            }
        }

        return (0f, 0f, 0f, 0f);
    }

    private static uint ResolveBlendingOpacityPercent(DwmRegistryHive hive, bool active, bool maximized)
    {
        var key = active
            ? maximized ? "ColorizationBlendingOpacityMaximized" : "ColorizationBlendingOpacity"
            : maximized ? "ColorizationBlendingOpacityInactiveMaximized" : "ColorizationBlendingOpacityInactive";

        var raw = DwmRegistryStore.ReadDwordForPage(hive, key) ?? OgSentinelAutoCompat;
        return ResolveBlendingOpacitySentinel(raw, active: active, maximized: maximized);
    }

    /// <summary>OG GlassKernel::Update — ColorizationBlendingOpacity* sentinels для Vista Blur.</summary>
    private static uint ResolveBlendingOpacitySentinel(uint raw, bool active, bool maximized)
    {
        if (raw == OgSentinelAutoCompat || raw == OgSentinelMsstyle)
            return active && !maximized ? 100u : active ? 100u : 100u;

        if (raw == OgSentinelTheme)
            return 100u;

        return Math.Min(raw, 100u);
    }

    private static float BgrToComponent(uint bgr, int shift) =>
        ((bgr >> shift) & 0xFF) / 255f;

    private static uint ToPickerArgb(float r, float g, float b, float a)
    {
        var alpha = (byte)Math.Clamp(Math.Round(a * 255f), 0, 255);
        var ri = (byte)Math.Clamp(Math.Round(r * 255f), 0, 255);
        var gi = (byte)Math.Clamp(Math.Round(g * 255f), 0, 255);
        var bi = (byte)Math.Clamp(Math.Round(b * 255f), 0, 255);
        return ((uint)alpha << 24) | ((uint)ri << 16) | ((uint)gi << 8) | bi;
    }
}
